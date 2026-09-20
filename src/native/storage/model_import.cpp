#include "model_import.h"
#include "audio/processor.hpp"
#include "model_catalog.h"
#include "flash_storage.h"
#include "usb_audio.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "esp_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <memory>

namespace {
using Model = coyopedal::pedal::Processor::Model;
bool prepare_import(std::uint8_t* data, Model::TuningReport& report, char* error,
                    std::size_t capacity) {
    Model::TuningOptions options;
    options.max_trials = 96U;
    options.probe_gain = 1.0F;
    options.target_peak = 0.001; // -60 dBFS on the shared validation corpus.
    return Model::prepare_tuned(data, Model::kFileSize, data + Model::kFileSize, options, report,
                                error, capacity);
}
void tuning_headers(httpd_req_t* r, const Model::TuningReport& report, char (&value)[256]) {
    std::snprintf(
        value, sizeof value,
        "{\"trials\":%u,\"invalid_candidates\":%u,\"peak_dbfs\":%.6f,\"baseline_peak_dbfs\":%.6f,"
        "\"measured_ranges\":%s,\"float_block\":%u,\"native_split\":%u,\"parallel_float\":%s}",
        unsigned(report.trials), unsigned(report.invalid_candidates),
        20.0 * std::log10(std::max(report.peak, 1.0e-30)),
        20.0 * std::log10(std::max(report.baseline_peak, 1.0e-30)),
        report.measured_residual ? "true" : "false", unsigned(report.float_block_frames),
        unsigned(report.native_split_layer), report.parallel_float ? "true" : "false");
    httpd_resp_set_hdr(r, "X-CoyoPedal-Tuning", value);
}

// Immutable, independently committed slots. A power cut while writing a new
// slot cannot erase the index or payload of an existing imported model.
constexpr unsigned kSector = 4096, kSlot = 13 * kSector, kHeader = 128;
struct Free {
    void operator()(void* p) const {
        heap_caps_free(p);
    }
};
using Buffer = std::unique_ptr<std::uint8_t, Free>;
const esp_partition_t* partition() {
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                    static_cast<esp_partition_subtype_t>(0x42), "user_models");
}
std::uint32_t u32(const std::uint8_t* p) {
    return p[0] | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) |
           (std::uint32_t(p[3]) << 24);
}
void put(std::uint8_t* p, std::uint32_t v) {
    for (unsigned i = 0; i < 4; ++i)
        p[i] = v >> (i * 8);
}
bool valid_header(const std::uint8_t* h) {
    return !std::memcmp(h, "TSM1", 4) && u32(h + 4) == Model::kPreparedFileSize &&
           u32(h + 124) == esp_crc32_le(0, h, 124) && h[12] && h[35] == 0 && h[67] == 0;
}
esp_err_t fail(httpd_req_t* r, const char* status, const char* message) {
    httpd_resp_set_status(r, status);
    return httpd_resp_sendstr(r, message);
}
struct FlashJob {
    const esp_partition_t* partition;
    unsigned slot;
    const std::uint8_t* data;
    const std::uint8_t* header;
    SemaphoreHandle_t done;
    bool ok = false;
};
void write_slot(void* argument) {
    auto* j = static_cast<FlashJob*>(argument);
    // Flash writes disable caches: both this task's stack and each source
    // chunk must be internal RAM, even though the HTTP model lives in PSRAM.
    std::array<std::uint8_t, 256> chunk{}, verify{};
    bool ok = esp_partition_erase_range(j->partition, j->slot, kSlot) == ESP_OK;
    for (unsigned at = 0; ok && at < Model::kPreparedFileSize; at += chunk.size()) {
        const unsigned n = std::min<unsigned>(chunk.size(), Model::kPreparedFileSize - at);
        std::memcpy(chunk.data(), j->data + at, n);
        ok = esp_partition_write(j->partition, j->slot + kHeader + at, chunk.data(), n) == ESP_OK &&
             esp_partition_read(j->partition, j->slot + kHeader + at, verify.data(), n) == ESP_OK &&
             !std::memcmp(chunk.data(), verify.data(), n);
    }
    if (ok) {
        std::memcpy(chunk.data(), j->header, kHeader);
        ok = esp_partition_write(j->partition, j->slot, chunk.data(), kHeader) == ESP_OK &&
             esp_partition_read(j->partition, j->slot, verify.data(), kHeader) == ESP_OK &&
             !std::memcmp(chunk.data(), verify.data(), kHeader);
    }
    j->ok = ok;
    xSemaphoreGive(j->done);
    vTaskDelete(nullptr);
}
bool commit_slot(const esp_partition_t* p, unsigned slot, const std::uint8_t* data,
                 const std::uint8_t* header) {
    auto done = xSemaphoreCreateBinary();
    if (!done)
        return false;
    FlashJob job{p, slot, data, header, done};
    if (xTaskCreatePinnedToCore(write_slot, "model_flash", 4096, &job, 5, nullptr, 0) != pdPASS) {
        vSemaphoreDelete(done);
        return false;
    }
    // Never time out and free the source underneath the writer.
    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);
    return job.ok;
}
bool get_id(httpd_req_t* r, char (&id)[24]) {
    char query[96]{};
    if (httpd_req_get_url_query_str(r, query, sizeof query) != ESP_OK ||
        httpd_query_key_value(query, "id", id, sizeof id) != ESP_OK || !id[0])
        return false;
    for (const unsigned char c : id) {
        if (!c)
            break;
        if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9') &&
            c != '-' && c != '_')
            return false;
    }
    return true;
}
} // namespace

// Called only during the existing library scan, on the UI owner task. A newly
// imported slot becomes visible after reboot; HTTP never mutates the UI tables.
extern "C" void coyopedal_s3_read_imported_models() {
    const auto* p = partition();
    if (!p)
        return;
    std::array<std::uint8_t, kHeader> h{};
    for (unsigned at = 0; at + kSlot <= p->size; at += kSlot) {
        if (esp_partition_read(p, at, h.data(), h.size()) != ESP_OK || !valid_header(h.data()))
            continue;
        if (coyopedal_model_count >= COYOPEDAL_MODEL_MAX)
            break;
        auto& m = coyopedal_models[coyopedal_model_count++];
        m = {};
        std::memcpy(m.id, h.data() + 12, sizeof m.id - 1);
        std::memcpy(m.name, h.data() + 36, sizeof m.name - 1);
        m.offset = COYOPEDAL_FLASH_USER_MODELS_BASE + at + kHeader;
        m.size = Model::kPreparedFileSize;
        m.user_model = true;
    }
}

esp_err_t coyopedal_model_import(httpd_req_t* r) {
    char id[24]{};
    if (!get_id(r, id))
        return fail(r, "400 Bad Request", "id must be 1-23 ASCII letters, digits, - or _\n");
    for (unsigned i = 0; i < coyopedal_model_count; ++i)
        if (!std::strcmp(coyopedal_models[i].id, id))
            return fail(r, "409 Conflict", "model id already exists in library\n");
    const bool raw = r->content_len == Model::kFileSize;
    if (!raw && r->content_len != Model::kPreparedFileSize)
        return fail(r, "400 Bad Request", "expected raw or prepared A2-Full NAMB\n");
    const auto* p = partition();
    if (!p)
        return fail(r, "503 Service Unavailable", "no user_models partition\n");
    std::array<std::uint8_t, kHeader> h{};
    if (esp_partition_read(p, 0, h.data(), h.size()) != ESP_OK)
        return fail(r, "500 Internal Server Error", "cannot read model storage\n");
    unsigned slot = p->size;
    for (unsigned at = 0; at + kSlot <= p->size; at += kSlot) {
        if (esp_partition_read(p, at, h.data(), h.size()) != ESP_OK)
            return fail(r, "500 Internal Server Error", "cannot read model slot\n");
        if (valid_header(h.data())) {
            if (!std::strcmp(reinterpret_cast<char*>(h.data() + 12), id))
                return fail(r, "409 Conflict", "model id already imported\n");
        } else if (slot == p->size)
            slot = at;
    }
    if (slot == p->size)
        return fail(r, "409 Conflict", "user model storage full (9 slots)\n");
    Buffer data(static_cast<std::uint8_t*>(
        heap_caps_malloc(Model::kPreparedFileSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
    if (!data)
        return fail(r, "503 Service Unavailable", "model staging allocation failed\n");
    for (unsigned at = 0; at < r->content_len;) {
        const int got =
            httpd_req_recv(r, reinterpret_cast<char*>(data.get() + at), r->content_len - at);
        if (got <= 0)
            return fail(r, "400 Bad Request", "incomplete upload\n");
        at += got;
    }
    if (!raw && !Model::validate_prepared(data.get(), r->content_len))
        return fail(r, "422 Unprocessable Entity", "invalid prepared model checksum or version\n");
    if (!usb_audio_begin_model_update())
        return fail(r, "409 Conflict", "audio pipeline busy\n");
    struct Resume {
        ~Resume() {
            usb_audio_end_model_update();
        }
    } resume;
    char error[128]{};
    const auto start = esp_timer_get_time();
    Model::TuningReport tuning;
    if (raw && !prepare_import(data.get(), tuning, error, sizeof error))
        return fail(r, "422 Unprocessable Entity", error);
    if (!Model::validate_prepared(data.get(), Model::kPreparedFileSize))
        return fail(r, "422 Unprocessable Entity", "invalid prepared parameters\n");
    const double seconds = (esp_timer_get_time() - start) / 1000000.0;
    h.fill(0);
    std::memcpy(h.data(), "TSM1", 4);
    put(h.data() + 4, Model::kPreparedFileSize);
    put(h.data() + 8, esp_crc32_le(0, data.get(), Model::kPreparedFileSize));
    std::memcpy(h.data() + 12, id, sizeof id);
    char name[32]{};
    if (httpd_req_get_hdr_value_str(r, "X-Model-Name", name, sizeof name) != ESP_OK || !name[0])
        std::snprintf(name, sizeof name, "%s", id);
    std::memcpy(h.data() + 36, name, sizeof name);
    put(h.data() + 124, esp_crc32_le(0, h.data(), 124));
    const bool wrote = commit_slot(p, slot, data.get(), h.data());
    if (!wrote)
        return fail(r, "500 Internal Server Error", "model flash write/verification failed\n");
    char response[256];
    std::snprintf(response, sizeof response,
                  "{\"id\":\"%s\",\"stored\":true,\"method\":\"%s\",\"conversion_seconds\":%.6f,"
                  "\"bytes\":%u,\"reboot_to_refresh_library\":true}\n",
                  id, raw ? "device-sweep-tuned" : "prepared", seconds,
                  unsigned(Model::kPreparedFileSize));
    char tuning_header[256]{};
    if (raw)
        tuning_headers(r, tuning, tuning_header);
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, response);
}

esp_err_t coyopedal_model_download(httpd_req_t* r) {
    char id[24]{};
    if (!get_id(r, id))
        return fail(r, "400 Bad Request", "supply model id\n");
    const auto* p = partition();
    if (!p)
        return fail(r, "404 Not Found", "no model storage\n");
    std::array<std::uint8_t, kHeader> h{};
    Buffer data(static_cast<std::uint8_t*>(
        heap_caps_malloc(Model::kPreparedFileSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
    if (!data)
        return fail(r, "503 Service Unavailable", "allocation failed\n");
    for (unsigned at = 0; at + kSlot <= p->size; at += kSlot) {
        if (esp_partition_read(p, at, h.data(), h.size()) != ESP_OK || !valid_header(h.data()) ||
            std::strcmp(reinterpret_cast<char*>(h.data() + 12), id))
            continue;
        if (esp_partition_read(p, at + kHeader, data.get(), Model::kPreparedFileSize) != ESP_OK ||
            esp_crc32_le(0, data.get(), Model::kPreparedFileSize) != u32(h.data() + 8))
            return fail(r, "500 Internal Server Error", "stored model checksum mismatch\n");
        httpd_resp_set_type(r, "application/octet-stream");
        return httpd_resp_send(r, reinterpret_cast<char*>(data.get()), Model::kPreparedFileSize);
    }
    return fail(r, "404 Not Found", "model not found\n");
}
