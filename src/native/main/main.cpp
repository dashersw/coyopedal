// Boot and memory placement for the pedal: the panic record, the effects' memory,
// the audio and maintenance boot paths, and the order the DSP, USB host and UI
// claim internal SRAM in.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "driver/gpio.h"
#include "esp_cpu.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_heap_caps_init.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_private/panic_internal.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "audio/effects.h"
// Defined by the NAM S3-native graph. Declared here rather than pulled in
// through a header because main.cpp has no other reason to see that component's
// interface.
extern "C" void coyopedal_nam_bank_arena_lend_to_heap(void);
extern "C" void* coyopedal_nam_bank_arena_take_effects(size_t bytes);
extern "C" bool coyopedal_nam_bank_arena_owns_ptr(const void* block);

#include "audio/processor.hpp"
#include "usb_audio.h"
#include "remote_service.h"
#include "model_catalog.h"
#include "factory_presets.h"
#include "flash_storage.h"
#include "heap_map.h"
#include "memory.h"
#include "runtime.h"
#include "control.h"

#if __has_include("remote_config.h")
#include "remote_config.h"
#else
#define COYOPEDAL_REMOTE_DISABLE_USB_AUDIO 0
#define COYOPEDAL_REMOTE_WIFI_SSID ""
#endif

bool s3_v1_ui_start();
void s3_v1_ui_wake();
bool coyopedal_library_init();
bool pedalboard_ble_start();
bool s3_v1_maintenance_status_start(const char* ssid);

extern "C" const std::uint8_t kModelStart[] asm("_binary_fallback_model_namb_start");
extern "C" const std::uint8_t kModelEnd[] asm("_binary_fallback_model_namb_end");

// Shared with usb_audio.cpp's two-stage DSP pipeline.
coyopedal::pedal::Processor g_engine;

// A panic must survive into the next boot. The board's only console is its USB
// port, and in audio mode that port is a host for the guitar interface, so the
// panic is recorded in RTC memory and reported to the log ring on the next boot.
// RTC memory keeps its contents across a panic reset but holds garbage after
// power-on, so a magic word gates the report.
extern "C" void __real_esp_panic_handler(panic_info_t* info);
// IDF keeps the abort() message here, not in panic_info_t: panic_abort() stores
// the caller's string in g_panic_abort_details and then executes an illegal
// instruction, so the handler sees a generic illegal-instruction frame whose
// addr is panic_abort itself. The string and the task name say which assert
// fired.
extern "C" bool g_panic_abort;
extern "C" char* g_panic_abort_details;
constexpr std::uint32_t kPanicRecordMagic = 0x54415552U; // 'TAUR'
constexpr std::size_t kPanicTextBytes = 128U;
constexpr std::size_t kPanicTaskBytes = 20U;
RTC_NOINIT_ATTR volatile std::uint32_t g_s3_panic_magic;
RTC_NOINIT_ATTR volatile std::uint32_t g_s3_panic_pc;
RTC_NOINIT_ATTR volatile std::uint32_t g_s3_panic_cause;
RTC_NOINIT_ATTR volatile std::uint32_t g_s3_panic_exception;
RTC_NOINIT_ATTR volatile std::uint32_t g_s3_panic_core;
RTC_NOINIT_ATTR char g_s3_panic_text[kPanicTextBytes];
RTC_NOINIT_ATTR char g_s3_panic_task[kPanicTaskBytes];

// Runs from the panic context: no locks, no heap, no ESP_LOG. Copy by hand so
// nothing here can call into flash-resident string code with the cache off.
void IRAM_ATTR copy_panic_text(volatile char* const out, const std::size_t capacity,
                               const char* const in) {
    std::size_t index = 0U;
    if (in != nullptr) {
        while (index + 1U < capacity && in[index] != '\0') {
            out[index] = in[index];
            ++index;
        }
    }
    out[index] = '\0';
}

extern "C" void IRAM_ATTR __wrap_esp_panic_handler(panic_info_t* const info) {
    if (info != nullptr) {
        g_s3_panic_pc = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(info->addr));
        g_s3_panic_cause = info->frame != nullptr ? panic_get_cause(info->frame) : UINT32_MAX;
        g_s3_panic_exception = static_cast<std::uint32_t>(info->exception);
        g_s3_panic_core = static_cast<std::uint32_t>(info->core);
        copy_panic_text(g_s3_panic_text, kPanicTextBytes,
                        g_panic_abort ? g_panic_abort_details : info->description);
        const TaskHandle_t task = xTaskGetCurrentTaskHandleForCore(info->core);
        copy_panic_text(g_s3_panic_task, kPanicTaskBytes,
                        task != nullptr ? pcTaskGetName(task) : nullptr);
        g_s3_panic_magic = kPanicRecordMagic;
    }
    __real_esp_panic_handler(info);
}

// Called once the log ring is capturing, so /v1/logs carries it. kTag is
// declared further down the file, so this uses its value directly.
void report_previous_panic() {
    ESP_LOGI("nam", "boot: reset reason=%d", static_cast<int>(esp_reset_reason()));
    if (g_s3_panic_magic != kPanicRecordMagic) {
        return;
    }
    g_s3_panic_text[kPanicTextBytes - 1U] = '\0';
    g_s3_panic_task[kPanicTaskBytes - 1U] = '\0';
    ESP_LOGE("nam",
             "previous boot ended in a panic on core %lu in task '%s': %s "
             "(pc=0x%08lx cause=%lu exception=%lu)",
             static_cast<unsigned long>(g_s3_panic_core), g_s3_panic_task, g_s3_panic_text,
             static_cast<unsigned long>(g_s3_panic_pc),
             static_cast<unsigned long>(g_s3_panic_cause),
             static_cast<unsigned long>(g_s3_panic_exception));
    g_s3_panic_magic = 0U;
    g_s3_panic_pc = 0U;
    g_s3_panic_cause = 0U;
    g_s3_panic_exception = 0U;
    g_s3_panic_core = 0U;
    g_s3_panic_text[0] = '\0';
    g_s3_panic_task[0] = '\0';
}

namespace {

constexpr const char* kTag = "nam";
constexpr std::uint32_t kSampleRate = 48000;
constexpr std::size_t kSplitLayer = COYOPEDAL_PEDAL_S3_SPLIT_LAYER;
constexpr std::size_t kDelaySamples = kSampleRate * 2U + 4U;
constexpr std::size_t kReverbLineCount = 8U;
// The heap promotion, largest first, for a boot whose lines did not come out
// of the link-time arena (a maintenance boot has lent the arena to the heap).
// An audio boot has all eight lines internal from the arena and the promotion
// finds nothing to do.
constexpr std::array<std::size_t, 6> kPromotedReverbLines = {
    1U, 5U, 3U, 7U, 6U, 2U,
};
std::array<void*, kReverbLineCount> reverb_base_lines{};
std::array<void*, kReverbLineCount> reverb_promoted_lines{};
void* modulation_promoted_line = nullptr;

} // namespace

extern "C" bool coyopedal_s3_reverb_promote_hot_lines() {
    std::size_t promoted = 0U;
    for (const std::size_t line : kPromotedReverbLines) {
        if (reverb_promoted_lines[line] != nullptr) {
            ++promoted;
            continue;
        }
        void* const base = reverb_base_lines[line];
        const std::size_t bytes = coyopedal_fx_reverb_line_state_size(line);
        if (base == nullptr || bytes == 0U) {
            continue;
        }
        if (esp_ptr_internal(base)) {
            // Already internal from the link-time arena; nothing to promote.
            ++promoted;
            continue;
        }
        void* const fast =
            heap_caps_aligned_alloc(16U, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (fast == nullptr) {
            ESP_LOGW(kTag, "reverb line %u stays in PSRAM; need=%u free=%u largest=%u",
                     static_cast<unsigned>(line), static_cast<unsigned>(bytes),
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                     static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
            continue;
        }
        std::memcpy(fast, base, bytes);
        if (!coyopedal_fx_reverb_line_attach(line, fast, bytes)) {
            heap_caps_free(fast);
            continue;
        }
        reverb_promoted_lines[line] = fast;
        ++promoted;
        ESP_LOGI(kTag, "reverb line %u promoted to internal SRAM (%u bytes)",
                 static_cast<unsigned>(line), static_cast<unsigned>(bytes));
    }
    return promoted != 0U;
}

extern "C" void coyopedal_s3_reverb_demote_hot_lines() {
    for (const std::size_t line : kPromotedReverbLines) {
        void* const fast = reverb_promoted_lines[line];
        void* const base = reverb_base_lines[line];
        if (fast == nullptr || base == nullptr) {
            continue;
        }
        const std::size_t bytes = coyopedal_fx_reverb_line_state_size(line);
        std::memcpy(base, fast, bytes);
        if (!coyopedal_fx_reverb_line_attach(line, base, bytes)) {
            ESP_LOGE(kTag, "could not restore reverb line %u to PSRAM",
                     static_cast<unsigned>(line));
            continue;
        }
        heap_caps_free(fast);
        reverb_promoted_lines[line] = nullptr;
    }
}

extern "C" bool coyopedal_s3_modulation_promote_line() {
    const std::size_t bytes = coyopedal_fx_modulation_line_state_size();
    if (modulation_promoted_line != nullptr) {
        return true;
    }
    void* const fast = heap_caps_aligned_alloc(16U, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (fast == nullptr) {
        ESP_LOGW(kTag, "modulation line stays in PSRAM; need=%u free=%u largest=%u",
                 static_cast<unsigned>(bytes),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
        return false;
    }
    if (!coyopedal_fx_modulation_line_migrate(fast, bytes)) {
        heap_caps_free(fast);
        return false;
    }
    modulation_promoted_line = fast;
    ESP_LOGI(kTag, "modulation line promoted to internal SRAM (%u bytes)",
             static_cast<unsigned>(bytes));
    return true;
}

esp_err_t pmu_init();

namespace {

constexpr gpio_num_t kModeButtonGpio = GPIO_NUM_0;
constexpr std::uint32_t kModeButtonStackBytes = 2048U;
// BOOT is a footswitch on every board: a tap toggles bypass on release, while a
// hold switches between audio and maintenance the instant it reaches the
// threshold. The mode switch cannot wait for the release: nothing would happen
// at 1500ms, so a player with no feedback keeps pressing for several seconds.
constexpr std::uint32_t kModeButtonHoldMs = 1500U;
// A mode switch reboots. GPIO0 is also the boot strapping pin, so the reboot
// has to wait for the release; this bounds that wait for a stuck contact.
constexpr std::uint32_t kModeButtonReleaseTimeoutMs = 10000U;
// The TCB must be internal (xPortCheckValidTCBMem). RTC fast memory counts as
// internal and keeps it out of the DRAM the graph needs.
RTC_FAST_ATTR StaticTask_t mode_button_tcb{};
// The stack can be in PSRAM because this task never touches flash: it debounces
// the pin and calls coyopedal_remote_toggle_mode(), which is one xTaskNotifyGive.
// The mode worker in services/remote_service.cpp does the transition itself and
// so needs an internal stack.
EXT_RAM_BSS_ATTR alignas(16) StackType_t mode_button_stack[kModeButtonStackBytes]{};
TaskHandle_t mode_button_task_handle{};

void IRAM_ATTR mode_button_isr(void*) {
    BaseType_t higher_priority_woken = pdFALSE;
    vTaskNotifyGiveFromISR(mode_button_task_handle, &higher_priority_woken);
    if (higher_priority_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

void mode_button_task(void*) {
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        // GPIO0 is active-low. Debounce once after the falling edge, then wait
        // for release so switch bounce cannot enqueue a second mode change.
        vTaskDelay(pdMS_TO_TICKS(50));
        if (gpio_get_level(kModeButtonGpio) != 0) {
            gpio_intr_enable(kModeButtonGpio);
            continue;
        }
        gpio_intr_disable(kModeButtonGpio);
        std::uint32_t held_ms = 50U;
        bool switched = false;
        while (gpio_get_level(kModeButtonGpio) == 0) {
            if (!switched && held_ms >= kModeButtonHoldMs) {
                // Ask for the switch while the button is still down. The panel
                // goes to "Please wait" immediately, which is the cue to let
                // go; the loop below then waits out the rest of the press.
                switched = true;
                coyopedal_remote_toggle_mode();
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            held_ms += 10U;
        }
        // A press that already asked for a mode change is not also a tap.
        if (!switched && coyopedal_remote_audio_mode() && !coyopedal_remote_mode_busy()) {
            // The same call the on-screen footswitch makes, so the control model
            // and the remote API see one kind of engage, not two.
            coyopedal_control_state_t state{};
            coyopedal_ui_control_state(&state);
            const bool engage = state.engaged == 0U;
            (void)coyopedal_ui_control_set_engaged(engage);
#if !defined(GEA_EMBEDDED_NO_DISPLAY) || !GEA_EMBEDDED_NO_DISPLAY
            s3_v1_ui_wake();
#endif
            ESP_LOGI(kTag, "BOOT tap: pedal %s", engage ? "engaged" : "bypassed");
        }
        // Re-arm the edge after the release bounce; re-enabling on a still
        // ringing contact would queue a phantom press that toggles straight back.
        vTaskDelay(pdMS_TO_TICKS(50));
        gpio_intr_enable(kModeButtonGpio);
    }
}

bool start_mode_button() {
    // The application button is BOOT/GPIO0. The task is static and blocks on a
    // GPIO edge, so its steady-state cost is zero.
    gpio_reset_pin(kModeButtonGpio);
    gpio_config_t config{};
    config.pin_bit_mask = 1ULL << static_cast<unsigned>(kModeButtonGpio);
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_NEGEDGE;
    if (gpio_config(&config) != ESP_OK) {
        ESP_LOGE(kTag, "BOOT mode button GPIO configuration failed");
        return false;
    }
    const esp_err_t isr_service = gpio_install_isr_service(0);
    if (isr_service != ESP_OK && isr_service != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(kTag, "BOOT mode button ISR service failed");
        return false;
    }
    mode_button_task_handle = xTaskCreateStaticPinnedToCore(
        // On a press it must outrank the saturated audio workers.
        mode_button_task, "mode_button", kModeButtonStackBytes, nullptr, configMAX_PRIORITIES - 1,
        mode_button_stack, &mode_button_tcb, 0);
    if (mode_button_task_handle == nullptr) {
        ESP_LOGE(kTag, "BOOT mode button static task creation failed");
        return false;
    }
    if (gpio_isr_handler_add(kModeButtonGpio, mode_button_isr, nullptr) != ESP_OK) {
        ESP_LOGE(kTag, "BOOT mode button ISR handler failed");
        vTaskDelete(mode_button_task_handle);
        mode_button_task_handle = nullptr;
        return false;
    }
    ESP_LOGI(kTag, "BOOT/GPIO0 ready: tap toggles the pedal, hold %ums for maintenance",
             static_cast<unsigned>(kModeButtonHoldMs));
    return true;
}

} // namespace

void coyopedal_mode_button_wait_for_release() {
    // GPIO0 is the ESP32-S3 boot strapping pin as well as the footswitch, so a
    // reset taken while it is held can strap the chip into ROM download mode
    // instead of the application. Every restart goes through here, which also
    // covers an OTA or a remote command that lands mid-press.
    if (mode_button_task_handle == nullptr) {
        return;
    }
    std::uint32_t waited_ms = 0U;
    while (gpio_get_level(kModeButtonGpio) == 0 && waited_ms < kModeButtonReleaseTimeoutMs) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited_ms += 10U;
    }
    if (waited_ms >= kModeButtonReleaseTimeoutMs) {
        ESP_LOGW(kTag, "BOOT still held after %ums; restarting anyway",
                 static_cast<unsigned>(waited_ms));
    }
}

namespace {
void* audio_reverb_state{};
void* audio_reverb_predelay{};
void* audio_tank_internal{};
void* audio_tank_psram{};
float* audio_delay{};

// Every Q15 tank line is internal, from the link-time effects arena. A line in
// PSRAM shares the D-cache with the UI and the display flush, and stage A
// overruns while they are busy. See "The effects arena" in docs/MEMORY.md.
constexpr std::uint32_t kPreferredInternalLineMask = 0xFFU;

std::size_t reverb_internal_line_bytes() {
    std::size_t bytes = 0U;
    for (std::size_t line = 0; line < coyopedal_fx_reverb_line_count(); ++line) {
        if ((kPreferredInternalLineMask & (1U << line)) != 0U) {
            bytes += coyopedal_fx_reverb_line_state_size(line);
        }
    }
    return bytes;
}

// Internal SRAM the effects hold for the whole run, reserved at link time.
//
// A heap block of this size would sit in the middle of the one large internal
// region and split it, and the A2-Full history planner cannot place its 25
// histories across the fragments. A .bss array sits below _heap_start instead,
// so the heap simply starts higher and stays in one piece.
//
// It holds all eight Q15 tank lines: CompactReverb::kMaximum times two bytes,
// that is 1,138 + 7,342 + 2,962 + 6,082 + 1,522 + 6,922 + 4,358 + 5,186 =
// 35,512, rounded up to the arena's 16-byte grain. reserve_reverb_internal()
// takes them as one block and start_audio_effects() lays the lines out inside
// it. The reverb's core state is not here: it comes from the NAM bank arena's
// unused tails (see "The NAM bank arena" in docs/MEMORY.md).
constexpr std::size_t kEffectsArenaBytes = 35520U;
alignas(16) std::uint8_t g_effects_arena[kEffectsArenaBytes];
std::size_t g_effects_arena_used;
// Set once the arena has been handed to the maintenance heap. From then on every
// byte of it belongs to the heap and must not be handed to the reverb as well.
bool g_effects_arena_lent;

// Bump allocation only. Nothing here is ever freed individually -- the effects
// hold their storage for as long as the graph exists -- so there is no free
// list to fragment and no allocator state beyond the offset.
void* effects_arena_alloc(const std::size_t bytes) {
    if (g_effects_arena_lent) {
        return nullptr;
    }
    if (bytes == 0U) {
        return nullptr;
    }
    const std::size_t aligned = (bytes + 15U) & ~std::size_t{15U};
    if (aligned > kEffectsArenaBytes - g_effects_arena_used) {
        return nullptr;
    }
    std::uint8_t* const block = g_effects_arena + g_effects_arena_used;
    g_effects_arena_used += aligned;
    std::memset(block, 0, aligned);
    return block;
}

bool effects_arena_owns(const void* const block) {
    if (g_effects_arena_lent) {
        return false;
    }
    const auto* const address = static_cast<const std::uint8_t*>(block);
    return address >= g_effects_arena && address < g_effects_arena + kEffectsArenaBytes;
}

// Releasing the arena means rewinding it. Its storage is static, so handing a
// pointer into it to heap_caps_free() would abort the board.
void effects_arena_release(void*& block) {
    if (effects_arena_owns(block)) {
        block = nullptr;
        return;
    }
    heap_caps_free(block);
    block = nullptr;
}

void effects_arena_reset() {
    g_effects_arena_used = 0U;
}

// Seconds to wait before rebooting a maintenance boot that could not reach the
// network. Long enough that a transient association failure has a chance to be
// logged and read over USB, short enough that the board is trying again while
// the user is still at the bench.
constexpr std::uint64_t kMaintenanceRetryDelayUs = 30ULL * 1000ULL * 1000ULL;

void schedule_maintenance_retry_reboot() {
    static esp_timer_handle_t retry_timer{};
    if (retry_timer != nullptr) {
        return;
    }
    const esp_timer_create_args_t args{
        .callback = [](void*) { esp_restart(); },
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "maint_retry",
        .skip_unhandled_events = false,
    };
    if (esp_timer_create(&args, &retry_timer) != ESP_OK ||
        esp_timer_start_once(retry_timer, kMaintenanceRetryDelayUs) != ESP_OK) {
        ESP_LOGE(kTag, "could not arm the maintenance retry reboot");
        retry_timer = nullptr;
        return;
    }
    ESP_LOGW(kTag, "maintenance network is down; rebooting in %u s to try again",
             static_cast<unsigned>(kMaintenanceRetryDelayUs / 1000000ULL));
}

// A maintenance boot runs no effects and needs the memory for Wi-Fi, the HTTP
// server, BLE and the panel, so it hands the arena to the allocator as a region
// of its own. The audio boot never calls this and keeps the fixed addresses.
void effects_arena_lend_to_heap() {
    const auto start = reinterpret_cast<intptr_t>(g_effects_arena);
    const esp_err_t added = heap_caps_add_region(start, start + kEffectsArenaBytes - 1);
    if (added != ESP_OK) {
        ESP_LOGW(kTag, "could not lend the %u-byte effects arena to the heap: %s",
                 static_cast<unsigned>(kEffectsArenaBytes), esp_err_to_name(added));
        return;
    }
    g_effects_arena_lent = true;
    ESP_LOGI(kTag, "effects arena lent to the heap for this maintenance boot: %u bytes at %p",
             static_cast<unsigned>(kEffectsArenaBytes), static_cast<void*>(g_effects_arena));
}

// The reverb's core state is touched at 24 kHz, so it belongs in internal SRAM.
// When the NAM bank arena cannot supply it, it is taken from the heap before the
// model loads, but only if the graph can still be placed afterwards: the
// A2-Full graph needs kGraphInternalBytes of internal SRAM, plus
// kGraphPlannerSlack, because its coefficient banks sit on 32 KiB boundaries and
// its 25 histories must fit the regions left around them. Without that slack
// the planner fails and the board has no amp at all.
constexpr std::size_t kGraphInternalBytes = 213580U;
constexpr std::size_t kGraphPlannerSlack = 13400U;

bool reserve_reverb_internal() {
    const std::size_t state_bytes = coyopedal_fx_reverb_state_size();
    if (state_bytes == 0U) {
        return false;
    }
    if (audio_reverb_state != nullptr) {
        return true;
    }
    // The lines come from the link-time arena, so they are never a lottery and
    // never a wall across the middle of the graph's region.
    const std::size_t line_bytes = reverb_internal_line_bytes();
    audio_tank_internal = line_bytes == 0U ? nullptr : effects_arena_alloc(line_bytes);
    if (line_bytes != 0U && audio_tank_internal == nullptr) {
        // Expected on a maintenance boot, which has lent the arena away; the
        // heap fallback in start_audio_effects() picks the lines up. On an
        // audio boot it means kEffectsArenaBytes no longer covers the tank.
        ESP_LOG_LEVEL_LOCAL(g_effects_arena_lent ? ESP_LOG_INFO : ESP_LOG_ERROR, kTag,
                            "reverb tank lines not taken from the arena (%u bytes, arena %u); "
                            "falling back to the heap",
                            static_cast<unsigned>(line_bytes),
                            static_cast<unsigned>(kEffectsArenaBytes));
    } else if (line_bytes != 0U) {
        ESP_LOGI(kTag, "reverb tank lines reserved from the link-time arena: %u at %p",
                 static_cast<unsigned>(line_bytes), audio_tank_internal);
    }
    // The core state comes out of the NAM bank arena's unused tail, which the
    // placed graph hands back. After the graph, the USB host and the display are
    // placed, the heap has space but no contiguous block this large, and taking
    // more than the tail pushes the graph into the heap Gea's runtime allocator
    // draws on; that allocator aborts instead of failing.
    const std::size_t state_slot = (state_bytes + 15U) & ~std::size_t{15U};
    if (auto* const slice = coyopedal_nam_bank_arena_take_effects(state_slot)) {
        audio_reverb_state = slice;
        ESP_LOGI(kTag, "reverb core state in internal SRAM from the NAM arena: %u bytes at %p",
                 static_cast<unsigned>(state_bytes), audio_reverb_state);
        return true;
    }
    // The state is only worth claiming if the graph still fits afterwards.
    const std::size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (free_internal < kGraphInternalBytes + kGraphPlannerSlack + state_bytes) {
        ESP_LOGI(kTag,
                 "reverb state stays in PSRAM: %u internal free, the graph needs %u + %u to place "
                 "and the state wants %u; reserving it would cost the board its amp",
                 static_cast<unsigned>(free_internal), static_cast<unsigned>(kGraphInternalBytes),
                 static_cast<unsigned>(kGraphPlannerSlack), static_cast<unsigned>(state_bytes));
        return audio_tank_internal != nullptr;
    }
    audio_reverb_state =
        heap_caps_aligned_calloc(16U, 1U, state_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (audio_reverb_state == nullptr) {
        ESP_LOGW(kTag, "reverb state reservation failed: need %u; free=%u largest=%u",
                 static_cast<unsigned>(state_bytes), static_cast<unsigned>(free_internal),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
        return audio_tank_internal != nullptr;
    }
    ESP_LOGI(kTag, "reverb state reserved: %u at %p; largest free now %u",
             static_cast<unsigned>(state_bytes), audio_reverb_state,
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    return true;
}

// Arena storage is decided at link time and is never handed back to an
// allocator; only heap blocks are.
void release_effects_block(void*& block) {
    if (block != nullptr && coyopedal_nam_bank_arena_owns_ptr(block)) {
        block = nullptr;
        return;
    }
    effects_arena_release(block);
}

void stop_audio_graph() {
    // Caller has drained all DSP stages. Detach before returning any storage.
    ESP_LOGI(kTag, "maintenance: detaching effects");
    coyopedal_fx_detach();
    for (auto*& line : reverb_promoted_lines) {
        heap_caps_free(line);
        line = nullptr;
    }
    heap_caps_free(modulation_promoted_line);
    modulation_promoted_line = nullptr;
    reverb_base_lines.fill(nullptr);
    heap_caps_free(audio_delay);
    audio_delay = nullptr;
    release_effects_block(audio_tank_internal);
    heap_caps_free(audio_tank_psram);
    audio_tank_psram = nullptr;
    release_effects_block(audio_reverb_state);
    heap_caps_free(audio_reverb_predelay);
    audio_reverb_predelay = nullptr;
    effects_arena_reset();
    ESP_LOGI(kTag, "maintenance: releasing NAM arenas");
    g_engine.unload_model();
    ESP_LOGI(kTag, "maintenance: graph released");
}

bool start_audio_effects() {
    const std::size_t reverb_state_size = coyopedal_fx_reverb_state_size();
    std::size_t reverb_total_size = reverb_state_size;
    std::size_t reverb_internal_size = reverb_state_size;
    std::size_t reverb_psram_size = 0U;
    std::size_t tank_internal_size = 0U;
    std::size_t tank_psram_size = 0U;
    std::uint32_t internal_line_mask = kPreferredInternalLineMask;
    for (std::size_t line = 0; line < coyopedal_fx_reverb_line_count(); ++line) {
        const std::size_t line_size = coyopedal_fx_reverb_line_state_size(line);
        reverb_total_size += line_size;
        if ((internal_line_mask & (1U << line)) != 0U) {
            tank_internal_size += line_size;
        } else {
            tank_psram_size += line_size;
        }
    }
    // reserve_reverb_internal() normally placed the core state already. If it
    // could not, try the heap, and fall back to PSRAM so the board still boots.
    if (audio_reverb_state == nullptr) {
        audio_reverb_state = heap_caps_aligned_calloc(16U, 1U, reverb_internal_size,
                                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (audio_reverb_state == nullptr) {
        audio_reverb_state =
            heap_caps_calloc(1U, reverb_internal_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        reverb_internal_size = 0U;
    }
    if (reverb_state_size != 0U &&
        !coyopedal_fx_reverb_attach(audio_reverb_state, reverb_state_size)) {
        ESP_LOGE(kTag, "no internal RAM for %u-byte compact reverb; free=%u largest=%u",
                 static_cast<unsigned>(reverb_total_size),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
        return false;
    }
    if (audio_tank_internal == nullptr) {
        audio_tank_internal = heap_caps_aligned_calloc(16U, 1U, tank_internal_size,
                                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (audio_tank_internal == nullptr) {
        ESP_LOGW(kTag, "compact reverb hot lines remain in PSRAM; need=%u free=%u largest=%u",
                 static_cast<unsigned>(tank_internal_size),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
        internal_line_mask = 0U;
        tank_psram_size += tank_internal_size;
        tank_internal_size = 0U;
    }
    // With every line internal (the arena holds all eight) there is no PSRAM
    // tank at all; a zero-byte calloc returns null and must not read as failure.
    audio_tank_psram = tank_psram_size != 0U ? heap_caps_calloc(1U, tank_psram_size,
                                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                                             : nullptr;
    if (audio_tank_psram == nullptr && tank_psram_size != 0U) {
        ESP_LOGE(kTag, "could not allocate %u-byte compact reverb PSRAM tank",
                 static_cast<unsigned>(tank_psram_size));
        return false;
    }
    auto* internal_line_state = static_cast<std::uint8_t*>(audio_tank_internal);
    auto* psram_line_state = static_cast<std::uint8_t*>(audio_tank_psram);
    for (std::size_t line = 0; line < coyopedal_fx_reverb_line_count(); ++line) {
        const std::size_t line_size = coyopedal_fx_reverb_line_state_size(line);
        const bool internal = (internal_line_mask & (1U << line)) != 0U;
        void* const line_state = internal ? static_cast<void*>(internal_line_state)
                                          : static_cast<void*>(psram_line_state);
        if (!coyopedal_fx_reverb_line_attach(line, line_state, line_size)) {
            ESP_LOGE(kTag, "could not attach compact reverb line %u (%u bytes); free=%u largest=%u",
                     static_cast<unsigned>(line), static_cast<unsigned>(line_size),
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                     static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
            return false;
        }
        reverb_base_lines[line] = line_state;
        if (internal) {
            internal_line_state += line_size;
        } else {
            psram_line_state += line_size;
        }
    }
    const std::size_t predelay_bytes = coyopedal_fx_reverb_predelay_state_size();
    if (predelay_bytes != 0U) {
        // Always PSRAM. The ring is read and written once per sample at
        // increasing addresses, which the PSRAM cache handles well, and the
        // display's DMA buffers are allocated after this and need the internal
        // SRAM.
        audio_reverb_predelay =
            heap_caps_calloc(1U, predelay_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (audio_reverb_predelay == nullptr ||
            !coyopedal_fx_reverb_predelay_attach(audio_reverb_predelay, predelay_bytes)) {
            ESP_LOGE(kTag, "could not attach the %u-byte reverb predelay",
                     static_cast<unsigned>(predelay_bytes));
            return false;
        }
        tank_psram_size += predelay_bytes;
    }
    reverb_total_size += predelay_bytes;
    reverb_internal_size += tank_internal_size;
    reverb_psram_size = tank_psram_size;
    if (!esp_ptr_internal(audio_reverb_state)) {
        reverb_psram_size += reverb_state_size;
    }
    ESP_LOGI(kTag, "compact reverb: %u bytes (%u internal, %u PSRAM, tank=%s); %u internal remain",
             static_cast<unsigned>(reverb_total_size), static_cast<unsigned>(reverb_internal_size),
             static_cast<unsigned>(reverb_psram_size),
             internal_line_mask == 0U ? "PSRAM" : "hybrid",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
    coyopedal_fx_init();
    audio_delay =
        static_cast<float*>(heap_caps_calloc(kDelaySamples, sizeof(float), MALLOC_CAP_SPIRAM));
    if (audio_delay == nullptr || !coyopedal_fx_delay_attach(audio_delay, kDelaySamples)) {
        ESP_LOGW(kTag, "delay unavailable; continuing with six effect blocks");
    }
    // Keep every block off while USB and the UI come up. The UI applies the
    // selected preset atomically before audio resumes.
    for (int block = 0; block < COYOPEDAL_FX_BLOCK_COUNT; ++block) {
        coyopedal_fx_set_enabled(static_cast<coyopedal_fx_block_t>(block), false);
    }
    ESP_LOGI(kTag, "effects ready; awaiting preset recall");

    return true;
}
} // namespace

bool pedalboard_audio_unload() {
    if (!usb_audio_begin_update())
        return false;
    stop_audio_graph();
    ESP_LOGI(kTag, "audio graph released: internal free=%u largest=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    // Hold the transport muted until the graph and preset have been restored.
    return true;
}

bool pedalboard_audio_reload() {
    char error[96]{};
    const unsigned model = coyopedal_active_model();
    // Claim the reverb's internal storage before the model loads.
    (void)reserve_reverb_internal();
    const auto load_model = [&]() {
        return model < coyopedal_model_count
                   ? coyopedal_load_model(model, error, sizeof error)
                   : g_engine.load_namb(kModelStart, kModelEnd - kModelStart, error, sizeof error);
    };
    const bool loaded = load_model();
    if (!loaded || !start_audio_effects()) {
        ESP_LOGE(kTag, "audio restore failed: %s", error);
        stop_audio_graph();
        return false;
    }
    return true;
}

void pedalboard_audio_resume() {
#if !COYOPEDAL_REMOTE_DISABLE_USB_AUDIO
    usb_audio_start(); // Idempotent; keeps an attached interface enumerated.
    coyopedal_s3_modulation_promote_line();
    coyopedal_s3_reverb_promote_hot_lines();
#endif
    usb_audio_end_update();
}

namespace {
void start_audio_recovery() {
    stop_audio_graph();
    (void)usb_audio_begin_update();
    (void)s3_v1_maintenance_status_start(COYOPEDAL_REMOTE_WIFI_SSID);
    coyopedal_remote_start();
    pedalboard_ble_start();
    coyopedal_remote_mode_start();
    ESP_LOGE(kTag, "Audio could not start; maintenance available for recovery");
}
} // namespace

// What the panel's QSPI bus and LCD transaction pool need contiguous at
// Display::init(); see "SPI bus init failed: ESP_ERR_NO_MEM" in the Gea memory
// service. Held through gea::framework::Runtime::holdDisplayReserve().
constexpr std::size_t kDisplayInternalReserveBytes = 4096U;

// The Gea runtime owns boot: @geastack/targets' app_main brings the chip up and
// calls this weak hook before it claims the display framebuffer, the radios and
// the frame loop. Everything below is the pedal's own hardware -- the DSP, the
// model library and the USB audio host -- which must have its contiguous
// internal DMA allocations before the panel takes what is left.
extern "C" void gea_app_native_boot(void) {
    // The display's internal DMA reserve belongs to the app, not the runtime
    // (GEA_EMBEDDED_DISPLAY_INTERNAL_RESERVE_BYTES is 0 in package.json). It is
    // taken first and, on an audio boot, released while the graph is placed.
    gea::framework::Runtime::holdDisplayReserve(
        gea::platform::memory::Memory::reserveInternalDma(kDisplayInternalReserveBytes));
    coyopedal_remote_capture_logs();
    // Before anything else can overwrite it: why the board came back.
    report_previous_panic();
    coyopedal_remote_prepare_runtime();
    constexpr const char* kEngineDescription = "LX7-native A22 mixed-precision A2-Full";
    ESP_LOGI(kTag, "nam-pedalboard: %s, split=%u", kEngineDescription,
             static_cast<unsigned>(COYOPEDAL_PEDAL_S3_SPLIT_LAYER));
    ESP_LOGI(kTag, "S3-native masks: low=%08lx low_w16=%08lx skipped_low=%08lx",
             static_cast<unsigned long>(coyopedal::pedal::Processor::Model::kLowLayerMask),
             static_cast<unsigned long>(coyopedal::pedal::Processor::Model::kLowRingW16Mask),
             static_cast<unsigned long>(coyopedal::pedal::Processor::Model::kSkippedLowLayerMask));
    if (pmu_init() != ESP_OK) {
        ESP_LOGW(kTag, "PMU configuration failed; power may be fragile");
    }

    esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result == ESP_ERR_NVS_NO_FREE_PAGES || nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_result = nvs_flash_init();
    }
    if (nvs_result != ESP_OK) {
        ESP_LOGE(kTag, "NVS initialization failed: %s", esp_err_to_name(nvs_result));
    } else if (!start_mode_button()) {
        ESP_LOGE(kTag, "audio-maintenance mode switch unavailable");
    }

    // Audio is the DEFAULT boot: a pedal switched on should play. Maintenance
    // is the exception, entered on an explicit one-shot request (the mode
    // button, the end of an AUDIO TRY window). An audio boot turns both radios
    // off, so a crash there would leave the board unreachable and, rebooting
    // into audio again, crashing forever; a boot that follows a panic, a
    // watchdog or a brownout therefore lands in maintenance, where the API is
    // up to say why. Every flag is one-shot, so consume them all before
    // deciding: a leftover maintenance request must not survive to veto the
    // next AUDIO TRY.
    const bool audio_requested = nvs_result == ESP_OK && coyopedal_remote_audio_boot_requested();
    const bool maintenance_requested =
        nvs_result == ESP_OK && coyopedal_remote_maintenance_boot_requested();
    const esp_reset_reason_t reset_reason = esp_reset_reason();
    const bool crashed = reset_reason == ESP_RST_PANIC || reset_reason == ESP_RST_INT_WDT ||
                         reset_reason == ESP_RST_TASK_WDT || reset_reason == ESP_RST_WDT ||
                         reset_reason == ESP_RST_BROWNOUT;
    const bool audio_boot =
        nvs_result == ESP_OK && !maintenance_requested && (audio_requested || !crashed);
    if (!audio_boot) {
        ESP_LOGI(kTag,
                 "maintenance boot: audio engine, effects, USB host and UI are not initialized");
        effects_arena_lend_to_heap();
        // The NAM coefficient banks are link-time storage too, and nothing on a
        // maintenance boot loads the graph that owns them. Wi-Fi and OTA get
        // them instead; the reboot back to audio takes them away again.
        coyopedal_nam_bank_arena_lend_to_heap();
        (void)s3_v1_maintenance_status_start(COYOPEDAL_REMOTE_WIFI_SSID);
        if (!coyopedal_remote_start()) {
            ESP_LOGE(kTag, "wireless maintenance startup failed");
            // A maintenance boot without a network is unreachable, so try
            // another boot rather than wait here.
            schedule_maintenance_retry_reboot();
        }
        if (!pedalboard_ble_start())
            ESP_LOGE(kTag, "BLE discovery startup failed");
        (void)usb_audio_begin_update();
        coyopedal_remote_mode_start();
        return;
    }

    // Before anything that can fail: the deadline by which an AUDIO TRY boot
    // must be back on the network, so a pending audio profile or OTA validation
    // keeps control even if the restored preset saturates both cores.
    coyopedal_remote_arm_audio_window();

    coyopedal_remote_prepare_requested_diagnostics();

    // The USB host starts after the model: usb_audio_start() also creates the
    // two DSP stage workers, and they must not exist before a model does.

    ESP_LOGI(kTag, "before NAM load: internal free=%u largest=%u; PSRAM free=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    // The panel's reserve steps aside while the graph is placed; the display
    // only needs its four kilobytes by the time Display::init() runs.
    gea::framework::Runtime::holdDisplayReserve(nullptr);
    coyopedal_log_internal_heap_census("before NAM load");
    const std::size_t model_size = static_cast<std::size_t>(kModelEnd - kModelStart);
    char error[96]{};
    const std::int64_t load_start = esp_timer_get_time();
    (void)reserve_reverb_internal();
    const auto load_configured_model = [&]() {
        bool loaded = false;
        if (coyopedal_flash_store_init() && coyopedal_models_init() && coyopedal_presets_init()) {
            const char* const initial_profile = coyopedal_preset_profile(0U);
            for (unsigned index = 0; index < coyopedal_model_count; ++index) {
                if (std::strcmp(coyopedal_models[index].id, initial_profile) == 0) {
                    loaded = coyopedal_load_model(index, error, sizeof error);
                    break;
                }
            }
        }
        if (!loaded && !g_engine.model_loaded()) {
            loaded = g_engine.load_namb(kModelStart, model_size, error, sizeof error);
        }
        return loaded || g_engine.model_loaded();
    };
    const bool model_loaded = load_configured_model();
    if (!model_loaded) {
        ESP_LOGE(kTag, "model load failed: %s", error);
        start_audio_recovery();
        return;
    }
    ESP_LOGI(kTag, "factory preset A2-Full model loaded and calibrated in %.0f ms",
             (esp_timer_get_time() - load_start) / 1000.0);

    if (!start_audio_effects()) {
        start_audio_recovery();
        return;
    }

    // The graph and the reverb have their internal SRAM; the panel's reserve
    // comes back now, ahead of the USB host.
    gea::framework::Runtime::holdDisplayReserve(
        gea::platform::memory::Memory::reserveInternalDma(kDisplayInternalReserveBytes));

#if COYOPEDAL_REMOTE_DISABLE_USB_AUDIO
    ESP_LOGW(kTag, "USB audio host disabled; native USB-Serial/JTAG remains active");
#else
    // Claim realtime stacks before LCD/UI allocation fragments internal SRAM.
    // This must stay AFTER the model load: usb_audio_start() creates the stage A
    // and stage B DSP workers, and they may not exist before a model does.
    ESP_LOGI(kTag,
             "starting USB audio host for the attached UAC2 interface; internal free=%u largest=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    usb_audio_start();
    // Both cores are real-time from here on, so an idle task can go seconds
    // without running. The task watchdog's report on a starved idle task is
    // printed from interrupt context and costs stage A a block each time, so
    // the idle tasks leave the watchdog. Maintenance boots keep it.
    //
    // Reconfiguring is what actually drops them. Deleting the idle task's entry
    // leaves the watchdog's idle hook registered, and that hook goes on calling
    // esp_task_wdt_reset() for a task the watchdog no longer knows -- one
    // "task not found" error per idle tick, which on a board whose console is a
    // real UART is thousands of blocking writes a second with audio starving
    // behind them. It is invisible on a console nothing is listening to.
#ifdef CONFIG_ESP_TASK_WDT_PANIC
    constexpr bool kWatchdogPanics = true;
#else
    constexpr bool kWatchdogPanics = false;
#endif
    esp_task_wdt_config_t watchdog = {};
    watchdog.timeout_ms = CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000U;
    watchdog.idle_core_mask = 0;
    watchdog.trigger_panic = kWatchdogPanics;
    if (esp_task_wdt_reconfigure(&watchdog) != ESP_OK) {
        ESP_LOGW(kTag, "idle tasks stay on the task watchdog");
    }
    // Give an already-attached interface first use of the contiguous DMA heap.
    // Without this short window, USB enumeration and LCD stripe allocation can
    // race: the host installs successfully, then the UI splits the only block
    // large enough for the endpoint-pipe descriptor array.
    for (unsigned attempt = 0; attempt < 100U; ++attempt) {
        usb_audio_diagnostics_t diagnostics{};
        usb_audio_get_diagnostics(&diagnostics);
        if (diagnostics.connected || diagnostics.device_present) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGI(kTag, "USB host installed=%s; internal free=%u largest=%u",
             usb_audio_host_installed() ? "yes" : "no",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
#endif
    // Every non-maintenance boot is an audio boot, even with the USB host
    // disabled, so the mode button always knows where it goes next.
    coyopedal_remote_mark_audio_boot();
    // USB owns its small contiguous DMA allocations first. The UI framebuffer,
    // profile staging buffer and task stack live in PSRAM; only the 1.6 KiB LCD
    // DMA stripe is taken from the remaining internal heap.
#if defined(GEA_EMBEDDED_NO_DISPLAY) && GEA_EMBEDDED_NO_DISPLAY
    // This board has no panel and no canvas, so the Gea app never mounts and the
    // bridge's own init hook never runs. What the panel build gets on the way to
    // drawing -- the flash store, the model catalog, the preset table and the
    // control model the boot preset is recalled into -- is what audio and the
    // remote API actually read, so it is brought up here by the same call the
    // panel build makes. Only the drawing is missing.
    if (!coyopedal_library_init()) {
        ESP_LOGE(kTag, "control model initialization failed; audio remains available");
    } else {
        ESP_LOGI(kTag, "headless: library and control model ready, no display on this board");
        coyopedal_remote_sync_network_ui();
    }
#else
    if (!s3_v1_ui_start()) {
        ESP_LOGE(kTag, "A2-Full UI initialization failed; audio remains available");
    } else {
        ESP_LOGI(kTag, "A2-Full display and touch UI ready");
        coyopedal_remote_sync_network_ui();
    }
#endif
    // Playing mode keeps both radios off. BOOT or the on-screen maintenance
    // action releases the audio graph before starting Wi-Fi/BLE.
    ESP_LOGI(kTag, "audio mode: Wi-Fi and BLE off; maintenance enables updates");
#if !COYOPEDAL_REMOTE_DISABLE_USB_AUDIO
    // The SPI panel needs a small contiguous internal allocation during startup.
    // Claim it before opportunistic effect-line promotion; effects then use only
    // the internal SRAM genuinely left over by the complete live application.
    if (usb_audio_begin_update()) {
        coyopedal_s3_modulation_promote_line();
        coyopedal_s3_reverb_promote_hot_lines();
        usb_audio_end_update();
    } else {
        ESP_LOGW(kTag, "could not pause audio for effect-line promotion");
    }
    // A measurement window confirms the pedal is engaged through the same
    // UI path the footswitch uses. The UI CHECK line it logs is the evidence.
    coyopedal_remote_audio_window_configure();
#endif
    coyopedal_remote_mode_start();
    coyopedal_remote_validate_running_ota();
    coyopedal_remote_start_requested_diagnostics();
}
