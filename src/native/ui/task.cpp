// The render loop, the panel driver and touch all belong to the Gea runtime
// (@geastack/targets). What is left here is the pedal's own bookkeeping around
// it: the library init that must complete before the JSX mounts, a low-priority
// pump for the tuner, and the mode requests
// the maintenance service issues from its own task.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "model_catalog.h"
#include "factory_presets.h"
#include "flash_storage.h"
#include "board_bridge.hpp"
#include "board_ui.h"
#include "control.h"
#include "audio/effects.h"
#include "audio/tuner.h"
#include "audio/usb_frame_processor.h"
#include "remote_service.h"

#include "display.h"
#include "event.h"
#include "services/frame_scheduler.h"
#include "ui/tree_internal.h"

void s3_v1_ui_wake();
bool coyopedal_library_init();

namespace {

constexpr char kTag[] = "v1_ui";
TaskHandle_t pump_task_handle{};
std::atomic<std::uint32_t> ui_runs{};
std::atomic<std::uint32_t> ui_notifies{};
std::atomic<std::uint32_t> ui_tuner_waits{};
std::atomic<bool> maintenance_screen{};
char maintenance_ssid[33]{};
char maintenance_detail[64]{"STARTING"};

// Does anything actually reach the panel? The driver reports the panel ready
// after a full-screen flush completes, so a blank screen can only mean the
// framebuffer it sent was black -- which is either "the JSX never mounted" or
// "it mounted and painted nothing". These two numbers separate those: the node
// count is the mounted tree, and countNonBlackPixels reads back the pixels the
// panel was last given.
void log_panel_census() {
    const int nodes = gea::embedded::ui::Tree::instance().nodeCount();
    const int painted = gea::platform::display::Display::countNonBlackPixels(true);
    const auto* const canvas = gea::platform::display::Display::canvas();
    ESP_LOGI(kTag, "panel census: nodes=%d non_black_px=%d canvas=%dx%d brightness=%d", nodes,
             painted, canvas != nullptr ? canvas->width() : -1,
             canvas != nullptr ? canvas->height() : -1,
             gea::platform::display::Display::brightness());
}

// The tuner needs a steady pitch update, which is not a frame and does not
// belong on the render task. 100 ms idle, 16 ms while the tuner is open.
TaskHandle_t pump_task_storage{};
bool node_ceiling_warned{};

void pump_task(void*) {
    while (true) {
        ui_runs.fetch_add(1U, std::memory_order_relaxed);
        const std::uint32_t run = ui_runs.load(std::memory_order_relaxed);
        if (run == 30U)
            log_panel_census();
        // GEA_EMBEDDED_MAX_NODES is 160 for this app against the framework default
        // of 512, which is what makes the per-node style marker arrays affordable
        // in internal SRAM (each is kMaxNodes * 2 bytes, and there are several).
        // The tree is about 54 nodes and the amp library is a virtual list, so the
        // budget is deliberate -- but a silent overrun would corrupt style state, so
        // say so once the tree gets within a quarter of the ceiling. Silent in the
        // normal case: this must not pollute the maintenance log ring.
        if (!node_ceiling_warned && gea::embedded::ui::Tree::instance().nodeCount() > 120) {
            node_ceiling_warned = true;
            ESP_LOGW(kTag, "UI tree reached %d nodes against a GEA_EMBEDDED_MAX_NODES of 160",
                     gea::embedded::ui::Tree::instance().nodeCount());
        }
        // How close does a touch-driven re-render come to the render task's
        // stack? Reported once, next to the panel census. Never put a periodic
        // probe here: it would fill the maintenance log ring and push out the
        // boot log, the one place that records whether the A2-Full graph loaded.
        if (run == 30U) {
            const TaskHandle_t render = xTaskGetHandle("gea_runtime");
            if (render != nullptr)
                ESP_LOGI(kTag, "render stack headroom: %u bytes",
                         static_cast<unsigned>(uxTaskGetStackHighWaterMark(render)));
        }
        TickType_t wait = pdMS_TO_TICKS(100);
        if (!coyopedal_remote_mode_busy()) {
            if (coyopedal_pedal_dsp_tuner_active()) {
                coyopedal_tuner_update();
                s3_v1_ui_wake();
                ui_tuner_waits.fetch_add(1U, std::memory_order_relaxed);
                wait = pdMS_TO_TICKS(16);
            }
        }
        if (ulTaskNotifyTake(pdTRUE, wait) != 0U)
            ui_notifies.fetch_add(1U, std::memory_order_relaxed);
    }
}

bool initialize_ui() {
    if (maintenance_screen) {
        pedalboard_panel_network(maintenance_ssid, maintenance_detail);
        return pedalboard_panel_maintenance_init();
    }
    return coyopedal_library_init();
}

bool start_ui() {
    if (!initialize_ui())
        return false;
    // The pump's stack is in PSRAM; the task control block stays internal. By
    // the time the UI starts, the A2-Full graph has left internal SRAM with no
    // block large enough for a 4 KiB stack. The pump never touches flash, so a
    // PSRAM stack is safe. The boot preset does not depend on the pump;
    // coyopedal_controls_init() recalls it from coyopedal_ui_init().
    if (pump_task_handle == nullptr) {
        constexpr std::uint32_t kPumpStackBytes = 4096U;
        if (xTaskCreatePinnedToCoreWithCaps(pump_task, "v1_pump", kPumpStackBytes, nullptr, 3,
                                            &pump_task_storage, 0,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS)
            pump_task_handle = pump_task_storage;
        if (pump_task_handle == nullptr) {
            ESP_LOGE(kTag,
                     "UI pump unavailable: the tuner will not update; internal free=%u "
                     "largest=%u PSRAM free=%u",
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                     static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        }
    }
    return true;
}

} // namespace

// The library is not the panel. The flash store, the model catalog, the preset
// table and the control model are what the audio graph and the remote API read;
// the screen is only one of their readers, and on a board without one they all
// still have to come up. So this is what a boot needs either way, and the panel
// build reaches it through the same call.
bool coyopedal_library_init() {
    const bool storage_ready = coyopedal_flash_store_init();
    const bool models_ready = coyopedal_models_init();
    const bool presets_ready = coyopedal_presets_init();
    ESP_LOGI(kTag, "V1 library: %u models, %u presets", coyopedal_model_count,
             coyopedal_preset_count());
    if (!storage_ready || !models_ready || !presets_ready)
        ESP_LOGW(kTag, "factory library partition is incomplete");
    // coyopedal_controls_init() runs from here and recalls the boot preset, so
    // the library above has to be loaded first or there is nothing to recall.
    if (!coyopedal_ui_init())
        return false;
    coyopedal_ui_set_usb(COYOPEDAL_UI_USB_MOUNTED);
    return true;
}

bool s3_v1_ui_start() {
    maintenance_screen = false;
    return start_ui();
}

bool s3_v1_maintenance_status_start(const char* const ssid) {
    maintenance_screen = true;
    std::snprintf(maintenance_ssid, sizeof maintenance_ssid, "%s", ssid != nullptr ? ssid : "-");
    std::snprintf(maintenance_detail, sizeof maintenance_detail, "STARTING");
    return start_ui();
}

extern "C" void s3_v1_network_status(const bool enabled, const bool connected,
                                     const char* const ssid, const char* const detail) {
    if (maintenance_screen) {
        std::snprintf(maintenance_ssid, sizeof maintenance_ssid, "%s",
                      ssid != nullptr ? ssid : "-");
        std::snprintf(maintenance_detail, sizeof maintenance_detail, "%s",
                      detail != nullptr ? detail
                                        : (connected ? "CONNECTED"
                                           : enabled ? "CONNECTING"
                                                     : "OFF"));
        pedalboard_panel_network(ssid, detail);
    }
    s3_v1_ui_wake();
}

// Ask the runtime for a frame. The scheduler coalesces these, so a burst of
// status updates costs one repaint.
void s3_v1_ui_wake() {
    gea::framework::events::Event event{};
    event.type = gea::framework::events::EventType::Frame;
    (void)gea::framework::services::FrameScheduler::sendEvent(event);
    if (pump_task_handle != nullptr)
        xTaskNotifyGive(pump_task_handle);
}

extern "C" void s3_v1_ui_counters_reset() {
    ui_runs.store(0U, std::memory_order_relaxed);
    ui_notifies.store(0U, std::memory_order_relaxed);
    ui_tuner_waits.store(0U, std::memory_order_relaxed);
}

extern "C" void s3_v1_ui_counters_log() {
    ESP_LOGI(kTag, "pump counters runs=%u notify=%u tuner=%u screen=%s",
             ui_runs.load(std::memory_order_relaxed), ui_notifies.load(std::memory_order_relaxed),
             ui_tuner_waits.load(std::memory_order_relaxed), coyopedal_ui_screen_name());
}

bool s3_v1_ui_mode(const unsigned mode) {
    if (mode <= 3)
        maintenance_screen.store(mode == 1);
    const bool ok = pedalboard_panel_mode(mode);
    s3_v1_ui_wake();
    return ok;
}
