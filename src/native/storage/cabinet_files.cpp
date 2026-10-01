#include "cabinet_files.h"
#include "sd_models.h"
#include "remote_service.h"
#include "audio/cabinet.h"
#include "audio/ir_wav.hpp"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "psa/crypto.h"
#include "cJSON.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

namespace {
constexpr unsigned kMaxBytes = 1024 * 1024;
struct Free {
    void operator()(void* p) const {
        heap_caps_free(p);
    }
};
using Buffer = std::unique_ptr<unsigned char, Free>;
esp_err_t fail(httpd_req_t* request, const char* status, const char* message) {
    httpd_resp_set_status(request, status);
    return httpd_resp_sendstr(request, message);
}
bool ready(httpd_req_t* request) {
    if (coyopedal_remote_audio_mode()) {
        fail(request, "409 Conflict", "SD transfers require maintenance mode\n");
        return false;
    }
    if (!pedalboard_sd_ready()) {
        fail(request, "503 Service Unavailable", "SD card is unavailable\n");
        return false;
    }
    return true;
}
bool filename(httpd_req_t* request, char (&name)[COYOPEDAL_IR_PATH_MAX]) {
    if (httpd_req_get_hdr_value_str(request, "X-CoyoPedal-Filename", name, sizeof name) != ESP_OK)
        return false;
    const size_t n = std::strlen(name);
    if (n < 5 || name[0] == '.' || strcasecmp(name + n - 4, ".wav"))
        return false;
    for (size_t i = 0; i < n; ++i)
        if (!((name[i] >= 'A' && name[i] <= 'Z') || (name[i] >= 'a' && name[i] <= 'z') ||
              (name[i] >= '0' && name[i] <= '9') || name[i] == '-' || name[i] == '_' ||
              name[i] == '.'))
            return false;
    return true;
}
} // namespace

esp_err_t pedalboard_ir_upload(httpd_req_t* request) {
    if (!ready(request))
        return ESP_OK;
    char name[COYOPEDAL_IR_PATH_MAX]{}, expected[65]{};
    if (!filename(request, name) || request->content_len < 44 || request->content_len > kMaxBytes ||
        httpd_req_get_hdr_value_str(request, "X-CoyoPedal-SHA256", expected, sizeof expected) !=
            ESP_OK ||
        std::strlen(expected) != 64)
        return fail(request, "400 Bad Request",
                    "Use a WAV filename, SHA256 and 44..1048576 bytes\n");
    if (mkdir("/sdcard/ir", 0755) != 0 && errno != EEXIST)
        return fail(request, "500 Internal Server Error", "Could not create /ir\n");
    char path[COYOPEDAL_IR_PATH_MAX + 16]{}, temp[COYOPEDAL_IR_PATH_MAX + 48]{};
    std::snprintf(path, sizeof path, "/sdcard/ir/%s", name);
    struct stat st{};
    if (stat(path, &st) == 0)
        return fail(request, "409 Conflict", "IR already exists; existing file preserved\n");
    if (errno != ENOENT)
        return fail(request, "500 Internal Server Error", "Could not inspect destination\n");
    Buffer data(static_cast<unsigned char*>(
        heap_caps_malloc(request->content_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
    if (!data)
        return fail(request, "500 Internal Server Error", "No upload memory\n");
    const int64_t deadline = esp_timer_get_time() + 30000000;
    unsigned at = 0;
    while (at < request->content_len) {
        const int n = httpd_req_recv(request, reinterpret_cast<char*>(data.get() + at),
                                     std::min<unsigned>(4096, request->content_len - at));
        if (n == HTTPD_SOCK_ERR_TIMEOUT && esp_timer_get_time() < deadline)
            continue;
        if (n <= 0 || esp_timer_get_time() > deadline)
            return fail(request, "408 Request Timeout",
                        "IR upload incomplete; no file installed\n");
        at += n;
    }
    std::array<unsigned char, 32> digest{};
    size_t digest_size{};
    char sha[65]{};
    if (psa_hash_compute(PSA_ALG_SHA_256, data.get(), at, digest.data(), digest.size(),
                         &digest_size) != PSA_SUCCESS ||
        digest_size != digest.size())
        return fail(request, "500 Internal Server Error", "Could not checksum IR\n");
    for (unsigned i = 0; i < digest.size(); ++i)
        std::snprintf(sha + i * 2, 3, "%02x", digest[i]);
    if (strcasecmp(sha, expected))
        return fail(request, "400 Bad Request", "IR checksum mismatch; no file installed\n");
    std::snprintf(temp, sizeof temp, "/sdcard/ir/.upload-%lld.tmp",
                  static_cast<long long>(esp_timer_get_time()));
    FILE* file = fopen(temp, "w+b");
    if (!file)
        return fail(request, "500 Internal Server Error", "Could not write SD card\n");
    bool ok =
        fwrite(data.get(), 1, at, file) == at && fflush(file) == 0 && fseek(file, 0, SEEK_SET) == 0;
    data.reset();
    Buffer taps(
        static_cast<unsigned char*>(heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
    unsigned frames{};
    char error[96]{};
    const bool valid = ok && taps &&
                       pedalboard_ir_read_wav(file, reinterpret_cast<float*>(taps.get()), frames,
                                              error, sizeof error);
    if (valid)
        ok = fsync(fileno(file)) == 0;
    ok = fclose(file) == 0 && ok;
    if (!valid || !ok) {
        unlink(temp);
        return fail(request, valid ? "500 Internal Server Error" : "400 Bad Request",
                    error[0] ? error : "Could not validate or sync IR; no file installed\n");
    }
    if (stat(path, &st) == 0 || rename(temp, path) != 0) {
        unlink(temp);
        return fail(request, "409 Conflict", "Could not install IR; existing file preserved\n");
    }
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "X-CoyoPedal-SHA256", sha);
    char reply[256]{};
    std::snprintf(reply, sizeof reply,
                  "{\"name\":\"%s\",\"bytes\":%u,\"taps\":%u,\"sha256\":\"%s\"}\n", name, at,
                  frames, sha);
    return httpd_resp_sendstr(request, reply);
}

esp_err_t pedalboard_ir_download(httpd_req_t* request) {
    if (!ready(request))
        return ESP_OK;
    char name[COYOPEDAL_IR_PATH_MAX]{}, path[COYOPEDAL_IR_PATH_MAX + 16]{};
    if (!filename(request, name))
        return fail(request, "400 Bad Request", "Use X-CoyoPedal-Filename with a WAV filename\n");
    std::snprintf(path, sizeof path, "/sdcard/ir/%s", name);
    FILE* file = fopen(path, "rb");
    if (!file)
        return fail(request, "404 Not Found", "IR not found\n");
    httpd_resp_set_type(request, "audio/wav");
    std::array<char, 1024> buffer{};
    esp_err_t result = ESP_OK;
    while (const size_t n = fread(buffer.data(), 1, buffer.size(), file)) {
        result = httpd_resp_send_chunk(request, buffer.data(), n);
        if (result != ESP_OK)
            break;
    }
    const bool read_error = ferror(file);
    fclose(file);
    if (result != ESP_OK || read_error)
        return ESP_FAIL;
    return httpd_resp_send_chunk(request, nullptr, 0);
}

esp_err_t pedalboard_ir_list(httpd_req_t* request) {
    if (!ready(request))
        return ESP_OK;
    pedalboard_cabinet_scan();
    cJSON* list = cJSON_CreateArray();
    if (!list)
        return fail(request, "500 Internal Server Error", "No catalogue memory\n");
    for (unsigned i = 0; i < pedalboard_cabinet_count(); ++i) {
        const char* path = pedalboard_cabinet_path(i);
        // This endpoint lists files transferable from SD, not embedded choices.
        if (std::strncmp(path, "Factory/", 8) == 0)
            continue;
        cJSON* name = cJSON_CreateString(path);
        if (!name || !cJSON_AddItemToArray(list, name)) {
            cJSON_Delete(name);
            cJSON_Delete(list);
            return fail(request, "500 Internal Server Error", "No catalogue memory\n");
        }
    }
    char* reply = cJSON_PrintUnformatted(list);
    cJSON_Delete(list);
    if (!reply)
        return fail(request, "500 Internal Server Error", "No catalogue memory\n");
    httpd_resp_set_type(request, "application/json");
    const esp_err_t result = httpd_resp_sendstr(request, reply);
    cJSON_free(reply);
    return result;
}
