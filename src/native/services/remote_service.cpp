#include "remote_service.h"
#include "model_import.h"

#if __has_include("remote_config.h")
#include "remote_config.h"
#else
#define COYOPEDAL_REMOTE_ENABLED 0
#define COYOPEDAL_REMOTE_MODE_AP 1
#define COYOPEDAL_REMOTE_DISABLE_USB_AUDIO 0
#define COYOPEDAL_REMOTE_WIFI_SSID ""
#define COYOPEDAL_REMOTE_WIFI_PASSWORD ""
#define COYOPEDAL_REMOTE_TOKEN ""
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "display.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_cache.h"
#include "esp_log.h"
#include "esp_log_write.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "connectivity/wifi_task_stack.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "psa/crypto.h"

#include "usb/usb_host.h"

#include "usb_audio.h"
#include "factory_presets.h"
#include "control.h"
#include "events.h"
#include "audio/effects.h"
#include "audio/usb_frame_processor.h"
#include "heap_map.h"

extern "C" void s3_v1_network_status(bool enabled, bool connected, const char* ssid,
                                     const char* detail) __attribute__((weak));

// Runs a bounded UI action on the Gea UI task (src/native/ui/task.cpp) and waits
// for it; 7 engages the pedal.
bool s3_v1_ui_mode(unsigned mode);
// Posts a Frame event to the scheduler, which is what a touch on the panel ends
// up doing. A control write on its own changes the sound and leaves the screen
// alone until something asks for a frame.
void s3_v1_ui_wake();
// The panel's own JSX-facing setter (src/native/ui/board_bridge.cpp). Key 4 is
// the screen index, which is what a tap that navigates ends up writing.
void pbSet(double key, double value);

extern "C" {
// Kept by the USB host controller interrupt (third_party/usb/src/hcd_dwc.c).
extern volatile std::uint32_t pedalboard_usb_isr_cycles __attribute__((weak));
extern volatile std::uint32_t pedalboard_usb_isr_count __attribute__((weak));

// The task watchdog's interrupt calls this when a watched task has not fed
// it in time, before it prints its report straight to the console. Audio
// mode keeps core 0 busy enough that its idle task can starve; the report
// never reaches the log ring, so the window counts the firings instead.
DRAM_ATTR volatile std::uint32_t g_task_wdt_firings;
void IRAM_ATTR esp_task_wdt_isr_user_handler(void) {
    g_task_wdt_firings += 1U;
}
}

namespace {

constexpr char kTag[] = "remote";
// What discovery answers with. It is not a board model: the firmware cannot
// read a model number off the hardware, and hard-coding one made every board
// that is not the AMOLED introduce itself as an AMOLED. What it can say
// truthfully is which build is running, and only one board property changes the
// build enough to matter to a host -- whether there is a panel to drive. The
// prefix is the family, and it is what a host should match on; the suffix is
// this build. A board that has earned its own name defines COYOPEDAL_DEVICE_NAME.
#ifndef COYOPEDAL_DEVICE_NAME
#if defined(GEA_EMBEDDED_NO_DISPLAY) && GEA_EMBEDDED_NO_DISPLAY
#define COYOPEDAL_DEVICE_NAME "coyopedal-headless"
#else
#define COYOPEDAL_DEVICE_NAME "coyopedal-amoled-2.06"
#endif
#endif
constexpr std::uint16_t kHttpPort = 8080;
constexpr std::uint16_t kDiscoveryPort = 32123;
// The ring lives in PSRAM, so capacity here is almost free, and it is the only
// record of a boot that had no serial port. 128 KiB holds a whole session,
// including the boot chatter before Wi-Fi comes up.
constexpr std::size_t kLogCapacity = 128U * 1024U;
constexpr UBaseType_t kServicePriority = 3;
// The socket blocks between requests, so this costs no steady-state CPU. At
// priority 19 an authenticated UDP status/pause request can run beside stage A
// without delaying the priority-20 USB host task. This remains reachable when
// the ordinary priority-3 HTTP server has no scheduler slack.
constexpr UBaseType_t kDiscoveryPriority = 19;
// The NAM stage on Core 1 is priority 22. Radio work gets only the slack it
// leaves, so an HTTP request cannot preempt the audio deadline.
constexpr UBaseType_t kWifiPriority = 18;
constexpr char kModeNamespace[] = "coyopedal_mode";
constexpr char kMaintenanceKey[] = "maintenance";
constexpr char kExclusiveAudioKey[] = "exclusive";
// This one-shot must survive an OTA to a different image. RTC_NOINIT symbols
// can move when their structs change, so the OTA validation return belongs in
// stable NVS instead.
constexpr char kReturnMaintenanceKey[] = "ota_return";

// The remote log is cold diagnostic state. Internal SRAM belongs to the native
// DSP tables; cached PSRAM is more than fast enough for the low-priority HTTP
// reader and vprintf mirror.
//
// NOINIT, not BSS: .ext_ram.bss is zeroed on every startup, and a reboot is
// exactly when the log matters. Every reason this board restarts (a panic, an
// OTA, the return from an EFFECT SOAK's exclusive-audio boot) would otherwise
// erase the evidence of the boot that caused it. PSRAM keeps its contents
// across a reset but holds garbage after power-on, so a magic word gates the
// carry-over and the byte count travels with the buffer.
constexpr std::uint32_t kLogRingMagic = 0x4C4F4752U; // 'LOGR'
// Cache-line aligned: esp_cache_msync rejects an unaligned address outright,
// and a rejected sync leaves the ring holding whatever the cache happened to
// evict -- which is why a window's closing lines used to arrive shredded.
alignas(64) EXT_RAM_NOINIT_ATTR char g_log[kLogCapacity];
EXT_RAM_NOINIT_ATTR std::uint64_t g_log_total;
EXT_RAM_NOINIT_ATTR std::uint32_t g_log_magic;

// Runs before the first line is written. Keeps a ring that survived a reset and
// starts a fresh one otherwise.
void adopt_or_reset_log_ring() {
    if (g_log_magic == kLogRingMagic && g_log_total <= UINT64_MAX / 2U) {
        return;
    }
    g_log_total = 0U;
    g_log_magic = kLogRingMagic;
}
// The ring lives in cached PSRAM. A restart does not write dirty cache lines
// back, so whatever was logged in the last moments before esp_restart() --
// exactly the lines a window's close writes -- would never reach the ring.
// Sync the ring on the way out.
void log_ring_writeback() {
    const esp_err_t synced = esp_cache_msync(g_log, kLogCapacity, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    if (synced != ESP_OK) {
        // Nothing else can report this: the report is what was just lost.
        ESP_EARLY_LOGE(kTag, "log ring writeback failed: %s", esp_err_to_name(synced));
    }
    (void)esp_cache_msync(const_cast<std::uint64_t*>(&g_log_total), sizeof g_log_total,
                          ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    (void)esp_cache_msync(const_cast<std::uint32_t*>(&g_log_magic), sizeof g_log_magic,
                          ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}
portMUX_TYPE g_log_lock = portMUX_INITIALIZER_UNLOCKED;
vprintf_like_t g_console_vprintf{};
httpd_handle_t g_server{};
std::uint8_t g_mac[6]{};
char g_network_name[33]{};
TaskHandle_t g_wifi_task{};
EventGroupHandle_t g_wifi_events{};
esp_netif_t* g_wifi_netif{};
bool g_wifi_initialized{};
std::atomic<bool> g_discovery_running{};
std::atomic<bool> g_mode_busy{};
TaskHandle_t g_mode_task{};
// Statically reserved, like mode_button_stack in main.cpp. This task is the only
// way back to maintenance mode once audio owns the box, so its stack cannot come
// from a heap the NAM arenas may have exhausted: a failed allocation would leave
// g_mode_task null and make every BOOT press a silent no-op.
//
// The task uses about 2,650 bytes across a full transition; 3584 keeps a 1.35x
// margin. Every byte beyond that is internal SRAM the arenas and the panel's
// QSPI bus need.
constexpr std::uint32_t kModeTaskStackBytes = 3584U;
//
// Internal does not have to mean DRAM. RTC fast memory is internal SRAM too:
// esp_task_stack_is_sane_cache_disabled() accepts it, xPortCheckValidTCBMem()
// accepts it, and both cores can reach it. It sits on the peripheral bus, which
// makes it the wrong home for anything the audio graph reads and exactly the
// right one for a stack that runs twice per mode change.
RTC_FAST_ATTR StaticTask_t g_mode_task_tcb{};
RTC_FAST_ATTR alignas(16) StackType_t g_mode_task_stack[kModeTaskStackBytes]{};
TaskHandle_t g_discovery_task{};
std::atomic<bool> g_audio_mode{};
std::atomic<bool> g_audio_pipeline_paused{};
std::atomic<bool> g_wifi_started{};
std::atomic<bool> g_wifi_connected{};
// Set once a reboot is committed. Tearing the radios down raises a station
// disconnect with reason 8 (WIFI_REASON_ASSOC_LEAVE) -- our own leave, not a
// fault -- and handling it as one replaced the "Rebooting into ..." the panel
// had just been given with "RETRYING - REASON 8", then asked a stack that is
// going away to associate again.
std::atomic<bool> g_restarting{};
char g_network_detail[64]{"OFF"};
constexpr EventBits_t kWifiHasIp = BIT0;

enum class OtaWorkerCommand : std::uint8_t {
    begin,
    write,
    finish,
    abort,
};

struct OtaWorkerState {
    const esp_partition_t* update{};
    esp_ota_handle_t handle{};
    std::size_t bytes{};
    esp_err_t result{ESP_FAIL};
    OtaWorkerCommand command{OtaWorkerCommand::begin};
    StaticSemaphore_t request_storage{};
    StaticSemaphore_t completion_storage{};
    SemaphoreHandle_t request{};
    SemaphoreHandle_t completion{};
    alignas(16) std::uint8_t buffer[1024]{};
};

// OTA writes run on their own task (see ModeFlagWrite below for why a flash
// write cannot run on the httpd task's PSRAM stack).
void ota_worker_task(void* const argument) {
    auto* const state = static_cast<OtaWorkerState*>(argument);
    bool done = false;
    while (!done) {
        xSemaphoreTake(state->request, portMAX_DELAY);
        switch (state->command) {
        case OtaWorkerCommand::begin:
            state->result =
                esp_ota_begin(state->update, OTA_WITH_SEQUENTIAL_WRITES, &state->handle);
            break;
        case OtaWorkerCommand::write:
            state->result = esp_ota_write(state->handle, state->buffer, state->bytes);
            break;
        case OtaWorkerCommand::finish:
            state->result = esp_ota_end(state->handle);
            if (state->result == ESP_OK) {
                state->result = esp_ota_set_boot_partition(state->update);
            }
            done = true;
            break;
        case OtaWorkerCommand::abort:
            state->result = esp_ota_abort(state->handle);
            done = true;
            break;
        }
        xSemaphoreGive(state->completion);
    }
    vTaskDelete(nullptr);
}

bool ota_worker_call(OtaWorkerState* const state, const OtaWorkerCommand command) {
    state->command = command;
    xSemaphoreGive(state->request);
    return xSemaphoreTake(state->completion, pdMS_TO_TICKS(60000)) == pdTRUE;
}

void set_network_status(const bool enabled, const bool connected, const char* const detail) {
    g_wifi_started.store(enabled, std::memory_order_release);
    g_wifi_connected.store(enabled && connected, std::memory_order_release);
    std::snprintf(g_network_detail, sizeof g_network_detail, "%s",
                  detail != nullptr ? detail : "-");
    if (s3_v1_network_status != nullptr) {
        s3_v1_network_status(enabled, enabled && connected, g_network_name, g_network_detail);
    }
}

// A flash write from the HTTP task aborts the board. httpd runs on a 32 KiB
// PSRAM stack (see config.task_caps in start_http_server), and
// spi_flash_disable_interrupts_caches_and_other_cpu() asserts
// esp_task_stack_is_sane_cache_disabled() for any task whose stack lives in
// external RAM:
//   assert failed: spi_flash_disable_interrupts_caches_and_other_cpu
//   cache_utils.c:126 (esp_task_stack_is_sane_cache_disabled())
// So the boot-mode flags (REBOOT, AUDIO MODE, EFFECT SOAK, EFFECT PROFILE, the
// end of an OTA) are written by a worker with an internal stack, as the OTA body
// is by ota_worker. The worker exists only while the HTTP server does, so it
// costs the A2-Full arenas nothing on an audio boot, and its stack comes from
// the heap rather than .bss so it is not reserved when audio owns the box.
struct ModeFlagWrite {
    const char* key;
    std::uint8_t value;
};
TaskHandle_t g_flash_worker{};
QueueHandle_t g_flash_requests{};
SemaphoreHandle_t g_flash_done{};
SemaphoreHandle_t g_flash_lock{};
volatile bool g_flash_result{};

bool write_mode_flag_here(const char* const key, const std::uint8_t value) {
    nvs_handle_t handle{};
    if (nvs_open(kModeNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    esp_err_t result = nvs_set_u8(handle, key, value);
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    return result == ESP_OK;
}

void flash_worker_task(void*) {
    while (true) {
        ModeFlagWrite request{};
        if (xQueueReceive(g_flash_requests, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        g_flash_result = write_mode_flag_here(request.key, request.value);
        xSemaphoreGive(g_flash_done);
    }
}

// Safe from any task: writes here when the caller's own stack can survive a
// disabled cache, and hands the write to the worker when it cannot.
bool write_mode_flag(const char* const key, const bool value) {
    const auto byte = static_cast<std::uint8_t>(value ? 1U : 0U);
    if (g_flash_worker == nullptr || g_flash_lock == nullptr ||
        xTaskGetCurrentTaskHandle() == g_flash_worker) {
        return write_mode_flag_here(key, byte);
    }
    if (xSemaphoreTake(g_flash_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(kTag, "boot-mode flag '%s' could not take the flash worker lock", key);
        return false;
    }
    const ModeFlagWrite request{key, byte};
    bool ok = false;
    if (xQueueSend(g_flash_requests, &request, pdMS_TO_TICKS(1000)) == pdTRUE &&
        xSemaphoreTake(g_flash_done, pdMS_TO_TICKS(10000)) == pdTRUE) {
        ok = g_flash_result;
    } else {
        ESP_LOGE(kTag, "boot-mode flag '%s' did not reach the flash worker", key);
    }
    xSemaphoreGive(g_flash_lock);
    return ok;
}

bool start_flash_worker() {
    if (g_flash_worker != nullptr) {
        return true;
    }
    if (g_flash_requests == nullptr) {
        g_flash_requests = xQueueCreate(2, sizeof(ModeFlagWrite));
    }
    if (g_flash_done == nullptr) {
        g_flash_done = xSemaphoreCreateBinary();
    }
    if (g_flash_lock == nullptr) {
        g_flash_lock = xSemaphoreCreateMutex();
    }
    if (g_flash_requests == nullptr || g_flash_done == nullptr || g_flash_lock == nullptr) {
        ESP_LOGE(kTag, "flash worker primitives unavailable; boot-mode writes stay on the caller");
        return false;
    }
    // Internal stack, deliberately: this task exists to be the one place a flash
    // write is legal. NVS needs little of it, and it is claimed only in
    // maintenance mode.
    if (xTaskCreatePinnedToCore(flash_worker_task, "flash_flag", 3072, nullptr, kServicePriority,
                                &g_flash_worker, 0) != pdPASS) {
        g_flash_worker = nullptr;
        ESP_LOGE(kTag, "flash worker task unavailable; internal free=%u largest=%u",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
        return false;
    }
    return true;
}

// The audio window: how long an audio boot may keep the radios off before it
// restarts itself. It lives in RTC memory so arming it costs no flash write on
// the audio side, and so does the return: audio is the default boot, so the
// window leaves an RTC marker asking the next boot for maintenance.
constexpr std::uint32_t kAudioWindowMagic = 0x41574E44U;       // 'AWND'
constexpr std::uint32_t kAudioWindowResultMagic = 0x41575253U; // 'AWRS'
constexpr std::uint32_t kMaintenanceReturnMagic = 0x4D4E5452U; // 'MNTR'
RTC_NOINIT_ATTR volatile std::uint32_t g_audio_window_magic;
RTC_NOINIT_ATTR volatile std::uint32_t g_maintenance_return_magic;
RTC_NOINIT_ATTR volatile std::uint32_t g_audio_window_seconds;
// Bit 0: engage the pedal once the UI is up. The boot preset recalls with the
// pedal bypassed, so a window that only wants to know whether the transport
// streams can leave it alone, and a window that wants to measure the amp and
// the effects under load has to switch it on.
constexpr std::uint32_t kAudioWindowEngage = 1U;
// Bit 1 with bits 8..15: apply this effect mask after the boot preset, so one
// window can measure the chain with a block switched off without editing the
// preset. The user's selection is not persisted: the window ends in a restart.
constexpr std::uint32_t kAudioWindowMask = 2U;
// Bit 2: suspend the Gea UI tasks for the window. The two cores share one
// 16 KiB instruction cache and one 32 KiB data cache, so rendering on either
// core evicts what the DSP stages fetch; this measures the amp with the UI
// standing still.
constexpr std::uint32_t kAudioWindowNoUi = 4U;
// Bit 3 with bits 16..23: load this library model once the UI is up instead of
// the boot preset's profile. A window can then measure the engine on a capture
// the presets do not reference without editing a preset or the factory library.
constexpr std::uint32_t kAudioWindowModel = 8U;
// Stage A on core 1 and stage B on core 0 for this window only. If a stage's
// cost follows its core, the core's other work is the cost; if it follows the
// layers, their data placement is.
constexpr std::uint32_t kAudioWindowSwapCores = 16U;
// Bit 5: drag a knob for the whole window, so the UI is drawing while the
// amp runs. The opposite of NOUI, and the one the radios cannot arrange from
// outside: maintenance is the only mode that answers the network, so a window
// that wants a busy UI has to bring its own finger. A gain sweep is the
// load-neutral choice - it repaints the knob row without changing which blocks
// run - and it goes through coyopedal_ui_control_set_param, the same call the
// panel's touch handlers make, so it dirties exactly the regions a drag does.
constexpr std::uint32_t kAudioWindowUiSoak = 32U;
constexpr unsigned kAudioWindowMaskShift = 8U;
constexpr unsigned kAudioWindowModelShift = 16U;
RTC_NOINIT_ATTR volatile std::uint32_t g_audio_window_flags;
bool g_audio_window_engage{};
bool g_audio_window_mask_requested{};
bool g_audio_window_model_requested{};
std::uint8_t g_audio_window_model{};
bool g_audio_window_no_ui{};
bool g_audio_window_swap_cores{};
bool g_audio_window_ui_soak{};
// Where the window started, so its accounting covers only the configured run.
std::uint64_t g_audio_window_start_frames{};
std::uint64_t g_audio_window_start_silent{};
std::uint64_t g_audio_window_start_trimmed{};
std::uint32_t g_audio_window_usb_isr_cycles_start{};
std::uint32_t g_audio_window_usb_isr_count_start{};
#if configGENERATE_RUN_TIME_STATS && configUSE_TRACE_FACILITY
// Per-task run time at the start of the window; the end snapshot follows it in
// the same PSRAM block. The difference per task, against the wall clock, says
// how much of each core the DSP stages actually had.
constexpr UBaseType_t kTaskSnapshotMax = 48U;
TaskStatus_t* g_task_snapshot_start{};
UBaseType_t g_task_snapshot_start_count{};
std::int64_t g_task_snapshot_start_us{};
#endif
std::uint8_t g_audio_window_mask{};
esp_timer_handle_t g_audio_window_timer{};

// What the audio window actually measured. Audio mode has both radios off and
// the peaks live in RAM, so the numbers have to survive the trip back to
// maintenance or they cannot be read at all. This is the whole point of the
// window: one number that says whether the signal reaches the DSP, and another
// that says whether anything leaves it.
struct AudioWindowResult {
    std::uint32_t magic;
    std::uint32_t seconds;
    float input_peak;
    float channel_peak[2];
    float output_peak;
    std::uint64_t clipped;
    std::uint64_t captured;
    std::uint64_t played;
    std::uint64_t silent;
    std::uint64_t errors;
    std::uint32_t connected;
    std::uint32_t host_installed;
    std::uint32_t bypassed;
};
RTC_NOINIT_ATTR AudioWindowResult g_audio_window_result;

#if configGENERATE_RUN_TIME_STATS && configUSE_TRACE_FACILITY
void log_task_run_time(const TaskStatus_t* const start, const UBaseType_t start_count,
                       const TaskStatus_t* const end, const UBaseType_t end_count,
                       const std::int64_t elapsed_us) {
    if (elapsed_us <= 0) {
        return;
    }
    // One line per task rather than one per core: a single long line is the one
    // shape the log ring loses, and the census is worthless with a hole in it.
    const BaseType_t cores[] = {0, 1, tskNO_AFFINITY};
    for (const BaseType_t core : cores) {
        const char* const core_name = core == 0 ? "0" : core == 1 ? "1" : "any";
        for (UBaseType_t index = 0; index < end_count; ++index) {
            const TaskStatus_t& task = end[index];
            if (task.xCoreID != core) {
                continue;
            }
            std::uint32_t before = 0U;
            for (UBaseType_t other = 0; other < start_count; ++other) {
                if (start[other].xHandle == task.xHandle) {
                    before = start[other].ulRunTimeCounter;
                    break;
                }
            }
            const std::uint64_t delta = task.ulRunTimeCounter - before;
            // Tenths of a percent of the window; anything under 0.1% is noise.
            const std::uint32_t tenths =
                static_cast<std::uint32_t>(delta * 1000U / static_cast<std::uint64_t>(elapsed_us));
            if (tenths == 0U) {
                continue;
            }
            ESP_LOGW(kTag, "audio window task: core=%s %s prio=%lu %lu.%lu%% of %lld ms", core_name,
                     task.pcTaskName, static_cast<unsigned long>(task.uxCurrentPriority),
                     static_cast<unsigned long>(tenths / 10U),
                     static_cast<unsigned long>(tenths % 10U),
                     static_cast<long long>(elapsed_us / 1000));
        }
    }
}
#endif

void audio_window_expired(void*) {
    usb_audio_diagnostics_t audio{};
    usb_audio_get_diagnostics(&audio);
    g_audio_window_result.seconds = g_audio_window_seconds;
    g_audio_window_result.input_peak = audio.input_peak;
    g_audio_window_result.channel_peak[0] = audio.input_channel_peak[0];
    g_audio_window_result.channel_peak[1] = audio.input_channel_peak[1];
    g_audio_window_result.output_peak = audio.output_peak;
    g_audio_window_result.clipped = audio.output_clipped_samples;
    g_audio_window_result.captured = audio.captured_frames;
    g_audio_window_result.played = audio.played_frames;
    g_audio_window_result.silent = audio.silent_frames;
    g_audio_window_result.errors = audio.transfer_errors;
    g_audio_window_result.connected = audio.connected ? 1U : 0U;
    g_audio_window_result.host_installed = usb_audio_host_installed() ? 1U : 0U;
    g_audio_window_result.bypassed = coyopedal_pedal_dsp_pedal_bypassed() ? 1U : 0U;
    std::atomic_thread_fence(std::memory_order_release);
    g_audio_window_result.magic = kAudioWindowResultMagic;
    // The half that says whether the DSP kept up. These counters accumulate
    // from the moment the transport connected, so they cover the whole window.
    // A crackle is a missed block deadline; the maxima say how far over it went.
    unsigned mask = 0U;
    for (unsigned block = 0; block < COYOPEDAL_FX_BLOCK_COUNT; ++block)
        if (coyopedal_fx_enabled(static_cast<coyopedal_fx_block_t>(block)))
            mask |= 1U << block;
#if configGENERATE_RUN_TIME_STATS && configUSE_TRACE_FACILITY
    // First of the summary, not last: who got the CPU is the question a slow UI
    // asks, and the lines a window writes last are the ones a restart can still
    // lose. Everything below it is a counter that survives on its own.
    if (g_task_snapshot_start != nullptr) {
        TaskStatus_t* const end = g_task_snapshot_start + kTaskSnapshotMax;
        const UBaseType_t end_count = uxTaskGetSystemState(end, kTaskSnapshotMax, nullptr);
        log_task_run_time(g_task_snapshot_start, g_task_snapshot_start_count, end, end_count,
                          esp_timer_get_time() - g_task_snapshot_start_us);
    }
#endif
    ESP_LOGW(kTag,
             "audio window dsp: engaged=%u amp=%u mask=%u core0=%lu/%lu core1=%lu/%lu "
             "miss0=%lu miss1=%lu input_drops=%lu output_drops=%lu "
             "internal free=%u largest=%u",
             coyopedal_pedal_dsp_pedal_bypassed() ? 0U : 1U,
             coyopedal_pedal_dsp_bypassed() ? 0U : 1U, mask,
             static_cast<unsigned long>(audio.stage_a_cycles_per_block),
             static_cast<unsigned long>(audio.stage_a_max_cycles),
             static_cast<unsigned long>(audio.stage_b_cycles_per_block),
             static_cast<unsigned long>(audio.stage_b_max_cycles),
             static_cast<unsigned long>(audio.stage_a_deadline_misses),
             static_cast<unsigned long>(audio.stage_b_deadline_misses),
             static_cast<unsigned long>(audio.input_dropped_frames),
             static_cast<unsigned long>(audio.output_dropped_frames),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    {
        usb_audio_window_stats_t window{};
        usb_audio_window_stats(&window);
        char line[256];
        int at = std::snprintf(line, sizeof line,
                               "audio window since configure: core0 max=%lu miss=%lu core1 "
                               "max=%lu miss=%lu overruns:",
                               static_cast<unsigned long>(window.stage_a_max_cycles),
                               static_cast<unsigned long>(window.stage_a_misses),
                               static_cast<unsigned long>(window.stage_b_max_cycles),
                               static_cast<unsigned long>(window.stage_b_misses));
        for (unsigned slot = 0; slot < window.overruns_kept && at > 0 && at < int(sizeof line) - 24;
             ++slot) {
            at += std::snprintf(line + at, sizeof line - at, " %lums=%lu",
                                static_cast<unsigned long>(window.overrun_ms[slot]),
                                static_cast<unsigned long>(window.overrun_cycles[slot]));
        }
        at += std::snprintf(
            line + at, sizeof line - at, " task_wdt_firings=%lu silent=%llu trimmed=%llu",
            static_cast<unsigned long>(g_task_wdt_firings),
            static_cast<unsigned long long>(audio.silent_frames - g_audio_window_start_silent),
            static_cast<unsigned long long>(audio.trimmed_frames - g_audio_window_start_trimmed));
        ESP_LOGW(kTag, "%s", line);
    }
    {
        const std::uint64_t blocks =
            (audio.captured_frames - g_audio_window_start_frames) / COYOPEDAL_PEDAL_BLOCK_FRAMES;
        if (&pedalboard_usb_isr_cycles != nullptr && blocks != 0U) {
            const std::uint32_t isr_cycles =
                pedalboard_usb_isr_cycles - g_audio_window_usb_isr_cycles_start;
            const std::uint32_t isr_count =
                pedalboard_usb_isr_count - g_audio_window_usb_isr_count_start;
            const auto per_call = [](const std::uint32_t cycles, const std::uint32_t count) {
                return static_cast<unsigned long>(count == 0U ? 0U : cycles / count);
            };
            ESP_LOGW(
                kTag,
                "audio window usb: blocks=%lu callback=%lu/block isr=%lu/block "
                "isr_runs=%lu per 1000 blocks capture=%lu/call playback=%lu/call "
                "feedback=%lu/call resubmit=%lu/call",
                static_cast<unsigned long>(blocks),
                static_cast<unsigned long>(audio.usb_callback_cycles_per_block),
                static_cast<unsigned long>(isr_cycles / blocks),
                static_cast<unsigned long>(static_cast<std::uint64_t>(isr_count) * 1000U / blocks),
                per_call(audio.usb_capture_callback_cycles, audio.usb_capture_callback_count),
                per_call(audio.usb_playback_callback_cycles, audio.usb_playback_callback_count),
                per_call(audio.usb_feedback_callback_cycles, audio.usb_feedback_callback_count),
                per_call(audio.usb_resubmit_cycles, audio.usb_resubmit_count));
        }
    }
    ESP_LOGW(
        kTag,
        "audio window closing: input_peak=%.6f ch0=%.6f ch1=%.6f "
        "output_peak=%.6f clipped=%llu "
        "captured=%llu played=%llu silent=%llu connected=%s host=%s",
        static_cast<double>(audio.input_peak), static_cast<double>(audio.input_channel_peak[0]),
        static_cast<double>(audio.input_channel_peak[1]), static_cast<double>(audio.output_peak),
        static_cast<unsigned long long>(audio.output_clipped_samples),
        static_cast<unsigned long long>(audio.captured_frames),
        static_cast<unsigned long long>(audio.played_frames),
        static_cast<unsigned long long>(audio.silent_frames), audio.connected ? "yes" : "no",
        usb_audio_host_installed() ? "yes" : "no");
    // Audio is the default boot; the RTC marker brings the next one up in
    // maintenance without a flash write.
    g_maintenance_return_magic = kMaintenanceReturnMagic;
    log_ring_writeback();
    esp_restart();
}

void report_audio_window_result() {
    std::atomic_thread_fence(std::memory_order_acquire);
    if (g_audio_window_result.magic != kAudioWindowResultMagic) {
        return;
    }
    ESP_LOGW(kTag,
             "last audio window (%lu s): input_peak=%.6f ch0=%.6f ch1=%.6f "
             "output_peak=%.6f clipped=%llu "
             "captured=%llu played=%llu silent=%llu errors=%llu connected=%s host=%s "
             "pedal=%s",
             static_cast<unsigned long>(g_audio_window_result.seconds),
             static_cast<double>(g_audio_window_result.input_peak),
             static_cast<double>(g_audio_window_result.channel_peak[0]),
             static_cast<double>(g_audio_window_result.channel_peak[1]),
             static_cast<double>(g_audio_window_result.output_peak),
             static_cast<unsigned long long>(g_audio_window_result.clipped),
             static_cast<unsigned long long>(g_audio_window_result.captured),
             static_cast<unsigned long long>(g_audio_window_result.played),
             static_cast<unsigned long long>(g_audio_window_result.silent),
             static_cast<unsigned long long>(g_audio_window_result.errors),
             g_audio_window_result.connected != 0U ? "yes" : "no",
             g_audio_window_result.host_installed != 0U ? "yes" : "no",
             g_audio_window_result.bypassed != 0U ? "bypassed" : "engaged");
}

bool maintenance_boot_requested() {
    nvs_handle_t handle{};
    if (nvs_open(kModeNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    std::uint8_t requested = 0U;
    const esp_err_t result = nvs_get_u8(handle, kMaintenanceKey, &requested);
    nvs_close(handle);
    return result == ESP_OK && requested != 0U;
}

bool select_maintenance_boot(const bool maintenance) {
    return write_mode_flag(kMaintenanceKey, maintenance);
}

bool select_exclusive_audio_boot(const bool exclusive) {
    return write_mode_flag(kExclusiveAudioKey, exclusive);
}

bool return_maintenance_boot_requested() {
    nvs_handle_t handle{};
    if (nvs_open(kModeNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    std::uint8_t requested = 0U;
    const esp_err_t result = nvs_get_u8(handle, kReturnMaintenanceKey, &requested);
    nvs_close(handle);
    return result == ESP_OK && requested != 0U;
}

bool select_return_maintenance_boot(const bool requested) {
    return write_mode_flag(kReturnMaintenanceKey, requested);
}

// The NVS one-shot is cleared as soon as the boot decision reads it, so a
// crash on the way up cannot loop the board in audio. What the boot learned
// has to live somewhere for the rest of this startup, or the post-OTA return
// is decided from a flag that is already gone: the board then simply stays in
// audio until someone presses the mode button.
bool g_return_maintenance_after_startup{};

bool consume_return_maintenance_boot() {
    if (!return_maintenance_boot_requested()) {
        return g_return_maintenance_after_startup;
    }
    g_return_maintenance_after_startup = true;
    return select_return_maintenance_boot(false);
}

bool consume_exclusive_audio_boot() {
    nvs_handle_t handle{};
    if (nvs_open(kModeNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    std::uint8_t requested = 0U;
    esp_err_t result = nvs_get_u8(handle, kExclusiveAudioKey, &requested);
    if (result == ESP_OK && requested != 0U) {
        result = nvs_set_u8(handle, kExclusiveAudioKey, 0U);
        if (result == ESP_OK) {
            result = nvs_commit(handle);
        }
    }
    nvs_close(handle);
    return result == ESP_OK && requested != 0U;
}

constexpr std::uint32_t kEffectProfileMagic = 0x45504633U;        // "EPF3"
constexpr std::uint32_t kEffectProfileRequestMagic = 0x45505251U; // "EPRQ"
constexpr std::uint32_t kEffectSoakRequestMagic = 0x45535251U;    // "ESRQ"
// The realtime workers run at 19/22. The profiler normally sleeps and consumes
// no CPU, but its timer must be able to wake and stop a chain that has saturated
// both cores. BOOT remains the highest-priority escape hatch at 24.
constexpr UBaseType_t kEffectProfilerPriority = 23;
constexpr UBaseType_t kEffectMeasurementPriority = 18;
// The first row is the common NAM-only baseline, the next six isolate one
// effect at a time in signal-chain order, and the final row measures the actual
// all-effects-on chain. This makes each block cost an independent subtraction
// from the same baseline instead of accumulating run-to-run variation.
constexpr std::uint8_t kEffectProfileMasks[] = {
    0x00U, 0x01U, 0x02U, 0x10U, 0x04U, 0x20U, 0x08U, 0x3fU,
};

struct EffectProfileRow {
    std::uint8_t mask{};
    std::uint8_t reserved[3]{};
    usb_audio_diagnostics_t audio{};
    std::uint32_t internal_free{};
    std::uint32_t internal_largest{};
    std::uint32_t reverb_input_cycles{};
    std::uint32_t reverb_tank_cycles{};
    std::uint32_t reverb_blocks{};
};

struct EffectProfileStore {
    std::uint32_t magic{};
    std::uint32_t seconds{};
    std::uint32_t count{};
    std::uint32_t error{};
    std::uint8_t synthetic_input{1U};
    std::uint8_t cycle_telemetry{1U};
    std::uint16_t preset{0xffffU};
    std::uint16_t model{0xffffU};
    std::uint32_t layer_blocks{};
    std::uint32_t layer_cycles[24]{};
    std::uint32_t wide_mixin_blocks[23]{};
    EffectProfileRow rows[sizeof kEffectProfileMasks]{};
};

// Survives the emergency software restart used when the radio cannot reclaim
// its maintenance heap after a profile. The magic is written only after the
// measurements are complete (or a bounded failure has been recorded).
RTC_NOINIT_ATTR EffectProfileStore g_effect_profile_store;
std::atomic<bool> g_effect_profile_running{};

bool pause_audio_pipeline() {
    bool expected = false;
    if (!g_audio_pipeline_paused.compare_exchange_strong(expected, true)) {
        return true;
    }
    if (usb_audio_begin_update()) {
        return true;
    }
    g_audio_pipeline_paused.store(false, std::memory_order_release);
    return false;
}

void resume_audio_pipeline() {
    if (g_audio_pipeline_paused.exchange(false, std::memory_order_acq_rel)) {
        usb_audio_end_update();
    }
}

int remote_vprintf(const char* const format, va_list args) {
    char line[512];
    va_list copy;
    va_copy(copy, args);
    const int wanted = std::vsnprintf(line, sizeof line, format, copy);
    va_end(copy);

    if (wanted > 0) {
        const std::size_t bytes = std::min<std::size_t>(wanted, sizeof line - 1U);
        portENTER_CRITICAL(&g_log_lock);
        for (std::size_t index = 0; index < bytes; ++index) {
            g_log[g_log_total % kLogCapacity] = line[index];
            ++g_log_total;
        }
        portEXIT_CRITICAL(&g_log_lock);
    }
    return g_console_vprintf != nullptr ? g_console_vprintf(format, args) : wanted;
}

bool constant_time_equal(const char* const first, const char* const second) {
    if (first == nullptr || second == nullptr) {
        return false;
    }
    const std::size_t first_size = std::strlen(first);
    const std::size_t second_size = std::strlen(second);
    if (first_size != second_size) {
        return false;
    }
    unsigned difference = 0U;
    for (std::size_t index = 0; index < first_size; ++index) {
        difference |= static_cast<unsigned>(first[index] ^ second[index]);
    }
    return difference == 0U;
}

bool mac_hex(const psa_key_id_t key, const char* const message, const std::size_t message_size,
             char (&hex)[65]) {
    std::uint8_t mac[32]{};
    std::size_t mac_size{};
    if (psa_mac_compute(key, PSA_ALG_HMAC(PSA_ALG_SHA_256),
                        reinterpret_cast<const std::uint8_t*>(message), message_size, mac,
                        sizeof mac, &mac_size) != PSA_SUCCESS ||
        mac_size != sizeof mac) {
        return false;
    }
    for (std::size_t index = 0; index < sizeof mac; ++index) {
        std::snprintf(hex + index * 2U, 3U, "%02x", mac[index]);
    }
    return true;
}

// Streams a captured census as `H <heap start>` headers followed by
// `<address>,<size>,u|f` blocks, several to a line. The device does the walking
// and nothing else: attribution is a diff against another slot, which is host
// work and does not belong in the realtime image.
void send_heap_map(httpd_req_t* const request, const unsigned slot) {
    const coyopedal_heap_block_t* const blocks = coyopedal_heap_map_blocks(slot);
    const std::size_t count = coyopedal_heap_map_count(slot);
    std::size_t total = 0U;
    std::size_t used_bytes = 0U;
    std::size_t largest = 0U;
    for (std::size_t index = 0; blocks != nullptr && index < count; ++index) {
        total += blocks[index].size;
        if (blocks[index].used)
            used_bytes += blocks[index].size;
        else if (blocks[index].size > largest)
            largest = blocks[index].size;
    }
    char line[256]{};
    int used = std::snprintf(
        line, sizeof line,
        "slot=%u uptime_ms=%u blocks=%u dropped=%u "
        "captured_total=%u captured_used=%u captured_free=%u "
        "captured_largest=%u\n",
        slot, static_cast<unsigned>(coyopedal_heap_map_uptime_ms(slot)),
        static_cast<unsigned>(count), static_cast<unsigned>(coyopedal_heap_map_dropped(slot)),
        static_cast<unsigned>(total), static_cast<unsigned>(used_bytes),
        static_cast<unsigned>(total - used_bytes), static_cast<unsigned>(largest));
    httpd_resp_send_chunk(request, line, used);
    if (blocks == nullptr || count == 0U) {
        httpd_resp_send_chunk(request, nullptr, 0);
        return;
    }
    std::uint32_t heap = 0U;
    used = 0;
    for (std::size_t index = 0; index < count; ++index) {
        if (blocks[index].heap_start != heap) {
            if (used > 0) {
                line[used++] = '\n';
                httpd_resp_send_chunk(request, line, used);
                used = 0;
            }
            heap = blocks[index].heap_start;
            char header[48]{};
            const int length =
                std::snprintf(header, sizeof header, "H %08x\n", static_cast<unsigned>(heap));
            httpd_resp_send_chunk(request, header, length);
        }
        used += std::snprintf(line + used, sizeof line - static_cast<std::size_t>(used),
                              "%08x,%u,%c ", static_cast<unsigned>(blocks[index].address),
                              static_cast<unsigned>(blocks[index].size),
                              blocks[index].used ? 'u' : 'f');
        if (used > 200) {
            line[used++] = '\n';
            httpd_resp_send_chunk(request, line, used);
            used = 0;
        }
    }
    if (used > 0) {
        line[used++] = '\n';
        httpd_resp_send_chunk(request, line, used);
    }
    httpd_resp_send_chunk(request, nullptr, 0);
}

bool authorized(httpd_req_t* const request) {
    char token[96]{};
    if (httpd_req_get_hdr_value_str(request, "X-CoyoPedal-Token", token, sizeof token) != ESP_OK ||
        !constant_time_equal(token, COYOPEDAL_REMOTE_TOKEN)) {
        httpd_resp_set_status(request, "401 Unauthorized");
        httpd_resp_set_type(request, "text/plain");
        httpd_resp_sendstr(request, "authentication required\n");
        return false;
    }
    return true;
}

void send_status(httpd_req_t* const request) {
    usb_audio_diagnostics_t audio{};
    usb_audio_get_diagnostics(&audio);

    const esp_partition_t* const running = esp_ota_get_running_partition();
    const esp_app_desc_t* const app = esp_app_get_description();
    char elf_sha256[65]{};
    for (unsigned i = 0; i < 32; ++i)
        std::snprintf(elf_sha256 + i * 2, 3, "%02x", app->app_elf_sha256[i]);
    httpd_resp_set_hdr(request, "X-CoyoPedal-ELF-SHA256", elf_sha256);

    wifi_ap_record_t access_point{};
    const int rssi =
        esp_wifi_sta_get_ap_info(&access_point) == ESP_OK ? static_cast<int>(access_point.rssi) : 0;
    const char* const audio_protocol = audio.audio_protocol == 2U   ? "uac2"
                                       : audio.audio_protocol == 1U ? "uac1"
                                                                    : "none";
    const char* const usb_speed = audio.usb_speed == USB_SPEED_HIGH   ? "high"
                                  : audio.usb_speed == USB_SPEED_FULL ? "full"
                                  : audio.device_present              ? "low"
                                                                      : "none";
    // The status buffer is cold; keep it in PSRAM.
    constexpr std::size_t kResponseCapacity = 3072U;
    char* const response = static_cast<char*>(
        heap_caps_malloc(kResponseCapacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (response == nullptr) {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "status buffer unavailable");
        return;
    }
    std::snprintf(
        response, kResponseCapacity,
        "{\"device\":\"" COYOPEDAL_DEVICE_NAME "\",\"mode\":\"%s\","
        "\"network\":\"%s\","
        "\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"uptime_ms\":%lld,"
        "\"reset_reason\":%d,\"firmware\":{\"project\":\"%s\","
        "\"version\":\"%s\",\"built\":\"%s %s\",\"partition\":\"%s\"},"
        "\"memory\":{\"heap_free\":%u,\"heap_min\":%u,\"internal_free\":%u,"
        "\"internal_largest\":%u,\"psram_free\":%u},"
        "\"wifi\":{\"rssi\":%d},"
        "\"audio\":{\"connected\":%s,\"device_present\":%s,"
        "\"transport_supported\":%s,\"protocol\":\"%s\","
        "\"usb_speed\":\"%s\",\"vid_pid\":\"%04x:%04x\","
        "\"capture_mps\":%u,\"playback_mps\":%u,"
        // The peak meters are the first thing to read when the pedal is silent:
        // input_peak 0 means nothing arrives from the interface, input_peak
        // non-zero with output_peak 0 means the chain swallows it, and both
        // non-zero puts the fault past the output ring.
        "\"levels\":{\"input_peak\":%.6f,\"output_peak\":%.6f,"
        "\"clipped_samples\":%llu},"
        // What the last AUDIO TRY window measured, carried through the reboot
        // in RTC memory. Audio mode has no network, so this is the only way the
        // peaks from a real audio boot can be read.
        "\"last_window\":{\"valid\":%s,\"seconds\":%u,"
        "\"input_peak\":%.6f,\"input_channel_peak\":[%.6f,%.6f],"
        "\"output_peak\":%.6f,\"clipped_samples\":%llu,"
        "\"captured_frames\":%llu,\"played_frames\":%llu,"
        "\"silent_frames\":%llu,\"transfer_errors\":%llu,"
        "\"connected\":%s,\"host_installed\":%s,\"pedal_bypassed\":%s},"
        "\"captured_frames\":%llu,"
        "\"played_frames\":%llu,\"silent_frames\":%llu,\"trimmed_frames\":%llu,"
        "\"transfer_errors\":%llu,\"feedback_packets\":%llu,"
        "\"feedback_frames\":%.6f,\"input_drops\":%u,\"output_drops\":%u,"
        "\"deadline_misses\":%u,\"input_ring\":%u,\"output_ring\":%u,"
        "\"load_cycles_per_block\":%u,\"stage_a_recent_cycles\":%u,"
        "\"stage_b_recent_cycles\":%u,\"stage_a_stack_free\":%u,"
        "\"stage_b_stack_free\":%u,\"stage_a_cycles_per_block\":%u,"
        "\"stage_b_cycles_per_block\":%u,"
        "\"stage_a_max_cycles\":%u,\"stage_b_max_cycles\":%u,"
        "\"stage_a_deadline_misses\":%u,"
        "\"stage_b_deadline_misses\":%u,"
        "\"usb_capture_callback_cycles\":%u,"
        "\"usb_capture_callback_count\":%u,"
        "\"usb_playback_callback_cycles\":%u,"
        "\"usb_playback_callback_count\":%u,"
        "\"usb_feedback_callback_cycles\":%u,"
        "\"usb_feedback_callback_count\":%u,"
        "\"usb_callback_cycles_per_block\":%u}}",
        g_audio_mode.load(std::memory_order_acquire) ? "audio" : "maintenance", g_network_name,
        g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5],
        static_cast<long long>(esp_timer_get_time() / 1000), static_cast<int>(esp_reset_reason()),
        app != nullptr ? app->project_name : "", app != nullptr ? app->version : "",
        app != nullptr ? app->date : "", app != nullptr ? app->time : "",
        running != nullptr ? running->label : "", static_cast<unsigned>(esp_get_free_heap_size()),
        static_cast<unsigned>(esp_get_minimum_free_heap_size()),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
        static_cast<unsigned>(
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)), rssi,
        audio.connected ? "true" : "false", audio.device_present ? "true" : "false",
        audio.transport_supported ? "true" : "false", audio_protocol, usb_speed, audio.vendor_id,
        audio.product_id, audio.capture_mps, audio.playback_mps,
        static_cast<double>(audio.input_peak), static_cast<double>(audio.output_peak),
        static_cast<unsigned long long>(audio.output_clipped_samples),
        g_audio_window_result.magic == kAudioWindowResultMagic ? "true" : "false",
        static_cast<unsigned>(g_audio_window_result.seconds),
        static_cast<double>(g_audio_window_result.input_peak),
        static_cast<double>(g_audio_window_result.channel_peak[0]),
        static_cast<double>(g_audio_window_result.channel_peak[1]),
        static_cast<double>(g_audio_window_result.output_peak),
        static_cast<unsigned long long>(g_audio_window_result.clipped),
        static_cast<unsigned long long>(g_audio_window_result.captured),
        static_cast<unsigned long long>(g_audio_window_result.played),
        static_cast<unsigned long long>(g_audio_window_result.silent),
        static_cast<unsigned long long>(g_audio_window_result.errors),
        g_audio_window_result.connected != 0U ? "true" : "false",
        g_audio_window_result.host_installed != 0U ? "true" : "false",
        g_audio_window_result.bypassed != 0U ? "true" : "false",
        static_cast<unsigned long long>(audio.captured_frames),
        static_cast<unsigned long long>(audio.played_frames),
        static_cast<unsigned long long>(audio.silent_frames),
        static_cast<unsigned long long>(audio.trimmed_frames),
        static_cast<unsigned long long>(audio.transfer_errors),
        static_cast<unsigned long long>(audio.feedback_packets),
        static_cast<double>(audio.feedback_16_16) / 65536.0,
        static_cast<unsigned>(audio.input_dropped_frames),
        static_cast<unsigned>(audio.output_dropped_frames),
        static_cast<unsigned>(audio.deadline_misses),
        static_cast<unsigned>(audio.input_ring_frames),
        static_cast<unsigned>(audio.output_ring_frames),
        static_cast<unsigned>(audio.load_cycles_per_block),
        static_cast<unsigned>(audio.stage_a_recent_cycles),
        static_cast<unsigned>(audio.stage_b_recent_cycles),
        static_cast<unsigned>(audio.stage_a_stack_free),
        static_cast<unsigned>(audio.stage_b_stack_free),
        static_cast<unsigned>(audio.stage_a_cycles_per_block),
        static_cast<unsigned>(audio.stage_b_cycles_per_block),
        static_cast<unsigned>(audio.stage_a_max_cycles),
        static_cast<unsigned>(audio.stage_b_max_cycles),
        static_cast<unsigned>(audio.stage_a_deadline_misses),
        static_cast<unsigned>(audio.stage_b_deadline_misses),
        static_cast<unsigned>(audio.usb_capture_callback_cycles),
        static_cast<unsigned>(audio.usb_capture_callback_count),
        static_cast<unsigned>(audio.usb_playback_callback_cycles),
        static_cast<unsigned>(audio.usb_playback_callback_count),
        static_cast<unsigned>(audio.usb_feedback_callback_cycles),
        static_cast<unsigned>(audio.usb_feedback_callback_count),
        static_cast<unsigned>(audio.usb_callback_cycles_per_block));
    httpd_resp_set_type(request, "application/json");
    httpd_resp_sendstr(request, response);
    heap_caps_free(response);
}

esp_err_t status_handler(httpd_req_t* const request) {
    if (!authorized(request)) {
        return ESP_OK;
    }
    send_status(request);
    return ESP_OK;
}

std::uint64_t query_since(httpd_req_t* const request) {
    char query[64]{};
    char value[32]{};
    if (httpd_req_get_url_query_str(request, query, sizeof query) != ESP_OK ||
        httpd_query_key_value(query, "since", value, sizeof value) != ESP_OK) {
        return 0U;
    }
    char* end{};
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    return end != value && *end == '\0' ? static_cast<std::uint64_t>(parsed) : 0U;
}

esp_err_t logs_handler(httpd_req_t* const request) {
    if (!authorized(request)) {
        return ESP_OK;
    }
    const std::uint64_t requested = query_since(request);
    auto* const copy =
        static_cast<char*>(heap_caps_malloc(kLogCapacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (copy == nullptr) {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "log buffer unavailable");
        return ESP_OK;
    }

    std::uint64_t cursor{};
    std::uint64_t start{};
    std::size_t count{};
    portENTER_CRITICAL(&g_log_lock);
    cursor = g_log_total;
    const std::uint64_t oldest = cursor > kLogCapacity ? cursor - kLogCapacity : 0U;
    start = std::min(std::max(requested, oldest), cursor);
    count = static_cast<std::size_t>(cursor - start);
    for (std::size_t index = 0; index < count; ++index) {
        copy[index] = g_log[(start + index) % kLogCapacity];
    }
    portEXIT_CRITICAL(&g_log_lock);

    char cursor_text[32];
    std::snprintf(cursor_text, sizeof cursor_text, "%llu", static_cast<unsigned long long>(cursor));
    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(request, "X-CoyoPedal-Log-Cursor", cursor_text);
    httpd_resp_send(request, copy, count);
    heap_caps_free(copy);
    return ESP_OK;
}

extern "C" void s3_v1_ui_counters_reset();
extern "C" void s3_v1_ui_counters_log();

bool apply_effect_mask(const std::uint8_t mask) {
    bool applied = true;
    for (std::uint8_t block = 0; block < COYOPEDAL_FX_BLOCK_COUNT; ++block) {
        applied = coyopedal_ui_control_set_enabled(block, (mask & (1U << block)) != 0U) && applied;
    }
    return applied;
}

void delayed_effect_profile(const std::uint32_t seconds, const bool full_chain_only,
                            const bool synthetic_input, const bool cycle_telemetry) {
    vTaskPrioritySet(nullptr, kEffectProfilerPriority);
    const TickType_t test_duration = pdMS_TO_TICKS(seconds * 1000U);
    vTaskDelay(pdMS_TO_TICKS(350));

    g_effect_profile_store.magic = 0U;
    g_effect_profile_store.seconds = seconds;
    g_effect_profile_store.count = 0U;
    g_effect_profile_store.error = 0U;
    g_effect_profile_store.preset = 0xffffU;
    g_effect_profile_store.model = 0xffffU;
    g_effect_profile_store.layer_blocks = 0U;
    std::fill_n(g_effect_profile_store.layer_cycles, 24U, 0U);
    std::fill_n(g_effect_profile_store.wide_mixin_blocks, 23U, 0U);
    g_audio_mode.store(true, std::memory_order_release);
    ESP_LOGI(kTag, "exclusive effect profile; internal=%u largest=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    usb_audio_diagnostics_t connection{};
    // The board may still be attached to a computer when the profile boot
    // starts. Allow a minute to move the cable to the audio interface before
    // treating a missing device as a timeout.
    for (unsigned attempt = 0U; attempt < 600U; ++attempt) {
        usb_audio_get_diagnostics(&connection);
        if (connection.connected) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!connection.connected) {
        ESP_LOGE(kTag, "effect profile timed out waiting for USB audio enumeration");
        g_effect_profile_store.error = 5U;
    } else if (!pause_audio_pipeline()) {
        g_effect_profile_store.error = 4U;
    } else if (!usb_audio_set_diagnostic_input(synthetic_input)) {
        g_effect_profile_store.error = 6U;
    } else {
        coyopedal_control_state_t state{};
        coyopedal_ui_control_state(&state);
        g_effect_profile_store.preset = state.preset;
        g_effect_profile_store.model = state.model;
    }
    s3_v1_ui_counters_reset();

    const std::size_t first_mask = full_chain_only ? sizeof kEffectProfileMasks - 1U : 0U;
    const std::size_t mask_count = full_chain_only ? 1U : sizeof kEffectProfileMasks;
    for (std::size_t index = 0; index < mask_count && g_effect_profile_store.error == 0U; ++index) {
        const std::uint8_t mask = kEffectProfileMasks[first_mask + index];
        if (!apply_effect_mask(mask)) {
            g_effect_profile_store.error = 2U;
            break;
        }
        resume_audio_pipeline();
        // Resuming starts with an intentionally empty output ring. Let the
        // pipeline refill and the recurrent/effect state warm before resetting
        // the counters; otherwise startup silence and the first scheduling
        // transient are reported as steady-state underruns/deadline misses.
        vTaskDelay(pdMS_TO_TICKS(500));
        // app_main was raised above USB and DSP so a saturated test can always
        // reach its bounded stop path. The counter reset and measurement must
        // run below every realtime audio worker or the profiler manufactures
        // the 64-frame underrun it is trying to observe.
        vTaskPrioritySet(nullptr, kEffectMeasurementPriority);
        usb_audio_set_diagnostic_accounting(false);
        vTaskDelay(pdMS_TO_TICKS(5));
        usb_audio_reset_diagnostics();
        usb_audio_set_cycle_telemetry(cycle_telemetry);
        if (cycle_telemetry) {
            coyopedal_fx_reverb_profile_reset();
        }
        // Peak scanning touches every output sample and belongs to the
        // diagnostic profile, not the production-equivalent soak.
        usb_audio_capture_output_levels(!full_chain_only);
        usb_audio_set_diagnostic_accounting(true);
        vTaskDelay(test_duration);
        usb_audio_set_diagnostic_accounting(false);
        vTaskDelay(pdMS_TO_TICKS(5));
        if (!pause_audio_pipeline()) {
            vTaskPrioritySet(nullptr, kEffectProfilerPriority);
            usb_audio_capture_output_levels(false);
            g_effect_profile_store.error = 3U;
            break;
        }
        vTaskPrioritySet(nullptr, kEffectProfilerPriority);
        usb_audio_capture_output_levels(false);
        usb_audio_set_diagnostic_accounting(true);

        EffectProfileRow& row = g_effect_profile_store.rows[index];
        row = {};
        row.mask = mask;
        usb_audio_get_diagnostics(&row.audio);
        usb_audio_set_cycle_telemetry(false);
        if (cycle_telemetry) {
            coyopedal_fx_reverb_profile_read(&row.reverb_input_cycles, &row.reverb_tank_cycles,
                                             &row.reverb_blocks);
        }
        if (!full_chain_only) {
            g_effect_profile_store.layer_blocks += static_cast<std::uint32_t>(
                row.audio.captured_frames / COYOPEDAL_PEDAL_BLOCK_FRAMES);
        }
        row.internal_free =
            static_cast<std::uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        row.internal_largest =
            static_cast<std::uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        g_effect_profile_store.count = static_cast<std::uint32_t>(index + 1U);
    }

    usb_audio_set_diagnostic_input(false);
    usb_audio_capture_output_levels(false);
    s3_v1_ui_counters_log();

    std::atomic_thread_fence(std::memory_order_release);
    g_effect_profile_store.magic = kEffectProfileMagic;
    g_effect_profile_running.store(false, std::memory_order_release);
    if (!select_maintenance_boot(true)) {
        ESP_LOGE(kTag, "could not select maintenance boot after effect profile");
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

void schedule_restart() {
    // Whatever the caller last told the panel is the last thing it should say.
    g_restarting.store(true, std::memory_order_release);
    // The HTTP response has already been sent when this is called. Reboot from
    // the existing server task instead of allocating a one-shot task: after the
    // UI, USB host and three DSP slots are resident, the internal heap can be
    // deliberately fragmented into blocks smaller than a new task stack.
    vTaskDelay(pdMS_TO_TICKS(350));
    // A footswitch hold asks for the mode change at 1500ms, so the player is
    // very likely still pressing GPIO0 right now.
    coyopedal_mode_button_wait_for_release();
    esp_restart();
}

bool schedule_effect_profile(const std::uint32_t seconds, const bool full_chain_only,
                             const bool synthetic_input = true, const bool cycle_telemetry = true) {
    bool expected = false;
    if (!g_effect_profile_running.compare_exchange_strong(expected, true,
                                                          std::memory_order_acq_rel)) {
        return false;
    }
    g_effect_profile_store.seconds = seconds;
    g_effect_profile_store.count = 0U;
    g_effect_profile_store.error = 0U;
    g_effect_profile_store.synthetic_input = synthetic_input ? 1U : 0U;
    g_effect_profile_store.cycle_telemetry = cycle_telemetry ? 1U : 0U;
    std::atomic_thread_fence(std::memory_order_release);
    g_effect_profile_store.magic =
        full_chain_only ? kEffectSoakRequestMagic : kEffectProfileRequestMagic;
    if (!select_maintenance_boot(false) || !select_exclusive_audio_boot(true)) {
        g_effect_profile_store.magic = 0U;
        g_effect_profile_running.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

void send_effect_profile_result(httpd_req_t* const request) {
    constexpr std::size_t kCapacity = 12288U;
    char* const response =
        static_cast<char*>(heap_caps_malloc(kCapacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (response == nullptr) {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "effect profile response unavailable");
        return;
    }

    std::size_t used = 0U;
    const auto append = [&](const char* const format, auto... values) {
        if (used >= kCapacity) {
            return;
        }
        const int written = std::snprintf(response + used, kCapacity - used, format, values...);
        if (written > 0) {
            used = std::min(kCapacity, used + static_cast<std::size_t>(written));
        }
    };

    if (g_effect_profile_running.load(std::memory_order_acquire)) {
        append("{\"state\":\"running\"}\n");
    } else if (g_effect_profile_store.magic != kEffectProfileMagic) {
        append("{\"state\":\"none\"}\n");
    } else {
        std::atomic_thread_fence(std::memory_order_acquire);
        append("{\"state\":\"complete\",\"seconds\":%u,\"count\":%u,"
               "\"error\":%u,"
               "\"input\":\"%s\",\"preset\":%u,"
               "\"model\":%u,",
               static_cast<unsigned>(g_effect_profile_store.seconds),
               static_cast<unsigned>(g_effect_profile_store.count),
               static_cast<unsigned>(g_effect_profile_store.error),
               g_effect_profile_store.synthetic_input != 0U ? "synthetic" : "live",
               static_cast<unsigned>(g_effect_profile_store.preset),
               static_cast<unsigned>(g_effect_profile_store.model));
        append("\"layer_blocks\":%u,\"layer_cycles\":[",
               static_cast<unsigned>(g_effect_profile_store.layer_blocks));
        for (unsigned layer = 0U; layer < 24U; ++layer) {
            append("%s%u", layer == 0U ? "" : ",",
                   static_cast<unsigned>(g_effect_profile_store.layer_cycles[layer]));
        }
        append("],\"wide_mixin_blocks\":[");
        for (unsigned layer = 0U; layer < 23U; ++layer) {
            append("%s%u", layer == 0U ? "" : ",",
                   static_cast<unsigned>(g_effect_profile_store.wide_mixin_blocks[layer]));
        }
        append("],\"rows\":[");
        for (std::uint32_t index = 0U;
             index < g_effect_profile_store.count && index < sizeof kEffectProfileMasks; ++index) {
            const EffectProfileRow& row = g_effect_profile_store.rows[index];
            const usb_audio_diagnostics_t& audio = row.audio;
            append("%s{\"mask\":%u,\"connected\":%s,\"device_present\":%s,"
                   "\"core0\":%u,\"core1\":%u,"
                   "\"max0\":%u,\"max1\":%u,\"miss0\":%u,\"miss1\":%u,"
                   "\"captured\":%llu,\"played\":%llu,\"silent\":%llu,"
                   "\"trimmed\":%llu,"
                   "\"input_drops\":%u,\"output_drops\":%u,\"errors\":%llu,"
                   "\"input_ring\":%u,\"output_ring\":%u,"
                   "\"usb_callback_cycles_per_block\":%u,"
                   "\"usb_capture_callback_cycles\":%u,"
                   "\"usb_capture_callback_count\":%u,"
                   "\"usb_playback_callback_cycles\":%u,"
                   "\"usb_playback_callback_count\":%u,"
                   "\"usb_feedback_callback_cycles\":%u,"
                   "\"usb_feedback_callback_count\":%u,"
                   "\"clipped\":%llu,\"peak\":%.9g,"
                   "\"stack0\":%u,\"stack1\":%u,\"internal_free\":%u,"
                   "\"internal_largest\":%u,\"reverb_input_cycles\":%u,"
                   "\"reverb_tank_cycles\":%u,\"reverb_blocks\":%u}",
                   index == 0U ? "" : ",", static_cast<unsigned>(row.mask),
                   audio.connected ? "true" : "false", audio.device_present ? "true" : "false",
                   static_cast<unsigned>(audio.stage_a_cycles_per_block),
                   static_cast<unsigned>(audio.stage_b_cycles_per_block),
                   static_cast<unsigned>(audio.stage_a_max_cycles),
                   static_cast<unsigned>(audio.stage_b_max_cycles),
                   static_cast<unsigned>(audio.stage_a_deadline_misses),
                   static_cast<unsigned>(audio.stage_b_deadline_misses),
                   static_cast<unsigned long long>(audio.captured_frames),
                   static_cast<unsigned long long>(audio.played_frames),
                   static_cast<unsigned long long>(audio.silent_frames),
                   static_cast<unsigned long long>(audio.trimmed_frames),
                   static_cast<unsigned>(audio.input_dropped_frames),
                   static_cast<unsigned>(audio.output_dropped_frames),
                   static_cast<unsigned long long>(audio.transfer_errors),
                   static_cast<unsigned>(audio.input_ring_frames),
                   static_cast<unsigned>(audio.output_ring_frames),
                   static_cast<unsigned>(audio.usb_callback_cycles_per_block),
                   static_cast<unsigned>(audio.usb_capture_callback_cycles),
                   static_cast<unsigned>(audio.usb_capture_callback_count),
                   static_cast<unsigned>(audio.usb_playback_callback_cycles),
                   static_cast<unsigned>(audio.usb_playback_callback_count),
                   static_cast<unsigned>(audio.usb_feedback_callback_cycles),
                   static_cast<unsigned>(audio.usb_feedback_callback_count),
                   static_cast<unsigned long long>(audio.output_clipped_samples),
                   static_cast<double>(audio.output_peak),
                   static_cast<unsigned>(audio.stage_a_stack_free),
                   static_cast<unsigned>(audio.stage_b_stack_free),
                   static_cast<unsigned>(row.internal_free),
                   static_cast<unsigned>(row.internal_largest),
                   static_cast<unsigned>(row.reverb_input_cycles),
                   static_cast<unsigned>(row.reverb_tank_cycles),
                   static_cast<unsigned>(row.reverb_blocks));
        }
        append("]}\n");
    }
    httpd_resp_set_type(request, "application/json");
    httpd_resp_send(request, response, std::min(used, kCapacity - 1U));
    heap_caps_free(response);
}

esp_err_t command_handler(httpd_req_t* const request) {
    if (!authorized(request)) {
        return ESP_OK;
    }
    if (request->content_len <= 0 || request->content_len >= 96) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "invalid command");
        return ESP_OK;
    }
    char command[96]{};
    const int bytes = httpd_req_recv(request, command, request->content_len);
    if (bytes != request->content_len) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "incomplete command");
        return ESP_OK;
    }
    int length = bytes;
    while (length > 0 && std::isspace(static_cast<unsigned char>(command[length - 1]))) {
        command[--length] = '\0';
    }
    for (int index = 0; command[index] != '\0'; ++index) {
        command[index] =
            static_cast<char>(std::toupper(static_cast<unsigned char>(command[index])));
    }

    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    if (std::strcmp(command, "PING") == 0) {
        httpd_resp_sendstr(request, "PONG\n");
    } else if (std::strcmp(command, "UI STATS") == 0) {
#if defined(GEA_EMBEDDED_NO_DISPLAY) && GEA_EMBEDDED_NO_DISPLAY
        httpd_resp_sendstr(request, "OK no display on this board; nothing renders\n");
    } else if (false) {
#endif
        // Logs the window since the previous call, then starts a new one, so two
        // calls bracket whatever the display was doing in between.
        s3_v1_ui_counters_log();
        s3_v1_ui_counters_reset();
        httpd_resp_sendstr(request, "OK ui counters logged; window restarted\n");
    } else if (std::strcmp(command, "UI FLUSH") == 0) {
#if defined(GEA_EMBEDDED_NO_DISPLAY) && GEA_EMBEDDED_NO_DISPLAY
        // There is no surface to push and no panel to push it to, and the flush
        // would run against a framebuffer that was never allocated.
        httpd_resp_sendstr(request, "OK no display on this board; nothing to flush\n");
    } else if (false) {
#endif
        // The one cost nothing else here measures: pushing the WHOLE panel over
        // QSPI, with no layout, no raster and no dirty-region arithmetic in the
        // way. A full-screen repaint that appears to wipe across the screen over
        // seconds is either this number being enormous or many frames each paying
        // a small part of it, and the two have different fixes. flushRects with a
        // single full-screen rect is the only call that writes every pixel
        // unconditionally -- flush() returns early when the canvas is clean.
        const gea::platform::display::DisplayFlushRect whole = {
            0, 0, gea::platform::display::kWidth - 1, gea::platform::display::kHeight - 1};
        std::uint32_t calls0 = 0U;
        std::uint64_t px0 = 0U;
        gea::platform::display::Display::flushOdometerRead(calls0, px0);
        const std::int64_t started = esp_timer_get_time();
        gea::platform::display::Display::flushRects(&whole, 1);
        const std::int64_t elapsed = esp_timer_get_time() - started;
        std::uint32_t calls1 = 0U;
        std::uint64_t px1 = 0U;
        gea::platform::display::Display::flushOdometerRead(calls1, px1);
        char reply[160];
        std::snprintf(reply, sizeof reply,
                      "OK full-screen flush %lldus (%dx%d, %lu calls, %llu px)\n",
                      static_cast<long long>(elapsed), whole.x1 + 1, whole.y1 + 1,
                      static_cast<unsigned long>(calls1 - calls0),
                      static_cast<unsigned long long>(px1 - px0));
        ESP_LOGW(kTag, "ui flush: full screen %lldus calls=%lu px=%llu",
                 static_cast<long long>(elapsed), static_cast<unsigned long>(calls1 - calls0),
                 static_cast<unsigned long long>(px1 - px0));
        httpd_resp_sendstr(request, reply);
    } else if (std::strcmp(command, "HEAP MAP") == 0) {
        coyopedal_heap_map_capture(COYOPEDAL_HEAP_MAP_SLOT_LIVE);
        send_heap_map(request, COYOPEDAL_HEAP_MAP_SLOT_LIVE);
    } else if (std::strcmp(command, "STATUS") == 0 || std::strcmp(command, "AUDIO STATS") == 0) {
        send_status(request);
    } else if (std::strcmp(command, "LOGS CLEAR") == 0) {
        portENTER_CRITICAL(&g_log_lock);
        g_log_total = 0U;
        portEXIT_CRITICAL(&g_log_lock);
        httpd_resp_sendstr(request, "OK logs cleared\n");
    } else if (std::strcmp(command, "REBOOT") == 0) {
        // Come back in the mode the reboot was asked for from, by the same
        // route the OTA path uses: boot into audio so the NAM arenas allocate
        // during full startup, then take the one-shot return to maintenance.
        if (!g_audio_mode.load(std::memory_order_acquire) && !select_return_maintenance_boot(true))
            ESP_LOGE(kTag, "could not request the post-reboot return to maintenance");
        httpd_resp_sendstr(request, "OK rebooting\n");
        schedule_restart();
    } else if (std::strncmp(command, "AUDIO TRY ", 10) == 0) {
        // Hand off to a real audio boot for a bounded window and come straight
        // back. The window is armed before the engine starts and is enforced by
        // a timer that owes nothing to the engine, so a failed audio start
        // returns on schedule instead of leaving the board off the network.
        char* end{};
        const unsigned long seconds = std::strtoul(command + 10, &end, 10);
        std::uint32_t window_flags = 0U;
        if (std::strncmp(end, " ENGAGE", 7) == 0) {
            window_flags |= kAudioWindowEngage;
            end += 7;
        }
        if (std::strncmp(end, " MASK ", 6) == 0) {
            char* mask_end = nullptr;
            const unsigned long value = std::strtoul(end + 6, &mask_end, 0);
            constexpr unsigned long kEveryEffect = (1UL << COYOPEDAL_FX_BLOCK_COUNT) - 1UL;
            if (mask_end != end + 6 && value <= kEveryEffect) {
                window_flags |=
                    kAudioWindowMask | (static_cast<std::uint32_t>(value) << kAudioWindowMaskShift);
                end = mask_end;
            }
        }
        if (std::strncmp(end, " MODEL ", 7) == 0) {
            char* model_end = nullptr;
            const unsigned long value = std::strtoul(end + 7, &model_end, 10);
            // The maintenance boot never builds the model catalogue, so the
            // index can only be checked against it once the audio boot is up.
            if (model_end != end + 7 && value < 256UL) {
                window_flags |= kAudioWindowModel |
                                (static_cast<std::uint32_t>(value) << kAudioWindowModelShift);
                end = model_end;
            }
        }
        if (std::strncmp(end, " NOUI", 5) == 0) {
            window_flags |= kAudioWindowNoUi;
            end += 5;
        }
        if (std::strncmp(end, " SWAP", 5) == 0) {
            window_flags |= kAudioWindowSwapCores;
            end += 5;
        }
        if (std::strncmp(end, " UISOAK", 7) == 0) {
            window_flags |= kAudioWindowUiSoak;
            end += 7;
        }
        if (end == command + 10 || *end != '\0' || seconds < 5UL || seconds > 120UL) {
            httpd_resp_sendstr(request, "ERR AUDIO TRY <5..120 seconds> [ENGAGE] [MASK <0..63>] "
                                        "[MODEL <library-index>] [NOUI] [SWAP] [UISOAK]\n");
        } else if (!select_exclusive_audio_boot(true)) {
            httpd_resp_sendstr(request, "ERR could not request the audio boot\n");
        } else {
            g_audio_window_seconds = static_cast<std::uint32_t>(seconds);
            g_audio_window_flags = window_flags;
            g_audio_window_magic = kAudioWindowMagic;
            char reply[96]{};
            std::snprintf(reply, sizeof reply,
                          "OK audio boot for %lu s, then back to maintenance\n", seconds);
            httpd_resp_sendstr(request, reply);
            schedule_restart();
        }
    } else if (std::strcmp(command, "AUDIO MODE") == 0) {
        if (!g_audio_mode.load())
            coyopedal_remote_toggle_mode();
        httpd_resp_sendstr(request, "OK switching to audio; radios will turn off\n");
    } else if (std::strcmp(command, "EFFECT PROFILE RESULT") == 0) {
        send_effect_profile_result(request);
    } else if (std::strncmp(command, "EFFECT PROFILE ", 15) == 0) {
        const char* duration_text = command + 15;
        bool synthetic_input = true;
        if (std::strncmp(duration_text, "LIVE ", 5) == 0) {
            synthetic_input = false;
            duration_text += 5;
        } else if (std::strncmp(duration_text, "SYNTHETIC ", 10) == 0) {
            duration_text += 10;
        }
        char* end = nullptr;
        const unsigned long seconds = std::strtoul(duration_text, &end, 10);
        if (end == duration_text || *end != '\0' || seconds < 1UL || seconds > 60UL) {
            httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                "EFFECT PROFILE duration must be 1 to 60 seconds");
        } else if (!schedule_effect_profile(static_cast<std::uint32_t>(seconds), false, 1U,
                                            synthetic_input)) {
            httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "effect profile already running");
        } else {
            char reply[96];
            std::snprintf(reply, sizeof reply,
                          "OK isolated effects + full chain: 9 x %lu seconds; "
                          "rebooting into exclusive audio mode\n",
                          seconds);
            httpd_resp_sendstr(request, reply);
            schedule_restart();
        }
    } else if (std::strncmp(command, "EFFECT SOAK ", 12) == 0) {
        const char* duration_text = command + 12;
        bool synthetic_input = true;
        if (std::strncmp(duration_text, "LIVE ", 5) == 0) {
            synthetic_input = false;
            duration_text += 5;
        } else if (std::strncmp(duration_text, "SYNTHETIC ", 10) == 0) {
            duration_text += 10;
        }
        bool cycle_telemetry = true;
        if (std::strncmp(duration_text, "PRODUCTION ", 11) == 0) {
            cycle_telemetry = false;
            duration_text += 11;
        }
        char* end = nullptr;
        const unsigned long seconds = std::strtoul(duration_text, &end, 10);
        if (end == duration_text || *end != '\0' || seconds < 1UL || seconds > 60UL) {
            httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                "EFFECT SOAK duration must be 1 to 60 seconds");
        } else if (!schedule_effect_profile(static_cast<std::uint32_t>(seconds), true,
                                            synthetic_input, cycle_telemetry)) {
            httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "effect test already running");
        } else {
            char reply[96];
            std::snprintf(reply, sizeof reply,
                          "OK all-effects soak: %lu seconds; "
                          "rebooting into exclusive audio mode\n",
                          seconds);
            httpd_resp_sendstr(request, reply);
            schedule_restart();
        }
    } else if (std::strncmp(command, "EFFECT MASK ", 12) == 0) {
        char* end = nullptr;
        const unsigned long mask = std::strtoul(command + 12, &end, 0);
        constexpr unsigned long kAllEffects = (1UL << COYOPEDAL_FX_BLOCK_COUNT) - 1UL;
        if (end == command + 12 || *end != '\0' || mask > kAllEffects) {
            httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                "EFFECT MASK expects a value from 0 to 63");
        } else {
            if (!apply_effect_mask(static_cast<std::uint8_t>(mask))) {
                httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                    "effect mask could not be applied");
            } else {
                char reply[48];
                std::snprintf(reply, sizeof reply, "OK effect mask 0x%02lx\n", mask);
                httpd_resp_sendstr(request, reply);
            }
        }
    } else if (std::strncmp(command, "PRESET ", 7) == 0) {
        char* end = nullptr;
        const unsigned long index = std::strtoul(command + 7, &end, 10);
        if (end == command + 7 || *end != '\0' || index > UINT32_MAX) {
            httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "PRESET expects a library index");
        } else if (!coyopedal_ui_control_load_preset(static_cast<unsigned>(index))) {
            httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "preset load failed");
        } else {
            unsigned mask = 0U;
            for (std::uint8_t block = 0; block < COYOPEDAL_FX_BLOCK_COUNT; ++block) {
                if (coyopedal_fx_enabled(static_cast<coyopedal_fx_block_t>(block))) {
                    mask |= 1U << block;
                }
            }
            char reply[64];
            std::snprintf(reply, sizeof reply, "OK preset %lu loaded; effect mask 0x%02x\n", index,
                          mask);
            httpd_resp_sendstr(request, reply);
        }
    } else if (std::strcmp(command, "PRESET STATE TEST") == 0) {
        char error[96]{};
        if (!coyopedal_preset_load(0U, error, sizeof error)) {
            httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                error[0] == '\0' ? "preset load failed" : error);
        } else {
            for (std::uint8_t block = 0; block < COYOPEDAL_FX_BLOCK_COUNT; ++block) {
                coyopedal_fx_set_enabled(static_cast<coyopedal_fx_block_t>(block), false);
            }
            httpd_resp_sendstr(request,
                               "OK preset 1 gain/tone/model state applied; effects bypassed\n");
        }
    } else if (std::strncmp(command, "MODEL ", 6) == 0) {
        char* end = nullptr;
        const unsigned long index = std::strtoul(command + 6, &end, 10);
        if (end == command + 6 || *end != '\0' || index > UINT32_MAX) {
            httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "MODEL expects a library index");
        } else if (!coyopedal_ui_control_set_model(static_cast<unsigned>(index))) {
            httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "model load failed");
        } else {
            httpd_resp_sendstr(request, "OK model loaded\n");
        }
    } else if (std::strcmp(command, "HELP") == 0) {
        httpd_resp_sendstr(request, "PING\nSTATUS\nAUDIO STATS\nAUDIO MODE\n"
                                    "AUDIO TRY <5..120 seconds> [ENGAGE] [MASK <0..63>] "
                                    "[MODEL <library-index>] [NOUI] [SWAP] [UISOAK]\n"
                                    "HEAP MAP\nUI STATS\nUI FLUSH\n"
                                    "EFFECT MASK <0..63>\n"
                                    "EFFECT PROFILE <1..60 seconds>\nEFFECT PROFILE RESULT\n"
                                    "EFFECT SOAK <1..60 seconds>\n"
                                    "PRESET <library-index>\nPRESET STATE TEST\n"
                                    "MODEL <library-index>\n"
                                    "LOGS CLEAR\nREBOOT\n");
    } else {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "unknown command");
    }
    return ESP_OK;
}

esp_err_t ota_handler(httpd_req_t* const request) {
    if (!authorized(request)) {
        return ESP_OK;
    }
    const esp_partition_t* const update = esp_ota_get_next_update_partition(nullptr);
    if (update == nullptr) {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "no inactive OTA partition");
        return ESP_OK;
    }
    if (request->content_len <= 0 ||
        static_cast<std::size_t>(request->content_len) > update->size) {
        httpd_resp_send_err(request, HTTPD_413_CONTENT_TOO_LARGE,
                            "image does not fit inactive OTA partition");
        return ESP_OK;
    }
    const bool audio_was_running = g_audio_mode.load(std::memory_order_acquire);
    if (audio_was_running && !usb_audio_begin_update()) {
        httpd_resp_set_status(request, "503 Service Unavailable");
        httpd_resp_set_type(request, "text/plain");
        httpd_resp_sendstr(request, "audio pipeline did not reach a safe boundary\n");
        return ESP_OK;
    }

    // The HTTP task's large stack is in PSRAM. Flash operations temporarily
    // disable the external-memory cache, so execute them on a small
    // internal-stack worker while the HTTP task is blocked.
    auto* const ota = static_cast<OtaWorkerState*>(
        heap_caps_calloc(1U, sizeof(OtaWorkerState), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (ota == nullptr) {
        if (audio_was_running) {
            usb_audio_end_update();
        }
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "OTA worker allocation failed");
        return ESP_OK;
    }
    ota->update = update;
    ota->request = xSemaphoreCreateBinaryStatic(&ota->request_storage);
    ota->completion = xSemaphoreCreateBinaryStatic(&ota->completion_storage);
    if (xTaskCreatePinnedToCoreWithCaps(ota_worker_task, "ota_flash", 4096, ota,
                                        kServicePriority + 1U, nullptr, 0,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS ||
        !ota_worker_call(ota, OtaWorkerCommand::begin)) {
        heap_caps_free(ota);
        if (audio_was_running) {
            usb_audio_end_update();
        }
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA worker did not start");
        return ESP_OK;
    }
    esp_err_t result = ota->result;
    if (result != ESP_OK) {
        (void)ota_worker_call(ota, OtaWorkerCommand::abort);
        heap_caps_free(ota);
        if (audio_was_running) {
            usb_audio_end_update();
        }
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_OK;
    }

    auto* const buffer = reinterpret_cast<char*>(ota->buffer);
    constexpr std::size_t buffer_size = sizeof ota->buffer;
    int remaining = request->content_len;
    const std::int64_t transfer_deadline = esp_timer_get_time() + 60LL * 1000LL * 1000LL;
    while (remaining > 0) {
        const int received = httpd_req_recv(request, buffer, std::min<int>(remaining, buffer_size));
        if (received == HTTPD_SOCK_ERR_TIMEOUT && esp_timer_get_time() < transfer_deadline) {
            continue;
        }
        if (received <= 0) {
            (void)ota_worker_call(ota, OtaWorkerCommand::abort);
            heap_caps_free(ota);
            if (audio_was_running) {
                usb_audio_end_update();
            }
            httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                "OTA transfer failed; current image unchanged");
            return ESP_OK;
        }
        ota->bytes = static_cast<std::size_t>(received);
        const bool wrote = ota_worker_call(ota, OtaWorkerCommand::write);
        result = wrote ? ota->result : ESP_ERR_TIMEOUT;
        if (result != ESP_OK) {
            (void)ota_worker_call(ota, OtaWorkerCommand::abort);
            heap_caps_free(ota);
            if (audio_was_running) {
                usb_audio_end_update();
            }
            httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                "OTA flash write failed; current image unchanged");
            return ESP_OK;
        }
        remaining -= received;
    }

    const bool finished = ota_worker_call(ota, OtaWorkerCommand::finish);
    result = finished ? ota->result : ESP_ERR_TIMEOUT;
    heap_caps_free(ota);
    if (result != ESP_OK) {
        if (audio_was_running) {
            usb_audio_end_update();
        }
        ESP_LOGE(kTag, "OTA image rejected: %s", esp_err_to_name(result));
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                            "OTA image validation failed; current image unchanged");
        return ESP_OK;
    }

    ESP_LOGI(kTag, "validated %d-byte OTA in %s; rebooting", request->content_len, update->label);
    // Come back up in the mode the update was sent from, so a maintenance
    // measure/build/flash loop does not need the mode button every turn.
    // Always boot into audio first: the NAM arenas only get their contiguous
    // internal blocks during full audio/USB/UI startup, and a boot straight
    // into maintenance leaves a heap the later live switch cannot allocate in.
    // The one-shot return request then hands control back without a reboot.
    if (!audio_was_running && !select_return_maintenance_boot(true))
        ESP_LOGE(kTag, "could not request the post-OTA return to maintenance");
    if (!select_maintenance_boot(false)) {
        ESP_LOGE(kTag, "could not select the post-OTA boot mode");
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "OTA written but post-update audio mode could not be selected");
        return ESP_OK;
    }
    httpd_resp_set_type(request, "text/plain");
    httpd_resp_sendstr(request, "OK validated; rebooting into new slot in audio mode\n");
    schedule_restart();
    return ESP_OK;
}

esp_err_t model_import_handler(httpd_req_t* r) {
    if (!authorized(r))
        return ESP_OK;
    return coyopedal_model_import(r);
}
esp_err_t model_download_handler(httpd_req_t* r) {
    if (!authorized(r))
        return ESP_OK;
    return coyopedal_model_download(r);
}

bool register_handler(const char* const uri, const httpd_method_t method,
                      esp_err_t (*handler)(httpd_req_t*)) {
    httpd_uri_t descriptor{};
    descriptor.uri = uri;
    descriptor.method = method;
    descriptor.handler = handler;
    return httpd_register_uri_handler(g_server, &descriptor) == ESP_OK;
}

bool start_http_server() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = kHttpPort;
    config.ctrl_port = kHttpPort + 1U;
    config.task_priority = kServicePriority;
    config.core_id = 0;
    config.stack_size = 32768;
    // The HTTP task does not run in the realtime audio path, so place its
    // comparatively large stack in PSRAM and leave scarce internal SRAM to the
    // engine and radio.
    config.task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    config.max_uri_handlers = 14;
    // Before the first request: a handler that writes NVS on this 32 KiB PSRAM
    // stack aborts the board (see write_mode_flag).
    if (!start_flash_worker()) {
        ESP_LOGW(kTag, "boot-mode writes will run on the HTTP task and may abort");
    }
    const esp_err_t start_result = httpd_start(&g_server, &config);
    if (start_result != ESP_OK) {
        ESP_LOGE(kTag, "HTTP server start failed: %s (0x%x)", esp_err_to_name(start_result),
                 static_cast<unsigned>(start_result));
        return false;
    }
    const bool base_ready =
        register_handler("/v1/status", HTTP_GET, status_handler) &&
        register_handler("/v1/logs", HTTP_GET, logs_handler) &&
        register_handler("/v1/command", HTTP_POST, command_handler) &&
        register_handler("/v1/ota", HTTP_POST, ota_handler) &&
        register_handler("/v1/models/import", HTTP_POST, model_import_handler) &&
        register_handler("/v1/models/download", HTTP_GET, model_download_handler);
    return base_ready;
}

#if !COYOPEDAL_REMOTE_MODE_AP
void wifi_event(void*, esp_event_base_t event_base, const std::int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        const auto* const event = static_cast<wifi_event_sta_connected_t*>(event_data);
        ESP_LOGI(kTag,
                 "associated with %02x:%02x:%02x:%02x:%02x:%02x channel=%u authmode=%u; waiting "
                 "for DHCP",
                 event->bssid[0], event->bssid[1], event->bssid[2], event->bssid[3],
                 event->bssid[4], event->bssid[5], static_cast<unsigned>(event->channel),
                 static_cast<unsigned>(event->authmode));
        set_network_status(true, false, "ASSOCIATED - WAITING FOR IP");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const auto* const event = static_cast<wifi_event_sta_disconnected_t*>(event_data);
        if (g_restarting.load(std::memory_order_acquire)) {
            ESP_LOGI(kTag, "Wi-Fi left the network on the way down (reason=%u)",
                     static_cast<unsigned>(event->reason));
            return;
        }
        ESP_LOGW(
            kTag,
            "Wi-Fi disconnected from %s (%02x:%02x:%02x:%02x:%02x:%02x): reason=%u; reconnecting",
            g_network_name, event->bssid[0], event->bssid[1], event->bssid[2], event->bssid[3],
            event->bssid[4], event->bssid[5], static_cast<unsigned>(event->reason));
        if (g_wifi_events != nullptr) {
            xEventGroupClearBits(g_wifi_events, kWifiHasIp);
        }
        char detail[48];
        std::snprintf(detail, sizeof detail, "RETRYING - REASON %u",
                      static_cast<unsigned>(event->reason));
        set_network_status(true, false, detail);
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const auto* const event = static_cast<ip_event_got_ip_t*>(event_data);
        ESP_LOGI(kTag, "station ready: " IPSTR ":%u", IP2STR(&event->ip_info.ip),
                 static_cast<unsigned>(kHttpPort));
        if (g_wifi_task != nullptr) {
            vTaskPrioritySet(g_wifi_task, kWifiPriority);
            ESP_LOGI(kTag, "Wi-Fi association complete; task priority lowered to %u",
                     static_cast<unsigned>(kWifiPriority));
        }
        if (g_wifi_events != nullptr) {
            xEventGroupSetBits(g_wifi_events, kWifiHasIp);
        }
        char detail[48];
        std::snprintf(detail, sizeof detail, "CONNECTED - " IPSTR, IP2STR(&event->ip_info.ip));
        set_network_status(true, true, detail);
    }
}
#endif

bool start_wifi() {
    if (esp_netif_init() != ESP_OK) {
        return false;
    }
    const esp_err_t loop = esp_event_loop_create_default();
    if (loop != ESP_OK && loop != ESP_ERR_INVALID_STATE) {
        return false;
    }
    esp_read_mac(g_mac, ESP_MAC_WIFI_STA);

    wifi_init_config_t initialization = WIFI_INIT_CONFIG_DEFAULT();
    initialization.wifi_task_core_id = 1;
    // Smaller RX buffer counts can starve an all-channel association scan
    // before the maintenance service becomes reachable.
    initialization.static_rx_buf_num = 8;
    initialization.dynamic_rx_buf_num = 16;
    initialization.ampdu_tx_enable = 0;
    initialization.ampdu_rx_enable = 0;
    // The Wi-Fi blob creates its own "wifi" task with a hardcoded 3,584-byte
    // stack and no Kconfig to change it; that stack overflows during WPA2 auth
    // ("***ERROR*** A stack overflow in task wifi"). The gea ESP32 target wraps
    // the osi_funcs task-creation pointers to raise it; this app declares no
    // network capability -- it drives esp_wifi itself -- so it has to ask for
    // the adapter rather than getting it from gea's own Wi-Fi service.
    gea::targets::esp32::wifi::applyTaskStackOverride(&initialization);
    if (esp_wifi_init(&initialization) != ESP_OK) {
        return false;
    }
    g_wifi_initialized = true;
    g_wifi_task = xTaskGetHandle("wifi");
    if (g_audio_mode.load(std::memory_order_acquire) && g_wifi_task != nullptr) {
        // Association may take longer while audio is running, but it must use
        // realtime slack instead of preempting the Core 1 NAM stage.
        vTaskPrioritySet(g_wifi_task, kWifiPriority);
        ESP_LOGI(kTag, "Wi-Fi association running below the realtime audio tasks");
    } else {
        ESP_LOGI(kTag, "Wi-Fi task kept at association priority until an IP is acquired");
    }

    if (esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK)
        return false;
#if COYOPEDAL_REMOTE_MODE_AP
    g_wifi_netif = esp_netif_create_default_wifi_ap();
    if (!g_wifi_netif)
        return false;
    wifi_config_t network{};
    std::snprintf(g_network_name, sizeof g_network_name, "%s-%02X%02X%02X",
                  COYOPEDAL_REMOTE_WIFI_SSID, g_mac[3], g_mac[4], g_mac[5]);
    const std::size_t ssid_bytes = std::min(std::strlen(g_network_name), sizeof network.ap.ssid);
    std::memcpy(network.ap.ssid, g_network_name, ssid_bytes);
    std::snprintf(reinterpret_cast<char*>(network.ap.password), sizeof network.ap.password, "%s",
                  COYOPEDAL_REMOTE_WIFI_PASSWORD);
    network.ap.ssid_len = static_cast<std::uint8_t>(ssid_bytes);
    network.ap.channel = 6;
    network.ap.max_connection = 2;
    network.ap.authmode = WIFI_AUTH_WPA2_PSK;
    network.ap.pmf_cfg.required = true;
    if (esp_wifi_set_mode(WIFI_MODE_AP) != ESP_OK)
        return false;
    if (esp_wifi_set_config(WIFI_IF_AP, &network) != ESP_OK)
        return false;
#else
    g_wifi_netif = esp_netif_create_default_wifi_sta();
    if (!g_wifi_netif)
        return false;
    g_wifi_events = xEventGroupCreate();
    if (g_wifi_events == nullptr) {
        ESP_LOGE(kTag, "could not allocate Wi-Fi readiness event");
        return false;
    }
    std::snprintf(g_network_name, sizeof g_network_name, "%s", COYOPEDAL_REMOTE_WIFI_SSID);
    wifi_config_t network{};
    std::snprintf(reinterpret_cast<char*>(network.sta.ssid), sizeof network.sta.ssid, "%s",
                  COYOPEDAL_REMOTE_WIFI_SSID);
    std::snprintf(reinterpret_cast<char*>(network.sta.password), sizeof network.sta.password, "%s",
                  COYOPEDAL_REMOTE_WIFI_PASSWORD);
    network.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    network.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    network.sta.threshold.rssi = -127;
    network.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    // Station mode joins a WPA2-Personal network. Do not advertise PMF or SAE
    // negotiation here: some dual-band routers accept association and then
    // silently drop the 4-way exchange when those capabilities are mixed,
    // producing reason 15 despite a valid WPA2 passphrase.
    network.sta.pmf_cfg.capable = false;
    network.sta.pmf_cfg.required = false;
    network.sta.sae_pwe_h2e = WPA3_SAE_PWE_UNSPECIFIED;
    if (esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, nullptr) != ESP_OK)
        return false;
    if (esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, nullptr) != ESP_OK)
        return false;
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK)
        return false;
    if (esp_wifi_set_config(WIFI_IF_STA, &network) != ESP_OK)
        return false;
    set_network_status(true, false, "CONNECTING");
#endif
    if (esp_wifi_start() != ESP_OK) {
        set_network_status(false, false, "WI-FI START FAILED");
        return false;
    }
#if !COYOPEDAL_REMOTE_MODE_AP
    // Keep station-mode OTA/status reachable without making the radio driver
    // preempt the Core 1 NAM stage continuously while the network is idle.
    // Active HTTP/OTA traffic wakes the modem automatically.
    if (esp_wifi_set_ps(WIFI_PS_MIN_MODEM) != ESP_OK)
        return false;
    if (esp_wifi_connect() != ESP_OK)
        return false;
#if COYOPEDAL_REMOTE_DISABLE_USB_AUDIO
    // Finish WPA/EAPOL and DHCP before model calibration, display DMA and UI
    // initialization contend for CPU and memory bandwidth. Reconnection remains
    // asynchronous after this bounded boot-time window.
    const EventBits_t ready =
        xEventGroupWaitBits(g_wifi_events, kWifiHasIp, pdFALSE, pdTRUE, pdMS_TO_TICKS(15000));
    if ((ready & kWifiHasIp) == 0U) {
        ESP_LOGW(kTag, "station did not acquire an IP during the 15-second boot window");
        if (g_wifi_task != nullptr) {
            vTaskPrioritySet(g_wifi_task, kWifiPriority);
        }
    }
#endif
#else
    if (g_wifi_task != nullptr) {
        vTaskPrioritySet(g_wifi_task, kWifiPriority);
    }
    ESP_LOGI(kTag, "maintenance AP %s ready at 192.168.4.1:%u", g_network_name,
             static_cast<unsigned>(kHttpPort));
    set_network_status(true, true, "CONNECTED - 192.168.4.1");
#endif
    return true;
}

void discovery_task(void*) {
    const int descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (descriptor < 0) {
        vTaskSuspend(nullptr);
        return;
    }
    timeval timeout{0, 200000};
    setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kDiscoveryPort);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0) {
        close(descriptor);
        vTaskSuspend(nullptr);
        return;
    }
    constexpr psa_algorithm_t kAlgorithm = PSA_ALG_HMAC(PSA_ALG_SHA_256);
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attributes, kAlgorithm);
    psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);
    psa_key_id_t key{};
    const psa_status_t imported =
        psa_import_key(&attributes, reinterpret_cast<const std::uint8_t*>(COYOPEDAL_REMOTE_TOKEN),
                       std::strlen(COYOPEDAL_REMOTE_TOKEN), &key);
    psa_reset_key_attributes(&attributes);
    if (imported != PSA_SUCCESS) {
        ESP_LOGE(kTag, "could not initialize authenticated discovery");
        close(descriptor);
        vTaskSuspend(nullptr);
        return;
    }

    constexpr char kDiscoveryPrefix[] = "COYOPEDAL_DISCOVER_V1 ";
    constexpr char kRealtimeStatusPrefix[] = "COYOPEDAL_REALTIME_V1 STATUS ";
    constexpr char kRealtimePausePrefix[] = "COYOPEDAL_REALTIME_V1 PAUSE ";
    constexpr char kRealtimeResumePrefix[] = "COYOPEDAL_REALTIME_V1 RESUME ";
    constexpr std::size_t kNonceSize = 32U;
    constexpr std::size_t kProofSize = 64U;
    while (g_discovery_running.load()) {
        char request[160]{};
        sockaddr_in peer{};
        socklen_t peer_size = sizeof peer;
        const int bytes = recvfrom(descriptor, request, sizeof request - 1U, 0,
                                   reinterpret_cast<sockaddr*>(&peer), &peer_size);
        const char* prefix = nullptr;
        std::size_t prefix_size = 0U;
        bool realtime = false;
        bool pause_requested = false;
        bool resume_requested = false;
        const auto select_prefix = [&](const char* const candidate, const std::size_t size) {
            return bytes == static_cast<int>(size + kNonceSize + 1U + kProofSize) &&
                   std::memcmp(request, candidate, size) == 0;
        };
        if (select_prefix(kDiscoveryPrefix, sizeof kDiscoveryPrefix - 1U)) {
            prefix = kDiscoveryPrefix;
            prefix_size = sizeof kDiscoveryPrefix - 1U;
        } else if (select_prefix(kRealtimeStatusPrefix, sizeof kRealtimeStatusPrefix - 1U)) {
            prefix = kRealtimeStatusPrefix;
            prefix_size = sizeof kRealtimeStatusPrefix - 1U;
            realtime = true;
        } else if (g_audio_mode.load() && !g_mode_busy.load() &&
                   select_prefix(kRealtimePausePrefix, sizeof kRealtimePausePrefix - 1U)) {
            prefix = kRealtimePausePrefix;
            prefix_size = sizeof kRealtimePausePrefix - 1U;
            realtime = true;
            pause_requested = true;
        } else if (g_audio_mode.load() && !g_mode_busy.load() &&
                   select_prefix(kRealtimeResumePrefix, sizeof kRealtimeResumePrefix - 1U)) {
            prefix = kRealtimeResumePrefix;
            prefix_size = sizeof kRealtimeResumePrefix - 1U;
            realtime = true;
            resume_requested = true;
        }
        if (prefix == nullptr) {
            continue;
        }
        const std::size_t signed_request_size = prefix_size + kNonceSize;
        if (request[signed_request_size] != ' ') {
            continue;
        }
        char expected[65]{};
        if (!mac_hex(key, request, signed_request_size, expected) ||
            !constant_time_equal(expected, request + signed_request_size + 1U)) {
            continue;
        }
        char nonce[kNonceSize + 1U]{};
        std::memcpy(nonce, request + prefix_size, kNonceSize);
        if (!std::all_of(nonce, nonce + kNonceSize, [](const char value) {
                return std::isxdigit(static_cast<unsigned char>(value)) != 0;
            })) {
            continue;
        }
        char mac_address[18];
        std::snprintf(mac_address, sizeof mac_address, "%02x:%02x:%02x:%02x:%02x:%02x", g_mac[0],
                      g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5]);
        char signed_response[192];
        const int signed_size = std::snprintf(
            signed_response, sizeof signed_response, "COYOPEDAL_RESPONSE_V1\n%s\n%s\n%u\n%s", nonce,
            mac_address, static_cast<unsigned>(kHttpPort), g_network_name);
        char response_proof[65]{};
        if (signed_size <= 0 || signed_size >= static_cast<int>(sizeof signed_response) ||
            !mac_hex(key, signed_response, static_cast<std::size_t>(signed_size), response_proof)) {
            continue;
        }
        bool paused = g_audio_pipeline_paused.load(std::memory_order_acquire);
        if (pause_requested) {
            paused = pause_audio_pipeline();
        } else if (resume_requested) {
            resume_audio_pipeline();
            paused = false;
        }
        usb_audio_diagnostics_t audio{};
        usb_audio_get_diagnostics(&audio);
        char response[896];
        std::snprintf(response, sizeof response,
                      "{\"device\":\"" COYOPEDAL_DEVICE_NAME "\",\"network\":\"%s\","
                      "\"port\":%u,\"mac\":\"%s\",\"nonce\":\"%s\","
                      "\"proof\":\"%s\",\"realtime\":%s,\"paused\":%s,"
                      "\"audio\":{\"connected\":%s,\"device_present\":%s,"
                      "\"captured_frames\":%llu,\"played_frames\":%llu,"
                      "\"input_drops\":%u,\"output_drops\":%u,"
                      "\"transfer_errors\":%llu,\"input_ring\":%u,"
                      "\"output_ring\":%u,\"stage_a_recent_cycles\":%u,"
                      "\"stage_b_recent_cycles\":%u,"
                      "\"usb_capture_callback_cycles\":%u,"
                      "\"usb_capture_callback_count\":%u,"
                      "\"usb_playback_callback_cycles\":%u,"
                      "\"usb_playback_callback_count\":%u,"
                      "\"usb_feedback_callback_cycles\":%u,"
                      "\"usb_feedback_callback_count\":%u,"
                      "\"usb_callback_cycles_per_block\":%u}}",
                      g_network_name, static_cast<unsigned>(kHttpPort), mac_address, nonce,
                      response_proof, realtime ? "true" : "false", paused ? "true" : "false",
                      audio.connected ? "true" : "false", audio.device_present ? "true" : "false",
                      static_cast<unsigned long long>(audio.captured_frames),
                      static_cast<unsigned long long>(audio.played_frames),
                      static_cast<unsigned>(audio.input_dropped_frames),
                      static_cast<unsigned>(audio.output_dropped_frames),
                      static_cast<unsigned long long>(audio.transfer_errors),
                      static_cast<unsigned>(audio.input_ring_frames),
                      static_cast<unsigned>(audio.output_ring_frames),
                      static_cast<unsigned>(audio.stage_a_recent_cycles),
                      static_cast<unsigned>(audio.stage_b_recent_cycles),
                      static_cast<unsigned>(audio.usb_capture_callback_cycles),
                      static_cast<unsigned>(audio.usb_capture_callback_count),
                      static_cast<unsigned>(audio.usb_playback_callback_cycles),
                      static_cast<unsigned>(audio.usb_playback_callback_count),
                      static_cast<unsigned>(audio.usb_feedback_callback_cycles),
                      static_cast<unsigned>(audio.usb_feedback_callback_count),
                      static_cast<unsigned>(audio.usb_callback_cycles_per_block));
        sendto(descriptor, response, std::strlen(response), 0, reinterpret_cast<sockaddr*>(&peer),
               peer_size);
    }
    psa_destroy_key(key);
    close(descriptor);
    vTaskSuspend(nullptr);
}

void validate_running_ota() {
    const esp_partition_t* const running = esp_ota_get_running_partition();
    esp_ota_img_states_t state{};
    if (running != nullptr && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        // This runs only after model calibration and every persistent DSP
        // allocation has succeeded. Validate inline rather than on a temporary
        // task, whose internal stack would fragment the final Wi-Fi/USB DMA
        // block on the first boot into a newly written OTA slot.
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            ESP_LOGI(kTag, "OTA image completed startup; marked valid");
        }
    }
}

} // namespace

void coyopedal_remote_capture_logs() {
    if (g_console_vprintf == nullptr) {
        adopt_or_reset_log_ring();
        g_console_vprintf = esp_log_set_vprintf(remote_vprintf);
        (void)esp_register_shutdown_handler(log_ring_writeback);
        report_audio_window_result();
    }
}

bool coyopedal_remote_maintenance_boot_requested() {
    bool requested = maintenance_boot_requested();
    if (requested)
        (void)select_maintenance_boot(false);
    if (g_maintenance_return_magic == kMaintenanceReturnMagic) {
        g_maintenance_return_magic = 0U;
        requested = true;
    }
    return requested;
}

bool coyopedal_remote_audio_boot_requested() {
    // Audio is the default boot, so this answers whether audio was asked for
    // explicitly: only an explicit request still goes to audio after a crash.
    bool requested = consume_exclusive_audio_boot();
    if (g_audio_window_magic == kAudioWindowMagic) {
        requested = true;
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    const std::uint32_t pending = g_effect_profile_store.magic;
    if (pending == kEffectProfileRequestMagic || pending == kEffectSoakRequestMagic) {
        requested = true;
    }
    // Consume this one-shot here rather than at the end of startup: if it were
    // cleared only after the audio graph, USB and UI were up, a crash on the way
    // there would leave it set and loop the board in audio, off the network.
    if (consume_return_maintenance_boot()) {
        requested = true;
    }
    return requested;
}

void coyopedal_remote_arm_audio_window() {
    std::atomic_thread_fence(std::memory_order_acquire);
    if (g_audio_window_magic != kAudioWindowMagic) {
        return;
    }
    // A fresh window invalidates the previous result.
    g_audio_window_result.magic = 0U;
    const std::uint32_t seconds = g_audio_window_seconds;
    g_audio_window_engage = (g_audio_window_flags & kAudioWindowEngage) != 0U;
    g_audio_window_mask_requested = (g_audio_window_flags & kAudioWindowMask) != 0U;
    g_audio_window_no_ui = (g_audio_window_flags & kAudioWindowNoUi) != 0U;
    g_audio_window_swap_cores = (g_audio_window_flags & kAudioWindowSwapCores) != 0U;
    g_audio_window_ui_soak = (g_audio_window_flags & kAudioWindowUiSoak) != 0U;
    usb_audio_swap_stage_cores(g_audio_window_swap_cores);
    g_audio_window_mask = static_cast<std::uint8_t>(g_audio_window_flags >> kAudioWindowMaskShift);
    g_audio_window_model_requested = (g_audio_window_flags & kAudioWindowModel) != 0U;
    g_audio_window_model =
        static_cast<std::uint8_t>(g_audio_window_flags >> kAudioWindowModelShift);
    // One shot. A second audio boot without a fresh AUDIO TRY stays in audio.
    g_audio_window_magic = 0U;
    g_audio_window_flags = 0U;
    if (seconds < 5U || seconds > 120U) {
        return;
    }
    const esp_timer_create_args_t args{.callback = audio_window_expired,
                                       .arg = nullptr,
                                       .dispatch_method = ESP_TIMER_TASK,
                                       .name = "audio_window",
                                       .skip_unhandled_events = false};
    if (esp_timer_create(&args, &g_audio_window_timer) != ESP_OK ||
        esp_timer_start_once(g_audio_window_timer,
                             static_cast<std::uint64_t>(seconds) * 1000000ULL) != ESP_OK) {
        ESP_LOGE(kTag, "audio window timer could not be armed; this boot cannot return by itself");
        return;
    }
    ESP_LOGW(kTag, "audio window armed: restarting into maintenance in %lu s no matter what%s",
             static_cast<unsigned long>(seconds),
             g_audio_window_engage ? "; pedal will be engaged" : "");
}

// Loading a capture the card has not prepared yet runs the sweep tuner, which
// needs about 8 KB of stack. The UI task that loads models for the player has
// that; this is called on main, whose 4 KB stack does not. So the load gets a
// one-shot task of its own, with its stack in PSRAM: the tuner never runs with
// the cache off, and internal SRAM is what the graph needs.
namespace {
struct AudioWindowModelLoad {
    TaskHandle_t waiter;
    bool loaded;
};

void audio_window_model_task(void* argument) {
    auto* const load = static_cast<AudioWindowModelLoad*>(argument);
    load->loaded = coyopedal_ui_control_set_model(g_audio_window_model);
    xTaskNotifyGive(load->waiter);
    vTaskSuspend(nullptr);
}

bool audio_window_load_model() {
    constexpr std::size_t kStackBytes = 16U * 1024U;
    auto* const stack = static_cast<StackType_t*>(
        heap_caps_malloc(kStackBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto* const tcb = static_cast<StaticTask_t*>(
        heap_caps_calloc(1U, sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    AudioWindowModelLoad load{xTaskGetCurrentTaskHandle(), false};
    TaskHandle_t task = nullptr;
    if (stack != nullptr && tcb != nullptr) {
        task = xTaskCreateStaticPinnedToCore(audio_window_model_task, "window_model", kStackBytes,
                                             &load, uxTaskPriorityGet(nullptr), stack, tcb, 1);
    }
    if (task == nullptr) {
        heap_caps_free(stack);
        heap_caps_free(tcb);
        ESP_LOGE(kTag, "audio window: no task for the model load");
        return false;
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    vTaskDelete(task);
    heap_caps_free(stack);
    heap_caps_free(tcb);
    return load.loaded;
}
// Drives the UI for a UISOAK window with real taps. Not a control write and a
// repaint -- those measure ~2 ms and are not what a player waits on -- but the
// whole path a finger takes: TouchRuntime dispatch, hit test, the JSX handler,
// the store write and whatever repaint follows.
//
// queueTouchEvent takes panel-physical coordinates and rotates them itself, so
// the target is found rather than derived: each candidate is mapped through
// transformTouchToLogical and logged next to the effect mask it did or did not
// change. The block dots sit at CSS left 12, top 108 in a 251x205 canvas at a
// device pixel ratio of 2, so slot 0's centre is logical (56, 262).
struct UiSoakPoint {
    int x;
    int y;
};
// The seven chain dots, in panel-physical coordinates. The panel is rotated, so
// a logical (x, y) sits at physical (y, 501 - x); the dots run along logical x
// at 56/121/186/251/316/381/446 with their row centred on logical y 262. Every
// tap therefore toggles a block, which is the case worth timing -- points that
// miss only measure the harness's own sleeps, and points that hit the preset
// hero or a pill switch screens, which legitimately repaints everything.
constexpr UiSoakPoint kUiSoakPoints[] = {{262, 445}, {262, 380}, {262, 315}, {262, 250},
                                         {262, 185}, {262, 120}, {262, 56}};
constexpr unsigned kUiSoakHoldMs = 80U;
constexpr unsigned kUiSoakGapMs = 1200U;
// How long the panel must stay unpainted before a tap counts as finished, and
// the hard cap so one pathological repaint cannot stall the whole soak.
constexpr std::int64_t kUiSoakQuietUs = 250000;
constexpr std::int64_t kUiSoakSettleCapUs = 6000000;
unsigned ui_soak_effect_mask() {
    unsigned mask = 0U;
    for (unsigned block = 0; block < COYOPEDAL_FX_BLOCK_COUNT; ++block)
        if (coyopedal_fx_enabled(static_cast<coyopedal_fx_block_t>(block)))
            mask |= 1U << block;
    return mask;
}
void audio_window_ui_soak_task(void*) {
    constexpr unsigned kPointCount = sizeof kUiSoakPoints / sizeof *kUiSoakPoints;
    unsigned index = 0U;
    // Do not start tapping until the panel exists. With the pedal ENGAGED the amp
    // graph owns ~95% of both cores, so everything the runtime does before the
    // first frame runs ~20x slower: Display::init lands 20 s after the runtime
    // starts instead of 1 s. Taps fired into that window hit no UI at all --
    // every row reads "mask 63 -> 63 (no change), flushes=0 px=0" and the whole
    // run measures nothing, which is exactly how an engaged run got mistaken for
    // a healthy one. The odometer moving off zero is proof a real frame reached
    // the panel.
    {
        const std::int64_t waitStart = esp_timer_get_time();
        while (esp_timer_get_time() - waitStart < 90000000) {
            std::uint32_t calls = 0U;
            std::uint64_t px = 0U;
            gea::platform::display::Display::flushOdometerRead(calls, px);
            if (calls > 0U)
                break;
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        ESP_LOGW(kTag, "ui soak: panel first paint after %lld us; tapping now",
                 static_cast<long long>(esp_timer_get_time() - waitStart));
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    while (true) {
        const UiSoakPoint point = kUiSoakPoints[index % kPointCount];
        ++index;
        int logical_x = point.x;
        int logical_y = point.y;
        gea::framework::events::TouchRuntime::transformTouchToLogical(&logical_x, &logical_y);
        // Do NOT re-arm the requested mask here. Forcing the native blocks back on
        // behind the UI's back desynchronises the two: the store still believes
        // every block is enabled, the tap writes a value it already holds, nothing
        // in the tree changes and no frame paints at all (measured: every row
        // "no change", flushes=0 px=0, for a whole 120 s window). The soak's own
        // cycle already sweeps the chain off and back on across its seven points,
        // so the rows to compare are the ones taken while it is still nearly full.
        const unsigned before = ui_soak_effect_mask();
        std::uint32_t calls0 = 0U;
        std::uint64_t px0 = 0U;
        gea::platform::display::Display::flushOdometerRead(calls0, px0);
        const std::int64_t started = esp_timer_get_time();
        gea::framework::events::TouchRuntime::queueTouchEvent(
            gea::framework::events::TouchPhase::Down, true, point.x, point.y, 0);
        vTaskDelay(pdMS_TO_TICKS(kUiSoakHoldMs));
        gea::framework::events::TouchRuntime::queueTouchEvent(
            gea::framework::events::TouchPhase::Up, false, point.x, point.y, 0);
        // The old harness slept a fixed 80+400ms and printed the elapsed time,
        // so the number it reported was those two sleeps -- it never waited for
        // anything and could not see a repaint at all. Wait for the work
        // instead: the mask flipping says the store reacted, the flush odometer
        // going quiet says the panel has stopped being painted. Those are
        // different instants and the gap between them is the visible one.
        std::int64_t maskUs = 0;
        std::int64_t lastFlushUs = started;
        std::uint32_t calls = calls0;
        std::uint64_t px = px0;
        unsigned after = before;
        while (true) {
            const std::int64_t now = esp_timer_get_time();
            if (now - started > kUiSoakSettleCapUs)
                break;
            std::uint32_t c = 0U;
            std::uint64_t p = 0U;
            gea::platform::display::Display::flushOdometerRead(c, p);
            if (c != calls || p != px) {
                calls = c;
                px = p;
                lastFlushUs = now;
            }
            if (maskUs == 0) {
                after = ui_soak_effect_mask();
                if (after != before)
                    maskUs = now - started;
            }
            // Quiet once nothing has been painted for a while, but never before
            // the release plus a click's grace, or a slow first frame reads as
            // "already finished".
            if (now - started > static_cast<std::int64_t>(kUiSoakHoldMs + 200) * 1000 &&
                now - lastFlushUs > kUiSoakQuietUs)
                break;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (maskUs == 0)
            after = ui_soak_effect_mask();
        // No full-panel probe here. It belongs to the UI FLUSH command, which runs
        // in maintenance where the cores are free. Doing real panel work from this
        // task inside an ENGAGED window measures the task, not the panel: at
        // priority 2 under a 95%-loaded pair of cores a single full flush did not
        // return for 45 s and swallowed the entire run, so the taps it was meant
        // to annotate never happened. The per-frame scheduler dump reports the
        // flush cost from the frame task, where it actually occurs.
        ESP_LOGW(kTag,
                 "ui soak: tap phys=%d,%d logical=%d,%d mask %u -> %u %s | mask=%lldus "
                 "paint=%lldus flushes=%lu px=%llu",
                 point.x, point.y, logical_x, logical_y, before, after,
                 before == after ? "(no change)" : "CHANGED", static_cast<long long>(maskUs),
                 static_cast<long long>(lastFlushUs - started),
                 static_cast<unsigned long>(calls - calls0),
                 static_cast<unsigned long long>(px - px0));
        vTaskDelay(pdMS_TO_TICKS(kUiSoakGapMs));
    }
}
} // namespace

void coyopedal_remote_audio_window_configure() {
    if (g_audio_window_engage && !s3_v1_ui_mode(7)) {
        ESP_LOGE(kTag, "audio window: could not engage the pedal");
    }
    if (g_audio_window_mask_requested && !apply_effect_mask(g_audio_window_mask)) {
        ESP_LOGE(kTag, "audio window: mask %u could not be applied",
                 static_cast<unsigned>(g_audio_window_mask));
    }
    // The model goes after the pedal state and the mask: the panel's engage
    // exchange is not reliable when it follows a model load, and the load
    // touches neither of them.
    if (g_audio_window_model_requested) {
        if (audio_window_load_model()) {
            ESP_LOGW(kTag, "audio window: model %u (%s) loaded",
                     static_cast<unsigned>(g_audio_window_model),
                     coyopedal_models[g_audio_window_model].id);
        } else {
            ESP_LOGE(kTag, "audio window: model %u could not be loaded",
                     static_cast<unsigned>(g_audio_window_model));
        }
    }
    if (g_audio_window_engage || g_audio_window_mask_requested || g_audio_window_model_requested ||
        g_audio_window_no_ui) {
        unsigned mask = 0U;
        for (unsigned block = 0; block < COYOPEDAL_FX_BLOCK_COUNT; ++block)
            if (coyopedal_fx_enabled(static_cast<coyopedal_fx_block_t>(block)))
                mask |= 1U << block;
        ESP_LOGW(kTag, "audio window configured: engaged=%u amp=%u mask=%u ui=%s cores=%s",
                 coyopedal_pedal_dsp_pedal_bypassed() ? 0U : 1U,
                 coyopedal_pedal_dsp_bypassed() ? 0U : 1U, mask,
                 g_audio_window_no_ui ? "suspending" : "running",
                 g_audio_window_swap_cores ? "swapped" : "normal");
    }
    // Start the window's own accounting here: blocks, USB interrupt time and
    // per-task run time from now to the expiry, not from the boot.
    g_audio_window_start_frames = usb_audio_captured_frames();
    usb_audio_window_reset();
    {
        usb_audio_diagnostics_t at_start{};
        usb_audio_get_diagnostics(&at_start);
        g_audio_window_start_silent = at_start.silent_frames;
        g_audio_window_start_trimmed = at_start.trimmed_frames;
    }
    if (&pedalboard_usb_isr_cycles != nullptr) {
        g_audio_window_usb_isr_cycles_start = pedalboard_usb_isr_cycles;
        g_audio_window_usb_isr_count_start = pedalboard_usb_isr_count;
    }
#if configGENERATE_RUN_TIME_STATS && configUSE_TRACE_FACILITY
    if (g_task_snapshot_start == nullptr) {
        g_task_snapshot_start = static_cast<TaskStatus_t*>(
            heap_caps_malloc(2U * kTaskSnapshotMax * sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM));
    }
    if (g_task_snapshot_start != nullptr) {
        g_task_snapshot_start_count =
            uxTaskGetSystemState(g_task_snapshot_start, kTaskSnapshotMax, nullptr);
        g_task_snapshot_start_us = esp_timer_get_time();
    }
#endif
    if (g_audio_window_ui_soak && !g_audio_window_no_ui) {
        // Priority 2 is below every UI task it drives (v1_pump 3, app_frame 5),
        // so the soak can never be what the measurement finds busy: if the UI
        // is slow, it is slow around this, not because of it.
        if (xTaskCreatePinnedToCoreWithCaps(audio_window_ui_soak_task, "ui_soak", 3072, nullptr, 6,
                                            nullptr, 0,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            ESP_LOGE(kTag, "audio window: could not start the UI soak");
        } else {
            ESP_LOGW(kTag, "audio window: UI soak switching screens every %u ms", kUiSoakHoldMs);
        }
    }
    if (g_audio_window_no_ui) {
        // Last, and nothing is logged once the first task is stopped: a render
        // task suspended while it holds the log lock stalls whoever logs next.
        // Let the engage/mask frames finish before the render tasks stop; the
        // window ends in a restart, so nothing resumes them.
        vTaskDelay(pdMS_TO_TICKS(250));
        ESP_LOGW(kTag, "audio window: suspending the UI tasks");
        for (const char* const name : {"gea_runtime", "app_frame", "gea_rwrk", "v1_pump"}) {
            const TaskHandle_t task = xTaskGetHandle(name);
            if (task != nullptr) {
                vTaskSuspend(task);
            }
        }
    }
}

void coyopedal_remote_mark_audio_boot() {
    g_audio_mode.store(true, std::memory_order_release);
}

void coyopedal_remote_validate_running_ota() {
    validate_running_ota();
}

void coyopedal_remote_sync_network_ui() {
    if (s3_v1_network_status != nullptr) {
        const bool enabled = g_wifi_started.load(std::memory_order_acquire);
        s3_v1_network_status(enabled, enabled && g_wifi_connected.load(std::memory_order_acquire),
                             g_network_name, g_network_detail);
    }
}

void coyopedal_remote_prepare_runtime() {
    // lwIP cannot be deinitialized by ESP-IDF. Place its persistent task and
    // synchronization objects before NAM, without starting a network or radio.
    ESP_ERROR_CHECK(esp_netif_init());
    // ESP-IDF initializes PSA at startup, but its key-slot mutex is lazy and
    // survives crypto_free(). Materialize that small permanent lock before
    // NAM reserves its contiguous SRAM. Querying the null key takes the lock
    // without importing a key, allocating a cache, or accessing storage.
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    const auto status = psa_get_key_attributes(PSA_KEY_ID_NULL, &attributes);
    psa_reset_key_attributes(&attributes);
    if (status != PSA_ERROR_INVALID_HANDLE)
        ESP_LOGE(kTag, "crypto lock preparation failed: %ld", static_cast<long>(status));
}

bool coyopedal_remote_start() {
    if (g_wifi_initialized)
        return g_server != nullptr;
    validate_running_ota();
#if !COYOPEDAL_REMOTE_ENABLED
    ESP_LOGW(kTag, "remote_config.h missing; run tools/configure_remote.py before building");
    return false;
#else
    static_assert(sizeof(COYOPEDAL_REMOTE_TOKEN) - 1U >= 32U,
                  "COYOPEDAL_REMOTE_TOKEN must contain at least 32 characters");
    static_assert(sizeof(COYOPEDAL_REMOTE_WIFI_PASSWORD) - 1U >= 8U,
                  "Wi-Fi password must contain at least 8 characters");
    if (psa_crypto_init() != PSA_SUCCESS || !start_wifi() || !start_http_server()) {
        ESP_LOGE(kTag, "wireless maintenance startup failed");
        if (g_wifi_started.load(std::memory_order_acquire)) {
            set_network_status(true, g_wifi_connected.load(std::memory_order_acquire),
                               "WI-FI OK - OTA SERVICE FAILED");
        } else {
            set_network_status(false, false, "WI-FI START FAILED");
        }
        return false;
    }
    // Discovery only waits on a UDP socket.  Keep its cold 4 KiB stack out of
    // the internal heap reserved for USB DMA and the realtime audio tasks.
    g_discovery_running.store(true);
    if (xTaskCreatePinnedToCoreWithCaps(discovery_task, "remote_discovery", 4096, nullptr,
                                        kDiscoveryPriority, &g_discovery_task, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        g_discovery_running.store(false);
        ESP_LOGW(kTag, "UDP discovery unavailable; direct IP still works");
    }
    ESP_LOGI(kTag, "authenticated status, logs, commands and OTA ready on port %u",
             static_cast<unsigned>(kHttpPort));
    return true;
#endif
}

bool pedalboard_ble_start();
bool pedalboard_audio_unload();

namespace {
void mode_task(void*) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const bool entering = g_audio_mode.load();
        // Freeze controls on the UI task before touching the audio graph.
        if (!s3_v1_ui_mode(1)) {
            g_mode_busy.store(false);
            continue;
        }
        if (entering) {
            // Leaving the pedalboard reboots, for the same reason returning
            // does: the radios cannot be brought up in a heap the graph has
            // already shaped. Starting the BLE controller in place fails a
            // malloc in BLE_INIT and then asserts inside the ROM on core 1. The
            // flags are written first, the UI is told, and the next boot brings
            // the radios up from an untouched heap.
            if (!select_maintenance_boot(true) || !select_return_maintenance_boot(false)) {
                set_network_status(g_wifi_started.load(), g_wifi_connected.load(),
                                   "Could not select the maintenance boot; retry");
                s3_v1_ui_mode(3);
                g_mode_busy.store(false);
                continue;
            }
            set_network_status(g_wifi_started.load(), g_wifi_connected.load(),
                               "Rebooting into maintenance mode");
            schedule_restart();
        } else {
            // Returning to the pedalboard reboots instead of loading the graph
            // in place.
            //
            // The graph wants about 213 KB of internal SRAM. A maintenance boot
            // has spent the same memory on Wi-Fi, the HTTP server, BLE and the
            // display's staging buffers, and shutting the radios down gives it
            // back fragmented: plenty of bytes free, but no usable 32 KiB
            // aligned block for the coefficient tables or the rings. Nothing can
            // defragment a live heap; a boot starts from an unfragmented one,
            // which costs about three seconds and cannot fail this way.
            //
            // Clearing the maintenance flags only removes a veto. The
            // exclusive-audio flag asks for audio explicitly, which
            // gea_app_native_boot() honours even when the reset reason would
            // otherwise send the board to maintenance.
            if (!select_maintenance_boot(false) || !select_return_maintenance_boot(false) ||
                !select_exclusive_audio_boot(true)) {
                set_network_status(g_wifi_started.load(), g_wifi_connected.load(),
                                   "Could not select the audio boot; retry");
                s3_v1_ui_mode(1);
                g_mode_busy.store(false);
                continue;
            }
            set_network_status(g_wifi_started.load(), g_wifi_connected.load(),
                               "Rebooting into audio mode");
            schedule_restart();
        }
    }
}
} // namespace

void coyopedal_remote_mode_start() {
    if (g_mode_task)
        return;
    // The stack is static internal RAM, in RTC fast memory (see
    // g_mode_task_stack). It must be internal: this task's whole job is the
    // audio/maintenance transition, and both directions touch flash (NVS boot
    // flags, esp_wifi_init() opening NVS). A flash operation disables the
    // cache, and ESP-IDF asserts esp_task_stack_is_sane_cache_disabled() for
    // any task running on a stack in external RAM. Same reason the NimBLE host
    // task is internal (see services/ble_discovery.cpp).
    g_mode_task = xTaskCreateStaticPinnedToCore(mode_task, "mode_switch", kModeTaskStackBytes,
                                                nullptr, 4, g_mode_task_stack, &g_mode_task_tcb, 0);
    if (g_mode_task == nullptr)
        ESP_LOGE(kTag, "could not start live mode worker");
}

bool coyopedal_remote_mode_busy() {
    return g_mode_busy.load();
}

void coyopedal_remote_toggle_mode() {
    if (!g_mode_task || g_mode_busy.exchange(true))
        return;
    xTaskNotifyGive(g_mode_task);
}

void coyopedal_remote_prepare_requested_diagnostics() {
    std::atomic_thread_fence(std::memory_order_acquire);
    const std::uint32_t request = g_effect_profile_store.magic;
    if (request == kEffectProfileRequestMagic || request == kEffectSoakRequestMagic ||
        g_return_maintenance_after_startup || return_maintenance_boot_requested()) {
        // Raise app_main before the USB/DSP workers are created. A chain which
        // saturates both cores must not prevent the profiler or OTA validation
        // path from reaching its bounded stop-and-restart code.
        vTaskPrioritySet(nullptr, kEffectProfilerPriority);
    }
}

void coyopedal_remote_start_requested_diagnostics() {
    std::atomic_thread_fence(std::memory_order_acquire);
    const std::uint32_t request = g_effect_profile_store.magic;
    if (request == kEffectProfileRequestMagic || request == kEffectSoakRequestMagic) {
        g_effect_profile_running.store(true, std::memory_order_release);
        const std::uint32_t seconds = g_effect_profile_store.seconds;
        const bool synthetic_input = g_effect_profile_store.synthetic_input != 0U;
        const bool cycle_telemetry = g_effect_profile_store.cycle_telemetry != 0U;
        g_effect_profile_store.magic = 0U;
        // app_main already owns a live task and the profiler never returns.
        // Running on that task avoids allocating another TCB after USB, UI and
        // the promoted effect delay lines have deliberately packed internal
        // SRAM, so the measured heap is identical to normal audio use.
        delayed_effect_profile(seconds, request == kEffectSoakRequestMagic, synthetic_input,
                               cycle_telemetry);
    } else if (consume_return_maintenance_boot()) {
        // An OTA or REBOOT sent from maintenance boots into audio first, so the
        // NAM arenas are placed during full startup, and leaves this one-shot
        // request to go back to maintenance once that startup is complete.
        g_effect_profile_store.magic = 0U;
        g_return_maintenance_after_startup = false;
        (void)select_return_maintenance_boot(false);
        ESP_LOGI(kTag, "post-OTA audio startup complete; returning to maintenance");
        coyopedal_remote_toggle_mode();
    }
}

bool coyopedal_remote_audio_mode() {
    return g_audio_mode.load(std::memory_order_acquire);
}
