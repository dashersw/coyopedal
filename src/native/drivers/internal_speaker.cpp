#include "internal_speaker.h"
#include "speaker_buffer.hpp"
#include <atomic>
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#define GEA_AUDIO_DRIVER_INTERNAL 1
#include "audio.h"

namespace {
std::atomic<bool> requested{}, active{}, save_failed{};
std::atomic<std::uint32_t> revision{};
std::atomic<std::uint32_t> dropped_frames{};
std::atomic<int> status{}; // off, starting, playing, failed
TaskHandle_t worker{};
bool audio_mode{};
SpeakerBuffer buffer;
void state(int next) {
    if (status.exchange(next) != next)
        revision.fetch_add(1);
}
#if GEA_BOARD_HAS_SPEAKER
void run(void*) {
    using gea::platform::audio::OutputDriver;
    bool opened = false, primed = false;
    int saved = -1;
    std::uint32_t blocks = 0, underruns = 0;
    std::uint64_t render_us = 0, write_us = 0;
    TickType_t last_report = xTaskGetTickCount();
    std::int16_t pcm[128]{};
    for (;;) {
        const bool enabled = requested.load(std::memory_order_acquire);
        if (saved != static_cast<int>(enabled)) {
            nvs_handle_t nvs{};
            esp_err_t err = nvs_open("pedal_settings", NVS_READWRITE, &nvs);
            if (err == ESP_OK) {
                err = nvs_set_u8(nvs, "speaker", enabled);
                if (err == ESP_OK)
                    err = nvs_commit(nvs);
                nvs_close(nvs);
            }
            saved = enabled;
            if (save_failed.exchange(err != ESP_OK) != (err != ESP_OK))
                revision.fetch_add(1);
            if (err != ESP_OK) {
                ESP_LOGE("speaker", "Could not save setting: %s", esp_err_to_name(err));
            }
        }
        if (!enabled) {
            active.store(false, std::memory_order_release);
            if (opened)
                OutputDriver::close();
            opened = primed = false;
            buffer.clear();
            state(0);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (!opened) {
            state(1);
            if (!buffer.samples)
                buffer.samples = static_cast<std::int16_t*>(
                    heap_caps_calloc(SpeakerBuffer::capacity, sizeof(std::int16_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            OutputDriver::setVolume(100);
            opened = buffer.samples && OutputDriver::open(48000, 1, 16);
            if (!opened) {
                state(3);
                // Do not retry allocations or codec setup inside the audio deadline.
                while (requested.load())
                    vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            buffer.clear();
            active.store(true, std::memory_order_release);
            ESP_LOGI("speaker", "48 kHz mono monitor enabled alongside USB output");
            state(2);
        }
        const auto render_start = esp_timer_get_time();
        if (!primed && buffer.available() >= 384)
            primed = true;
        if (!primed || !buffer.render(pcm, 128)) {
            if (primed)
                ++underruns;
            std::fill_n(pcm, 128, 0);
            primed = false;
        }
        const auto write_start = esp_timer_get_time();
        render_us += write_start - render_start;
        if (!OutputDriver::write(pcm, 128, 20)) {
            active.store(false, std::memory_order_release);
            OutputDriver::close();
            opened = false;
            state(3);
            while (requested.load())
                vTaskDelay(pdMS_TO_TICKS(20));
        }
        write_us += esp_timer_get_time() - write_start;
        ++blocks;
        const auto now = xTaskGetTickCount();
        if (now - last_report >= pdMS_TO_TICKS(5000)) {
            ESP_LOGI("speaker",
                     "blocks=%lu underruns=%lu drops=%lu queued=%u render/write=%llu/%lluus",
                     static_cast<unsigned long>(blocks), static_cast<unsigned long>(underruns),
                     static_cast<unsigned long>(dropped_frames.exchange(0)),
                     static_cast<unsigned>(buffer.available()),
                     static_cast<unsigned long long>(render_us / blocks),
                     static_cast<unsigned long long>(write_us / blocks));
            blocks = underruns = 0;
            render_us = write_us = 0;
            last_report = now;
        }
    }
}
#endif
} // namespace
bool internal_speaker_available() {
#if GEA_BOARD_HAS_SPEAKER
    return true;
#else
    return false;
#endif
}
void internal_speaker_init() {
    nvs_handle_t nvs{};
    std::uint8_t value{};
    if (nvs_open("pedal_settings", NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, "speaker", &value);
        nvs_close(nvs);
    }
    requested.store(internal_speaker_available() && value != 0);
}
void internal_speaker_start() {
    audio_mode = true;
    internal_speaker_set_enabled(requested.load());
}
void internal_speaker_set_enabled(bool enabled) {
    if (!internal_speaker_available())
        return;
    requested.store(enabled, std::memory_order_release);
    revision.fetch_add(1);
#if GEA_BOARD_HAS_SPEAKER
    if (audio_mode && !worker && enabled) {
        state(1);
        // Core 0 has more spare time; USB (20) and DSP stage A (19) stay ahead.
        if (xTaskCreatePinnedToCoreWithCaps(run, "speaker", 4096, nullptr, 18, &worker, 0,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            worker = nullptr;
            ESP_LOGE("speaker", "Could not create speaker worker");
            state(3);
        }
    }
#endif
}
bool internal_speaker_enabled() {
    return requested.load();
}
std::uint32_t internal_speaker_revision() {
    return revision.load();
}
const char* internal_speaker_status() {
    if (!internal_speaker_available())
        return "Not available on this board";
    if (save_failed.load())
        return "Could not save speaker setting";
    switch (status.load()) {
    case 1:
        return "Starting speaker...";
    case 2:
        return "Speaker + USB output";
    case 3:
        return "Speaker failed; USB output active";
    default:
        return "USB output only";
    }
}
IRAM_ATTR void internal_speaker_submit(const std::uint64_t* frames, std::size_t count) {
    if (active.load(std::memory_order_acquire)) {
        const auto dropped = count - buffer.push(frames, count);
        if (dropped)
            dropped_frames.fetch_add(dropped, std::memory_order_relaxed);
    }
}
