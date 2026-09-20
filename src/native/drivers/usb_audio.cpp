// USB audio host: any UAC2 interface found from its descriptors, plus a fixed
// UAC1 profile for the iRig HD 2. See docs/USB_AUDIO.md.
#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>

#include "esp_cpu.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_helpers.h"
#include "usb/usb_host.h"
#include "usb/usb_types_ch9.h"

#include <new>

#include "audio/effects.h"
#include "audio/processor.hpp"
#include "audio/tuner.h"
#include "board_ui.h"
#include "uac2_pcm.hpp"
#include "usb_audio.h"
#include "usb_host_hooks.hpp"

void s3_v1_ui_wake();

extern coyopedal::pedal::Processor g_engine;

extern "C" void coyopedal_s3_pack_pcm_stereo(std::uint64_t* destination, const float* left,
                                             const float* right, std::size_t frames) noexcept;
extern "C" void coyopedal_s3_s32_to_float(float* destination, const std::int32_t* source,
                                          std::size_t frames, float scale) noexcept;

namespace {

constexpr char kTag[] = "usb_audio";
constexpr BaseType_t kUsbCore = 0;
constexpr BaseType_t kDspCore = 1;
constexpr UBaseType_t kUsbPriority = 20;
// Core 0 runs the USB client and stage A. Isochronous callbacks must preempt
// stage A: with stage A above the client, playback URBs are resubmitted after
// their isochronous slot has passed, and the transport counts an error and
// plays silence for each. The client's work therefore lands inside stage A's
// block, about 26k cycles, more when completions bunch.
constexpr UBaseType_t kStageAPriority = 19;
constexpr UBaseType_t kDspPriority = 22;
// The audio window's SWAP option: stage A on the DSP core and stage B on the
// USB core, to tell a slow core from slow work. Read when the stages are
// created, so it has to be set before usb_audio_start().
bool stage_cores_swapped{};
constexpr UBaseType_t kSetupPriority = 4;
// Reporting only: below every audio, USB and UI task, so a log line can never
// delay a URB or a DSP block.
constexpr UBaseType_t kStatsPriority = 1;
// The stage stacks are static so heap fragmentation cannot prevent the realtime
// tasks from being created. With every effect enabled a stage uses 1376 bytes,
// which leaves 608 bytes of headroom.
constexpr std::uint32_t kDspTaskStackBytes = 1984;
constexpr std::uint32_t kUsbLibraryTaskStackBytes = 3072;
static_assert(sizeof(StackType_t) == 1U, "ESP-IDF task stack sizes are expressed in bytes");

// The iRig HD 2 profile: a UAC1 device with a fixed interface layout.
constexpr std::uint16_t kHd2VendorId = 0x1963;
constexpr std::uint16_t kHd2ProductId = 0x0033;
constexpr std::uint32_t kSampleRate = 48000;
constexpr std::uint8_t kHd2PlaybackInterface = 1;
constexpr std::uint8_t kHd2PlaybackAlt = 1; // stereo 16-bit
constexpr std::uint8_t kHd2CaptureInterface = 2;
constexpr std::uint8_t kHd2CaptureAlt = 2; // mono 24-bit in 3-byte slots
constexpr std::uint8_t kHd2PlaybackEndpoint = 0x01;
constexpr std::uint8_t kHd2CaptureEndpoint = 0x82;
constexpr std::uint32_t kFramesPerPacket = kSampleRate / 1000U;
constexpr int kPacketsPerUrb = 1;
constexpr std::size_t kCaptureUrbCount = 3;
constexpr std::size_t kPlaybackUrbCount = 2;
constexpr std::size_t kFeedbackUrbCount = 2;
// Address 0 is the default pipe and never names an adopted device, so it is
// free to mean "release the current one" on the setup queue.
constexpr std::uint8_t kTeardownRequest = 0;
// One second of 5 ms polls for outstanding URBs after a disconnect.
constexpr int kTeardownDrainTicks = 200;
// The feedback endpoint carries only one 16.16 clock value per USB frame, but
// completing a one-packet URB for every value wakes the USB task 1000 times/s.
// Batch it like the audio endpoints. The most recent valid value in each URB
// becomes the rate used to size subsequent playback packets.
constexpr int kFeedbackPacketsPerUrb = 4;
constexpr std::size_t kInputRingFrames = 2048;
// DSP produces 64-frame blocks while Full Speed USB consumes about 48 frames per
// callback. A 512-frame ring absorbs callback-phase jitter without throwing
// away valid audio; it costs 4 KiB of internal SRAM.
constexpr std::size_t kOutputRingFrames = 512;
constexpr std::size_t kStartupFrames = kPlaybackUrbCount * kPacketsPerUrb * kFramesPerPacket;
// UAC1 playback is trimmed back to the startup fill, one packet per playback
// URB, once it drifts more than a packet above it.
constexpr std::size_t kTargetOutputRingFrames = kStartupFrames;
constexpr std::size_t kOutputTrimHighWaterFrames = kTargetOutputRingFrames + kFramesPerPacket;
// Capture and playback share the interface's clock, so the producer and the
// DSP consumer run at the same long-run rate and a backlog, once present, is
// permanent. Every transient that stops the consumer (all pipeline slots in
// flight, a model load, a late URB retirement) would otherwise add latency for
// the rest of the session.
// The level swings normally: the producer delivers 48 frames per packet and the
// consumer takes 64, so a depth below one block plus one packet is ordinary
// phase. Hold that as the target, and only correct above two further blocks,
// which is as deep as the three-slot pipeline can run. Anything past that
// followed a stall that already broke the audio, so shedding it in one step
// adds no new discontinuity.
constexpr std::size_t kTargetInputRingFrames = COYOPEDAL_PEDAL_BLOCK_FRAMES + kFramesPerPacket;
constexpr std::size_t kInputTrimHighWaterFrames =
    kTargetInputRingFrames + 2U * COYOPEDAL_PEDAL_BLOCK_FRAMES;
// The UAC2 output ring is clamped for the same reason: with a shared clock its
// depth would otherwise stay wherever the stream happened to start, and differ
// from boot to boot. The DSP publishes a whole block at once and the callback
// removes a whole packet at once, so the depth swings by one of each; the
// target covers that swing plus one packet, and one further block is allowed
// before correcting. Without a feedback endpoint (the XTONE Pro, for example)
// playback packets follow the capture packet sizes, and the ring depth is the
// only thing absorbing DSP completion jitter; each time it runs dry is a click.
// With rate-matched ends this corrects once after start.
constexpr std::size_t kUac2OutputTargetFrames =
    2U * kFramesPerPacket + COYOPEDAL_PEDAL_BLOCK_FRAMES;
constexpr std::size_t kUac2OutputHighWaterFrames =
    kUac2OutputTargetFrames + COYOPEDAL_PEDAL_BLOCK_FRAMES;
constexpr std::int64_t kStatsPeriodUs = 5000000;

enum class AudioProtocol : std::uint8_t {
    None = 0,
    Uac1 = 1,
    Uac2 = 2,
};

AudioProtocol audio_protocol{AudioProtocol::None};
std::uint16_t connected_vendor_id{};
std::uint16_t connected_product_id{};
usb_speed_t connected_speed{USB_SPEED_LOW};
bool device_present{};
bool transport_supported{};
std::uint8_t capture_interface{kHd2CaptureInterface};
std::uint8_t capture_alt{kHd2CaptureAlt};
std::uint8_t playback_interface{kHd2PlaybackInterface};
std::uint8_t playback_alt{kHd2PlaybackAlt};
std::uint8_t capture_endpoint{kHd2CaptureEndpoint};
std::uint8_t playback_endpoint{kHd2PlaybackEndpoint};
std::uint8_t feedback_endpoint{};
int capture_mps = 288;
int playback_mps = 384;
int feedback_mps{};
std::size_t playback_frame_bytes = 4;
uac2::Codec capture_codec{}, playback_codec{};
std::size_t capture_frame_bytes = 3;
bool capture_paced_playback{};
bool uac2_output_primed{};
// Only the USB callback task accesses capture packet credits. Shared-clock
// devices without explicit feedback replay the capture packet frame counts.
std::array<uint8_t, 64> capture_packet_frames{};
unsigned capture_packet_read{}, capture_packet_write{};
std::uint32_t nominal_frames_per_packet = kFramesPerPacket;
std::uint32_t feedback_16_16 = kFramesPerPacket << 16U;
std::uint32_t feedback_accumulator{};

template <typename Sample, std::size_t Capacity> struct SpscRing {
    static_assert((Capacity & (Capacity - 1U)) == 0U);
    Sample* samples{};
    std::atomic<std::uint32_t> read{};
    std::atomic<std::uint32_t> write{};

    struct WriteReservation {
        Sample* first{};
        std::size_t first_count{};
        Sample* second{};
        std::uint32_t sequence{};
    };

    bool init() noexcept {
        if (samples != nullptr) {
            return true;
        }
        samples = static_cast<Sample*>(heap_caps_aligned_calloc(
            64, Capacity, sizeof(Sample), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        return samples != nullptr;
    }

    [[nodiscard]] std::size_t available() const noexcept {
        const std::uint32_t r = read.load(std::memory_order_acquire);
        const std::uint32_t w = write.load(std::memory_order_acquire);
        return std::min<std::size_t>(w - r, Capacity);
    }
    bool pop_block(Sample* const out, const std::size_t count) noexcept {
        const std::uint32_t r = read.load(std::memory_order_relaxed);
        const std::uint32_t w = write.load(std::memory_order_acquire);
        if (w - r < count) {
            return false;
        }
        const std::size_t offset = r & (Capacity - 1U);
        const std::size_t first = std::min(count, Capacity - offset);
        std::memcpy(out, samples + offset, first * sizeof(Sample));
        std::memcpy(out + first, samples, (count - first) * sizeof(Sample));
        read.store(r + static_cast<std::uint32_t>(count), std::memory_order_release);
        return true;
    }
    bool reserve_write_block(const std::size_t count, WriteReservation& reservation) noexcept {
        const std::uint32_t w = write.load(std::memory_order_relaxed);
        const std::uint32_t r = read.load(std::memory_order_acquire);
        if (Capacity - (w - r) < count) {
            return false;
        }
        const std::size_t offset = w & (Capacity - 1U);
        reservation.first = samples + offset;
        reservation.first_count = std::min(count, Capacity - offset);
        reservation.second = samples;
        reservation.sequence = w;
        return true;
    }
    void commit_write_block(const WriteReservation& reservation, const std::size_t count) noexcept {
        write.store(reservation.sequence + static_cast<std::uint32_t>(count),
                    std::memory_order_release);
    }
    std::size_t pop_some(Sample* const out, const std::size_t requested) noexcept {
        const std::uint32_t r = read.load(std::memory_order_relaxed);
        const std::uint32_t w = write.load(std::memory_order_acquire);
        const std::size_t count = std::min<std::size_t>(requested, w - r);
        const std::size_t offset = r & (Capacity - 1U);
        const std::size_t first = std::min(count, Capacity - offset);
        std::memcpy(out, samples + offset, first * sizeof(Sample));
        std::memcpy(out + first, samples, (count - first) * sizeof(Sample));
        read.store(r + static_cast<std::uint32_t>(count), std::memory_order_release);
        return count;
    }
    std::size_t push_some(const Sample* const in, const std::size_t requested,
                          std::size_t* const available_after = nullptr) noexcept {
        const std::uint32_t w = write.load(std::memory_order_relaxed);
        const std::uint32_t r = read.load(std::memory_order_acquire);
        const std::size_t count = std::min<std::size_t>(requested, Capacity - (w - r));
        const std::size_t offset = w & (Capacity - 1U);
        const std::size_t first = std::min(count, Capacity - offset);
        std::memcpy(samples + offset, in, first * sizeof(Sample));
        std::memcpy(samples, in + first, (count - first) * sizeof(Sample));
        write.store(w + static_cast<std::uint32_t>(count), std::memory_order_release);
        if (available_after != nullptr) {
            *available_after = w + count - r;
        }
        return count;
    }
    std::size_t push_s32_scaled(const std::int32_t* const in, const std::size_t requested,
                                const float scale, std::size_t* const available_after) noexcept {
        static_assert(std::is_same_v<Sample, float>);
        const std::uint32_t w = write.load(std::memory_order_relaxed);
        const std::uint32_t r = read.load(std::memory_order_acquire);
        const std::size_t count = std::min<std::size_t>(requested, Capacity - (w - r));
        const std::size_t offset = w & (Capacity - 1U);
        const std::size_t first = std::min(count, Capacity - offset);
        coyopedal_s3_s32_to_float(samples + offset, in, first, scale);
        coyopedal_s3_s32_to_float(samples, in + first, count - first, scale);
        write.store(w + static_cast<std::uint32_t>(count), std::memory_order_release);
        if (available_after != nullptr) {
            *available_after = w + count - r;
        }
        return count;
    }
    std::size_t discard(const std::size_t requested) noexcept {
        const std::uint32_t r = read.load(std::memory_order_relaxed);
        const std::uint32_t w = write.load(std::memory_order_acquire);
        const std::size_t count = std::min<std::size_t>(requested, w - r);
        read.store(r + static_cast<std::uint32_t>(count), std::memory_order_release);
        return count;
    }
    void discard_all() noexcept {
        read.store(write.load(std::memory_order_acquire), std::memory_order_release);
    }
};

struct ControlCompletion {
    SemaphoreHandle_t semaphore{};
    usb_transfer_status_t status{USB_TRANSFER_STATUS_ERROR};
};

usb_host_client_handle_t client_handle{};
usb_device_handle_t device_handle{};
QueueHandle_t setup_queue{};
std::atomic<bool> setup_started{};
std::atomic<bool> connected{};
// Outstanding isochronous URBs. Teardown after a disconnect must wait for the
// host library to hand every one of them back before the transfers are freed.
std::atomic<int> urbs_in_flight{};
bool playback_claimed{};
bool capture_claimed{};
bool playback_started{};
TaskHandle_t dsp_task_handle{};
SpscRing<float, kInputRingFrames> input_ring{};
// Final stereo frame: signed PCM16 promoted into the upper 16 bits of each
// little-endian 32-bit channel slot. For a UAC2 interface with 4-byte stereo
// slots the playback completion copies these frames straight into the transfer
// buffer. The storage is static, in internal SRAM.
alignas(64) std::uint64_t output_ring_storage[kOutputRingFrames]{};
SpscRing<std::uint64_t, kOutputRingFrames> output_ring{};
std::array<usb_transfer_t*, kCaptureUrbCount> capture_urbs{};
std::array<usb_transfer_t*, kPlaybackUrbCount> playback_urbs{};
std::array<usb_transfer_t*, kFeedbackUrbCount> feedback_urbs{};

std::uint64_t captured_frames{};
std::uint64_t played_frames{};
std::uint64_t silent_frames{};
std::uint64_t trimmed_frames{};
std::uint64_t transfer_errors{};
std::uint64_t feedback_packets{};
std::atomic<std::uint32_t> input_dropped_frames{};
std::atomic<std::uint32_t> output_dropped_frames{};
std::atomic<std::uint32_t> dsp_interval_blocks{};
// The profiler can cover tens of thousands of blocks. Keep the hot-path
// accumulators 32-bit, but store cycles in 16-cycle units so a long soak does
// not wrap and no 64-bit atomic operation enters the real-time path.
constexpr unsigned kCycleAccumulatorShift = 4U;
std::atomic<std::uint32_t> dsp_interval_cycles{};
std::atomic<std::uint32_t> dsp_stage_b_cycles{};
std::atomic<std::uint32_t> dsp_interval_deadline_misses{};
std::atomic<std::uint32_t> dsp_total_deadline_misses{};
std::atomic<std::uint32_t> dsp_stage_a_deadline_misses{};
std::atomic<std::uint32_t> dsp_stage_b_deadline_misses{};
std::atomic<std::uint32_t> dsp_stage_a_max_cycles{};
std::atomic<std::uint32_t> dsp_stage_b_max_cycles{};
// Cycle timing is on by default. A crackle is a missed deadline, so the numbers
// that identify it have to exist during ordinary playing, not only inside a
// measurement window. It costs two CCOUNT reads and a few relaxed atomics per
// block.
std::atomic<bool> cycle_telemetry_enabled{true};
// Measurement windows are opened and closed from another task while the USB
// callbacks update the plain 64-bit transport counters. Close the accounting
// window and let the callback task drain before resetting or reading them.
std::atomic<bool> diagnostic_accounting_enabled{true};
// Peak metering runs from the first block. Three compares per sample against a
// NAM block is nothing measurable, and without it a silent pedal cannot be told
// apart from a silent input without a scope on the interface.
std::atomic<bool> capture_output_levels{true};
std::atomic<std::uint64_t> output_clipped_samples{};
std::atomic<std::uint32_t> output_peak_bits{};
std::atomic<std::uint32_t> input_peak_bits{};
// Per-channel peaks measured in the USB capture callback, before the frame is
// folded to mono. The pedal takes channel 0 and the interface sends two, so
// this is what says whether the guitar is on the channel we actually read.
std::atomic<std::uint32_t> input_channel_peak_bits[2]{};

float peak_from_bits(const std::atomic<std::uint32_t>& source) {
    const std::uint32_t bits = source.load(std::memory_order_relaxed);
    float peak = 0.0F;
    std::memcpy(&peak, &bits, sizeof peak);
    return peak;
}

void record_peak(std::atomic<std::uint32_t>& target, const float peak) {
    std::uint32_t peak_bits{};
    std::memcpy(&peak_bits, &peak, sizeof peak_bits);
    std::uint32_t observed = target.load(std::memory_order_relaxed);
    while (peak_bits > observed &&
           !target.compare_exchange_weak(observed, peak_bits, std::memory_order_relaxed)) {
    }
}
constexpr std::size_t kDiagnosticInputFrames = kSampleRate;
float* diagnostic_input{};
std::size_t diagnostic_input_position{};
std::atomic<bool> diagnostic_input_enabled{};
std::atomic<std::uint32_t> dsp_stage_a_recent_cycles{};
std::atomic<std::uint32_t> dsp_stage_b_recent_cycles{};
std::uint32_t dsp_deadline_cycles{};
std::uint32_t stage_a_telemetry_cycles{};
std::uint32_t stage_a_telemetry_blocks{};
std::uint32_t stage_b_telemetry_cycles{};
std::uint32_t stage_b_telemetry_blocks{};
inline bool diagnostic_accounting_active() noexcept {
    return diagnostic_accounting_enabled.load(std::memory_order_relaxed);
}
std::atomic<std::uint32_t> usb_capture_callback_cycles{};
std::atomic<std::uint32_t> usb_capture_callback_count{};
std::atomic<std::uint32_t> usb_playback_callback_cycles{};
std::atomic<std::uint32_t> usb_playback_callback_count{};
std::atomic<std::uint32_t> usb_feedback_callback_cycles{};
std::atomic<std::uint32_t> usb_feedback_callback_count{};
std::uint32_t average_accumulated_cycles(const std::uint32_t scaled_cycles,
                                         const std::uint32_t blocks) noexcept {
    if (blocks == 0U) {
        return 0U;
    }
    return static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(scaled_cycles) << kCycleAccumulatorShift) / blocks);
}

std::uint32_t load_cycles_per_block() noexcept {
    return std::max(dsp_stage_a_recent_cycles.load(std::memory_order_relaxed),
                    dsp_stage_b_recent_cycles.load(std::memory_order_relaxed));
}

// The audio window's own view of the stages: the heartbeat task zeroes the
// interval maxima and miss counts every five seconds, so a window that reads
// those sees only the last second. These are reset by the window alone, and
// the first overruns are kept with a timestamp so they can be lined up
// against whatever else the log says happened at that moment.
constexpr unsigned kWindowOverrunSlots = 8U;
std::atomic<std::uint32_t> dsp_window_max_cycles[2]{};
std::atomic<std::uint32_t> dsp_window_misses[2]{};
std::atomic<std::uint32_t> dsp_window_overruns{};
std::uint32_t dsp_window_overrun_ms[kWindowOverrunSlots]{};
std::uint32_t dsp_window_overrun_cycles[kWindowOverrunSlots]{};

// Called by both stages every block. IRAM like the stages themselves: a
// function fetched through the instruction cache costs a cache-line fill per
// block whenever the UI's PSRAM traffic has evicted it, and the block budget
// has tens of microseconds to give, not hundreds.
IRAM_ATTR void record_stage_cycles(const bool stage_a, const std::uint32_t cycles) noexcept {
    std::atomic<std::uint32_t>& maximum = stage_a ? dsp_stage_a_max_cycles : dsp_stage_b_max_cycles;
    std::uint32_t observed = maximum.load(std::memory_order_relaxed);
    while (cycles > observed &&
           !maximum.compare_exchange_weak(observed, cycles, std::memory_order_relaxed)) {
    }
    std::atomic<std::uint32_t>& window_maximum = dsp_window_max_cycles[stage_a ? 0 : 1];
    observed = window_maximum.load(std::memory_order_relaxed);
    while (cycles > observed &&
           !window_maximum.compare_exchange_weak(observed, cycles, std::memory_order_relaxed)) {
    }
    if (cycles <= dsp_deadline_cycles) {
        return;
    }
    dsp_window_misses[stage_a ? 0 : 1].fetch_add(1U, std::memory_order_relaxed);
    if (stage_a) {
        const unsigned slot = dsp_window_overruns.fetch_add(1U, std::memory_order_relaxed);
        if (slot < kWindowOverrunSlots) {
            dsp_window_overrun_ms[slot] = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
            dsp_window_overrun_cycles[slot] = cycles;
        }
    }
    dsp_interval_deadline_misses.fetch_add(1U, std::memory_order_relaxed);
    dsp_total_deadline_misses.fetch_add(1U, std::memory_order_relaxed);
    (stage_a ? dsp_stage_a_deadline_misses : dsp_stage_b_deadline_misses)
        .fetch_add(1U, std::memory_order_relaxed);
}
std::atomic<bool> model_load_pause{};
std::atomic<bool> tuner_active{};
// The footswitch. The amp block's own switch is the engine's bypass; this one
// takes the effects with it, so the two cannot be the same flag.
std::atomic<bool> pedal_bypassed{};
unsigned control_update_depth{};
StaticSemaphore_t model_control_mutex_storage;
SemaphoreHandle_t model_control_mutex =
    xSemaphoreCreateRecursiveMutexStatic(&model_control_mutex_storage);

// Staged pipeline (see docs/ARCHITECTURE.md). Stage A, on core 0: pre-amp
// effects and amp layers below kSplitLayer. Stage B, on core 1: the remaining
// layers, the head, the second modulation block and the delay. With the reverb
// on, the block returns to stage A for the reverb and PCM packing; otherwise
// stage B packs it. With a split layer of 9 or more, stage B also runs the
// reverb and always packs. The signal order is the same in every case.
constexpr std::size_t kSplitLayer = COYOPEDAL_PEDAL_S3_SPLIT_LAYER;
static_assert(kSplitLayer > 0U && kSplitLayer < coyopedal::pedal::Processor::kStagedLayerCount);
// A third in-flight audio block absorbs the 48-frame USB packet / 64-frame DSP
// block cadence without a third copy of the 4.2 KiB hot NAM scratch. Only two
// blocks can own scratch at once; the third slot is transport elasticity.
constexpr std::size_t kResidentPipelineSlots = 3;
constexpr std::size_t kScratchSlots = 2;
constexpr std::uint32_t kResidentPipelineMask = (1U << kResidentPipelineSlots) - 1U;

struct PipelineSlot {
    float audio[coyopedal::pedal::Processor::Model::kMaxStagedFrames];
    float right[coyopedal::pedal::Processor::Model::kMaxStagedFrames];
    int scratch_index{-1};
    // Whether the effects chain ran on this block, and whether the profile did.
    // They are separate so that bypassing the amp leaves the effects running.
    bool chain{};
    bool amp{};
    bool delay{};
    bool reverb{};
    std::uint32_t stage_a_cycles{};
    std::uint32_t stage_b_cycles{};
};
PipelineSlot pipeline_slots[kResidentPipelineSlots];
coyopedal::pedal::Processor::BlockScratch pipeline_scratch[kScratchSlots];
std::atomic<std::uint32_t> active_pipeline_mask{kResidentPipelineMask};

PipelineSlot& pipeline_slot(const int slot_index) noexcept {
    configASSERT(slot_index >= 0 && slot_index < static_cast<int>(kResidentPipelineSlots));
    return pipeline_slots[slot_index];
}

template <std::size_t Capacity> struct PipelineQueue {
    static_assert((Capacity & (Capacity - 1U)) == 0U);
    int items[Capacity]{};
    std::atomic<std::uint32_t> read{};
    std::atomic<std::uint32_t> write{};

    bool push(const int item) noexcept {
        const std::uint32_t w = write.load(std::memory_order_relaxed);
        const std::uint32_t r = read.load(std::memory_order_acquire);
        if (w - r == Capacity) {
            return false;
        }
        items[w & (Capacity - 1U)] = item;
        write.store(w + 1U, std::memory_order_release);
        return true;
    }

    bool pop(int& item) noexcept {
        const std::uint32_t r = read.load(std::memory_order_relaxed);
        const std::uint32_t w = write.load(std::memory_order_acquire);
        if (r == w) {
            return false;
        }
        item = items[r & (Capacity - 1U)];
        read.store(r + 1U, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool empty() const noexcept {
        return read.load(std::memory_order_acquire) == write.load(std::memory_order_acquire);
    }

    void reset() noexcept {
        read.store(0U, std::memory_order_relaxed);
        write.store(0U, std::memory_order_relaxed);
    }
};

// Stage A is the sole producer and Stage B the sole consumer. Eight entries
// cover three-slot forward work without invoking the FreeRTOS queue machinery
// for every audio block.
PipelineQueue<8> stage_b_queue{};
PipelineQueue<4> stage_c_queue{};
std::atomic<std::uint32_t> free_slot_mask{kResidentPipelineMask};
std::atomic<std::uint32_t> free_scratch_mask{(1U << kScratchSlots) - 1U};
TaskHandle_t stage_a_handle{};
StaticTask_t stage_a_tcb{};
StaticTask_t stage_b_tcb{};
alignas(16) StackType_t stage_a_stack[kDspTaskStackBytes]{};
alignas(16) StackType_t stage_b_stack[kDspTaskStackBytes]{};

bool acquire_mask_slot(std::atomic<std::uint32_t>& mask, int& slot_index) noexcept {
    std::uint32_t available = mask.load(std::memory_order_acquire);
    while (available != 0U) {
        const std::uint32_t bit = available & (0U - available);
        const std::uint32_t remaining = available & ~bit;
        if (mask.compare_exchange_weak(available, remaining, std::memory_order_acquire,
                                       std::memory_order_relaxed)) {
            slot_index = __builtin_ctz(bit);
            return true;
        }
    }
    return false;
}

bool acquire_pipeline_slot(int& slot_index) noexcept {
    if (!acquire_mask_slot(free_slot_mask, slot_index)) {
        return false;
    }
    int scratch_index{};
    if (!acquire_mask_slot(free_scratch_mask, scratch_index)) {
        free_slot_mask.fetch_or(1U << slot_index, std::memory_order_release);
        return false;
    }
    pipeline_slot(slot_index).scratch_index = scratch_index;
    return true;
}

void release_pipeline_scratch(PipelineSlot& slot) noexcept {
    configASSERT(slot.scratch_index >= 0 && slot.scratch_index < static_cast<int>(kScratchSlots));
    free_scratch_mask.fetch_or(1U << slot.scratch_index, std::memory_order_release);
    slot.scratch_index = -1;
}

void release_pipeline_slot(const int slot_index) noexcept {
    free_slot_mask.fetch_or(1U << slot_index, std::memory_order_release);
    xTaskNotifyGive(stage_a_handle);
}

IRAM_ATTR void finish_output_slot(const int slot_index, const bool stereo_output,
                                  const bool account_to_stage_b = true) noexcept {
    constexpr std::size_t kFrames = COYOPEDAL_PEDAL_BLOCK_FRAMES;
    PipelineSlot& slot = pipeline_slot(slot_index);
    const bool collect_cycles = cycle_telemetry_enabled.load(std::memory_order_relaxed);
    const std::uint32_t start = collect_cycles ? esp_cpu_get_cycle_count() : 0U;
    if (capture_output_levels.load(std::memory_order_relaxed)) {
        float peak = 0.0F;
        std::uint64_t clipped = 0U;
        for (std::size_t index = 0; index < kFrames; ++index) {
            const float left = slot.audio[index] < 0.0F ? -slot.audio[index] : slot.audio[index];
            peak = std::max(peak, left);
            clipped += left >= 1.0F ? 1U : 0U;
            if (stereo_output) {
                const float right =
                    slot.right[index] < 0.0F ? -slot.right[index] : slot.right[index];
                peak = std::max(peak, right);
                clipped += right >= 1.0F ? 1U : 0U;
            }
        }
        output_clipped_samples.fetch_add(clipped, std::memory_order_relaxed);
        record_peak(output_peak_bits, peak);
    }
    SpscRing<std::uint64_t, kOutputRingFrames>::WriteReservation output{};
    bool output_reserved = output_ring.reserve_write_block(kFrames, output);
    const auto pcm = [](const float value) noexcept {
        const float sample = std::clamp(value, -1.0F, 1.0F);
        const float scaled = sample >= 0.0F ? sample * 32767.0F : sample * 32768.0F;
        return static_cast<std::uint16_t>(static_cast<std::int16_t>(scaled));
    };
    const auto pack = [&](std::uint64_t* const destination, const std::size_t first_frame,
                          const std::size_t frame_count) noexcept {
        if (stereo_output) {
            coyopedal_s3_pack_pcm_stereo(destination, slot.audio + first_frame,
                                         slot.right + first_frame, frame_count);
        } else {
            // With every effect bypassed the head output is mono. Convert each
            // sample once and duplicate the exact PCM16 word into both channels.
            for (std::size_t index = 0; index < frame_count; ++index) {
                const std::uint32_t sample =
                    static_cast<std::uint32_t>(static_cast<std::int32_t>(
                        static_cast<std::int16_t>(pcm(slot.audio[first_frame + index]))))
                    << 16U;
                destination[index] = static_cast<std::uint64_t>(sample) |
                                     (static_cast<std::uint64_t>(sample) << 32U);
            }
        }
    };
    if (output_reserved) {
        pack(output.first, 0U, output.first_count);
        pack(output.second, output.first_count, kFrames - output.first_count);
    }
    if (collect_cycles) {
        const std::uint32_t elapsed = esp_cpu_get_cycle_count() - start;
        const std::uint32_t block_cycles =
            account_to_stage_b ? slot.stage_b_cycles + elapsed : slot.stage_a_cycles + elapsed;
        (account_to_stage_b ? dsp_stage_b_cycles : dsp_interval_cycles)
            .fetch_add(elapsed >> kCycleAccumulatorShift, std::memory_order_relaxed);
        if (account_to_stage_b) {
            stage_b_telemetry_cycles += elapsed;
            if (++stage_b_telemetry_blocks == 256U) {
                dsp_stage_b_recent_cycles.store(stage_b_telemetry_cycles / stage_b_telemetry_blocks,
                                                std::memory_order_relaxed);
                stage_b_telemetry_cycles = 0U;
                stage_b_telemetry_blocks = 0U;
            }
        } else {
            slot.stage_a_cycles += elapsed;
            stage_a_telemetry_cycles += elapsed;
        }
        record_stage_cycles(!account_to_stage_b, block_cycles);
    }
    if (output_reserved) {
        output_ring.commit_write_block(output, kFrames);
    } else {
        if (diagnostic_accounting_active()) {
            output_dropped_frames.fetch_add(static_cast<std::uint32_t>(kFrames),
                                            std::memory_order_relaxed);
        }
    }
    release_pipeline_slot(slot_index);
}

IRAM_ATTR void stage_a_task(void*) noexcept {
    constexpr std::size_t kFrames = COYOPEDAL_PEDAL_BLOCK_FRAMES;
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (true) {
            int slot_index{};
            if (stage_c_queue.pop(slot_index)) {
                PipelineSlot& slot = pipeline_slot(slot_index);
                const bool collect_cycles = cycle_telemetry_enabled.load(std::memory_order_relaxed);
                const std::uint32_t start = collect_cycles ? esp_cpu_get_cycle_count() : 0U;
                // Stage B has already run the delay; only the reverb is left.
                if (slot.reverb) {
                    coyopedal_fx_process_reverb_stereo(slot.audio, slot.right, kFrames);
                }
                if (collect_cycles) {
                    const std::uint32_t elapsed = esp_cpu_get_cycle_count() - start;
                    slot.stage_a_cycles += elapsed;
                    dsp_interval_cycles.fetch_add(elapsed >> kCycleAccumulatorShift,
                                                  std::memory_order_relaxed);
                    stage_a_telemetry_cycles += elapsed;
                }
                // The stereo block is complete here, so pack and publish it on
                // this stage rather than sending it back to stage B.
                finish_output_slot(slot_index, slot.reverb, false);
                continue;
            }
            if (model_load_pause.load(std::memory_order_acquire)) {
                break;
            }
            // Shed a ratcheted capture backlog before taking the next block,
            // and do it ahead of the slot acquisition so a pipeline that is
            // still saturated cannot keep the excess alive. This ring is the
            // only stage of the chain with no controller pulling it back to a
            // target, so without this the depth only ever grows.
            const std::size_t input_available = input_ring.available();
            if (input_available > kInputTrimHighWaterFrames) {
                input_ring.discard(input_available - kTargetInputRingFrames);
            }
            if (input_ring.available() < kFrames || !acquire_pipeline_slot(slot_index)) {
                break;
            }
            if (model_load_pause.load(std::memory_order_acquire)) {
                release_pipeline_scratch(pipeline_slot(slot_index));
                release_pipeline_slot(slot_index);
                break;
            }
            PipelineSlot& slot = pipeline_slot(slot_index);
            input_ring.pop_block(slot.audio, kFrames);
            if (capture_output_levels.load(std::memory_order_relaxed)) {
                float peak = 0.0F;
                for (std::size_t index = 0; index < kFrames; ++index) {
                    const float sample =
                        slot.audio[index] < 0.0F ? -slot.audio[index] : slot.audio[index];
                    peak = std::max(peak, sample);
                }
                record_peak(input_peak_bits, peak);
            }
            if (diagnostic_input_enabled.load(std::memory_order_acquire)) {
                const std::size_t first =
                    std::min(kFrames, kDiagnosticInputFrames - diagnostic_input_position);
                std::memcpy(slot.audio, diagnostic_input + diagnostic_input_position,
                            first * sizeof(slot.audio[0]));
                std::memcpy(slot.audio + first, diagnostic_input,
                            (kFrames - first) * sizeof(slot.audio[0]));
                diagnostic_input_position =
                    (diagnostic_input_position + kFrames) % kDiagnosticInputFrames;
            }
            const bool collect_cycles = cycle_telemetry_enabled.load(std::memory_order_relaxed);
            const std::uint32_t start = collect_cycles ? esp_cpu_get_cycle_count() : 0U;
            slot.chain = false;
            slot.amp = false;
            slot.delay = false;
            slot.reverb = false;
            if (tuner_active.load(std::memory_order_relaxed)) {
                coyopedal_tuner_feed(slot.audio, kFrames);
                std::fill_n(slot.audio, kFrames, 0.0F);
            } else if (!pedal_bypassed.load(std::memory_order_relaxed)) {
                slot.chain = coyopedal_fx_any_enabled();
                slot.delay = coyopedal_fx_enabled(COYOPEDAL_FX_DELAY);
                slot.reverb = coyopedal_fx_enabled(COYOPEDAL_FX_REVERB);
                if (slot.chain) {
                    coyopedal_fx_process_pre(slot.audio, kFrames);
                }
                if (!g_engine.bypassed()) {
                    auto& scratch = pipeline_scratch[slot.scratch_index];
                    slot.amp = g_engine.begin_block(slot.audio, kFrames, scratch);
                    if (slot.amp) {
                        g_engine.process_layers(scratch, 0, kSplitLayer);
                    }
                }
            }
            if (collect_cycles) {
                const std::uint32_t elapsed = esp_cpu_get_cycle_count() - start;
                slot.stage_a_cycles = elapsed;
                dsp_interval_cycles.fetch_add(elapsed >> kCycleAccumulatorShift,
                                              std::memory_order_relaxed);
                stage_a_telemetry_cycles += elapsed;
                if (++stage_a_telemetry_blocks == 256U) {
                    dsp_stage_a_recent_cycles.store(stage_a_telemetry_cycles /
                                                        stage_a_telemetry_blocks,
                                                    std::memory_order_relaxed);
                    stage_a_telemetry_cycles = 0U;
                    stage_a_telemetry_blocks = 0U;
                }
            }
            const bool queued = stage_b_queue.push(slot_index);
            configASSERT(queued);
            (void)queued;
            xTaskNotifyGive(dsp_task_handle);
            if (collect_cycles) {
#if COYOPEDAL_PEDAL_S3_SPLIT_LAYER >= 9
                record_stage_cycles(true, slot.stage_a_cycles);
#else
                if (!slot.delay && !slot.reverb) {
                    record_stage_cycles(true, slot.stage_a_cycles);
                }
#endif
            }
        }
    }
}

IRAM_ATTR void stage_b_task(void*) noexcept {
    constexpr std::size_t kFrames = COYOPEDAL_PEDAL_BLOCK_FRAMES;
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        int slot_index{};
        while (stage_b_queue.pop(slot_index)) {
            PipelineSlot& slot = pipeline_slot(slot_index);
            const bool collect_cycles = cycle_telemetry_enabled.load(std::memory_order_relaxed);
            const std::uint32_t start = collect_cycles ? esp_cpu_get_cycle_count() : 0U;
            if (slot.amp) {
                auto& scratch = pipeline_scratch[slot.scratch_index];
                g_engine.process_layers(scratch, kSplitLayer,
                                        coyopedal::pedal::Processor::kStagedLayerCount);
                g_engine.finish_block(scratch, slot.audio);
            }
#if COYOPEDAL_PEDAL_S3_SPLIT_LAYER >= 9
            // With layer 8 on stage A, stage B has room for the whole post-amp
            // chain and finishes the slot itself, with no return hop.
            if (slot.delay) {
                coyopedal_fx_process_delay(slot.audio, kFrames);
            }
            if (slot.reverb) {
                coyopedal_fx_process_reverb_stereo(slot.audio, slot.right, kFrames);
            }
#else
            // Delay precedes reverb in the signal chain and fits in stage B's
            // budget, so the return stage on core 0 runs only the reverb and
            // the PCM packing.
            if (slot.delay) {
                coyopedal_fx_process_delay(slot.audio, kFrames);
                slot.delay = false;
            }
#endif
            if (collect_cycles) {
                slot.stage_b_cycles = esp_cpu_get_cycle_count() - start;
            }
            release_pipeline_scratch(slot);
            if (collect_cycles) {
                const std::uint32_t elapsed = slot.stage_b_cycles;
                dsp_stage_b_cycles.fetch_add(elapsed >> kCycleAccumulatorShift,
                                             std::memory_order_relaxed);
                stage_b_telemetry_cycles += elapsed;
                dsp_interval_blocks.fetch_add(1U, std::memory_order_relaxed);
            }
#if COYOPEDAL_PEDAL_S3_SPLIT_LAYER >= 9
            finish_output_slot(slot_index, slot.reverb);
#else
            if (slot.reverb) {
                // Stage A runs the reverb and the pack. Record stage B's time
                // before handing the slot back; the finalizer closes stage A's
                // interval only.
                if (collect_cycles) {
                    record_stage_cycles(false, slot.stage_b_cycles);
                }
                const bool queued = stage_c_queue.push(slot_index);
                configASSERT(queued);
                (void)queued;
                xTaskNotifyGive(stage_a_handle);
            } else {
                finish_output_slot(slot_index, false);
            }
#endif
        }
    }
}

void control_done(usb_transfer_t* const transfer) noexcept {
    auto* const completion = static_cast<ControlCompletion*>(transfer->context);
    completion->status = transfer->status;
    xSemaphoreGive(completion->semaphore);
}

esp_err_t control_out(const usb_setup_packet_t& setup, const std::uint8_t* const payload,
                      const std::size_t payload_size) noexcept {
    usb_transfer_t* transfer{};
    esp_err_t result = usb_host_transfer_alloc(sizeof(setup) + payload_size, 0, &transfer);
    if (result != ESP_OK) {
        return result;
    }
    ControlCompletion completion{};
    completion.semaphore = xSemaphoreCreateBinary();
    if (completion.semaphore == nullptr) {
        usb_host_transfer_free(transfer);
        return ESP_ERR_NO_MEM;
    }
    std::memcpy(transfer->data_buffer, &setup, sizeof(setup));
    if (payload_size != 0U) {
        std::memcpy(transfer->data_buffer + sizeof(setup), payload, payload_size);
    }
    transfer->num_bytes = static_cast<int>(sizeof(setup) + payload_size);
    transfer->device_handle = device_handle;
    transfer->callback = control_done;
    transfer->context = &completion;
    result = usb_host_transfer_submit_control(client_handle, transfer);
    if (result == ESP_OK) {
        xSemaphoreTake(completion.semaphore, portMAX_DELAY);
    }
    if (result == ESP_OK && completion.status != USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGE(kTag, "control transfer status %d", static_cast<int>(completion.status));
        result = ESP_FAIL;
    }
    vSemaphoreDelete(completion.semaphore);
    usb_host_transfer_free(transfer);
    return result;
}

esp_err_t set_interface(const std::uint8_t interface_number,
                        const std::uint8_t alternate_setting) noexcept {
    usb_setup_packet_t setup{};
    USB_SETUP_PACKET_INIT_SET_INTERFACE(&setup, interface_number, alternate_setting);
    return control_out(setup, nullptr, 0);
}

// UAC1: SET_CUR(SAMPLING_FREQ) addressed to the streaming endpoint itself,
// with a 3-byte little-endian sample rate.
esp_err_t set_endpoint_rate(const std::uint8_t endpoint) noexcept {
    usb_setup_packet_t setup{};
    setup.bmRequestType = USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_CLASS |
                          USB_BM_REQUEST_TYPE_RECIP_ENDPOINT;
    setup.bRequest = 0x01; // SET_CUR
    setup.wValue = 0x0100; // SAMPLING_FREQ_CONTROL
    setup.wIndex = endpoint;
    setup.wLength = 3;
    const std::array<std::uint8_t, 3> rate{
        static_cast<std::uint8_t>(kSampleRate),
        static_cast<std::uint8_t>(kSampleRate >> 8U),
        static_cast<std::uint8_t>(kSampleRate >> 16U),
    };
    return control_out(setup, rate.data(), rate.size());
}

// Descriptor negotiation runs on the setup task; callbacks still run on the
// USB task. Keep exact response-length checks for clock/selector requests.
bool uac2_control(uint8_t ac, bool in, uint8_t request, uint8_t selector, uint8_t entity,
                  uint8_t* bytes, size_t size, uint8_t channel = 0) {
    usb_setup_packet_t setup{};
    setup.bmRequestType = (in ? USB_BM_REQUEST_TYPE_DIR_IN : USB_BM_REQUEST_TYPE_DIR_OUT) |
                          USB_BM_REQUEST_TYPE_TYPE_CLASS | USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
    setup.bRequest = request;
    setup.wValue = uint16_t(selector) << 8 | channel;
    setup.wIndex = uint16_t(entity) << 8 | ac;
    setup.wLength = size;
    if (!in)
        return control_out(setup, bytes, size) == ESP_OK;
    usb_transfer_t* transfer{};
    if (usb_host_transfer_alloc(sizeof(setup) + size, 0, &transfer) != ESP_OK)
        return false;
    ControlCompletion completion{};
    completion.semaphore = xSemaphoreCreateBinary();
    if (!completion.semaphore) {
        usb_host_transfer_free(transfer);
        return false;
    }
    std::memcpy(transfer->data_buffer, &setup, sizeof(setup));
    transfer->num_bytes = sizeof(setup) + size;
    transfer->device_handle = device_handle;
    transfer->callback = control_done;
    transfer->context = &completion;
    bool ok = usb_host_transfer_submit_control(client_handle, transfer) == ESP_OK;
    if (ok) {
        xSemaphoreTake(completion.semaphore, portMAX_DELAY);
        ok = completion.status == USB_TRANSFER_STATUS_COMPLETED &&
             transfer->actual_num_bytes == int(sizeof(setup) + size);
        if (ok)
            std::memcpy(bytes, transfer->data_buffer + sizeof(setup), size);
    }
    vSemaphoreDelete(completion.semaphore);
    usb_host_transfer_free(transfer);
    return ok;
}

bool adopt_hd2_layout(const usb_config_desc_t* const config) noexcept {
    int offset = 0;
    const auto* const playback =
        usb_parse_interface_descriptor(config, kHd2PlaybackInterface, kHd2PlaybackAlt, &offset);
    const auto* const capture =
        usb_parse_interface_descriptor(config, kHd2CaptureInterface, kHd2CaptureAlt, &offset);
    const auto* const playback_ep = usb_parse_endpoint_descriptor_by_address(
        config, kHd2PlaybackInterface, kHd2PlaybackAlt, kHd2PlaybackEndpoint, &offset);
    const auto* const capture_ep = usb_parse_endpoint_descriptor_by_address(
        config, kHd2CaptureInterface, kHd2CaptureAlt, kHd2CaptureEndpoint, &offset);
    if (playback == nullptr || capture == nullptr || playback_ep == nullptr ||
        capture_ep == nullptr || playback->bInterfaceClass != USB_CLASS_AUDIO ||
        capture->bInterfaceClass != USB_CLASS_AUDIO) {
        ESP_LOGE(kTag, "config does not match the iRig HD 2 layout; dump:");
        usb_print_config_descriptor(config, nullptr);
        return false;
    }
    playback_mps = playback_ep->wMaxPacketSize;
    capture_mps = capture_ep->wMaxPacketSize;
    audio_protocol = AudioProtocol::Uac1;
    capture_interface = kHd2CaptureInterface;
    capture_alt = kHd2CaptureAlt;
    playback_interface = kHd2PlaybackInterface;
    playback_alt = kHd2PlaybackAlt;
    capture_endpoint = kHd2CaptureEndpoint;
    playback_endpoint = kHd2PlaybackEndpoint;
    feedback_endpoint = 0;
    feedback_mps = 0;
    capture_frame_bytes = 3;
    playback_frame_bytes = 4;
    nominal_frames_per_packet = kFramesPerPacket;
    feedback_16_16 = nominal_frames_per_packet << 16U;
    transport_supported = true;
    ESP_LOGI(kTag, "iRig HD 2 endpoints: playback mps=%d, capture mps=%d", playback_mps,
             capture_mps);
    return true;
}

void dump_raw_descriptors(const usb_config_desc_t* config) noexcept;

// The host owns the interface's mute and gain, and an interface that comes up
// muted or at minimum gain streams only its own noise floor. Log each Feature
// Unit's state as found, then clear mute and set 0 dB. Feature Unit controls
// are optional per channel, so every request is allowed to fail.
constexpr std::uint8_t kFuMute = 0x01;
constexpr std::uint8_t kFuVolume = 0x02;
constexpr std::uint8_t kReqSetCur = 0x01;
constexpr std::uint8_t kReqGetCur = 0x81;

void open_feature_units(const uac2::Configuration& topology, const std::uint8_t ac) {
    for (const auto& unit : topology.features) {
        if (unit.control != ac) {
            continue;
        }
        // Master is channel 0; channels are numbered from 1.
        for (std::uint8_t channel = 0; channel <= unit.channels && channel <= 2U; ++channel) {
            std::uint8_t mute = 0xFFU;
            const bool read_mute =
                uac2_control(ac, true, kReqGetCur, kFuMute, unit.id, &mute, sizeof mute, channel);
            std::uint8_t volume[2]{};
            const bool read_volume = uac2_control(ac, true, kReqGetCur, kFuVolume, unit.id, volume,
                                                  sizeof volume, channel);
            const auto gain =
                static_cast<std::int16_t>(static_cast<std::uint16_t>(volume[0]) |
                                          (static_cast<std::uint16_t>(volume[1]) << 8));
            ESP_LOGI(kTag, "feature unit %u ch%u as found: mute=%s volume=%s%.2f dB", unit.id,
                     channel, read_mute ? (mute != 0U ? "on" : "off") : "n/a",
                     read_volume ? "" : "n/a ",
                     read_volume ? static_cast<double>(gain) / 256.0 : 0.0);

            std::uint8_t clear = 0U;
            const bool wrote_mute =
                uac2_control(ac, false, kReqSetCur, kFuMute, unit.id, &clear, 1U, channel);
            std::uint8_t unity[2] = {0U, 0U}; // 0 dB in 1/256 dB steps.
            const bool wrote_volume = uac2_control(ac, false, kReqSetCur, kFuVolume, unit.id, unity,
                                                   sizeof unity, channel);
            if (wrote_mute || wrote_volume) {
                ESP_LOGI(kTag, "feature unit %u ch%u set: unmute=%s 0 dB=%s", unit.id, channel,
                         wrote_mute ? "yes" : "no", wrote_volume ? "yes" : "no");
            }
        }
    }
}

bool adopt_uac2_layout(const usb_config_desc_t* config, usb_speed_t speed) {
    uac2::Configuration topology;
    if (config->wTotalLength > 4096) {
        ESP_LOGE(kTag, "configuration exceeds 4096-byte discovery limit");
        return false;
    }
    const auto error =
        uac2::discover(reinterpret_cast<const uint8_t*>(config), config->wTotalLength, topology);
    if (error != uac2::Error::None) {
        ESP_LOGW(kTag, "UAC2 discovery: %s", uac2::errorName(error));
        dump_raw_descriptors(config);
        return false;
    }
    audio_protocol = AudioProtocol::Uac2;
    // Audio mode has no network, so the topology and raw descriptors go to the
    // reboot-surviving log ring, to be read from maintenance mode.
    ESP_LOGI(kTag, "UAC2 topology: %u stream(s) %u terminal(s) %u feature unit(s) %u clock(s)",
             static_cast<unsigned>(topology.streams.size()),
             static_cast<unsigned>(topology.terminals.size()),
             static_cast<unsigned>(topology.features.size()),
             static_cast<unsigned>(topology.clocks.size()));
    dump_raw_descriptors(config);
    for (const auto& pair : uac2::pairs(topology)) {
        ESP_LOGI(kTag,
                 "UAC2 candidate IN if=%u alt=%u ep=%02x %uch/%ubit/%uB mps=%lu; "
                 "OUT if=%u alt=%u ep=%02x %uch/%ubit/%uB mps=%lu",
                 pair.capture.interface, pair.capture.alternate, pair.capture.data.address,
                 pair.capture.format.channels, pair.capture.format.bits,
                 pair.capture.format.subslot, (unsigned long)pair.capture.data.maxBytes,
                 pair.playback.interface, pair.playback.alternate, pair.playback.data.address,
                 pair.playback.format.channels, pair.playback.format.bits,
                 pair.playback.format.subslot, (unsigned long)pair.playback.data.maxBytes);
        const auto fifo = pedalboard_usb_fifo_capacity();
        if (speed != USB_SPEED_FULL || !uac2::fullSpeed48k(pair, fifo.input, fifo.output)) {
            ESP_LOGW(kTag, "UAC2 alternate cannot carry 48 kHz with the S3 1ms service loop");
            continue;
        }
        auto io = [&](bool in, uint8_t request, uint8_t selector, uint8_t entity, uint8_t* bytes,
                      size_t size) {
            return uac2_control(pair.capture.control, in, request, selector, entity, bytes, size);
        };
        uint8_t in_root{}, out_root{}, in_controls{}, out_controls{};
        uint64_t in_n{}, in_d{}, out_n{}, out_d{};
        auto in_error = uac2::resolveClock(topology, pair.capture.control, pair.capture.clock, io,
                                           in_root, in_n, in_d, in_controls);
        auto out_error = uac2::resolveClock(topology, pair.playback.control, pair.playback.clock,
                                            io, out_root, out_n, out_d, out_controls);
        if (in_error != uac2::Error::None || out_error != uac2::Error::None) {
            ESP_LOGW(kTag, "UAC2 clock traversal: IN %s OUT %s", uac2::errorName(in_error),
                     uac2::errorName(out_error));
            continue;
        }
        // The graph has no asynchronous sample-rate converter. Independently
        // clocked duplex streams need one even when both advertise 48 kHz.
        if (in_root != out_root || in_n != out_n || in_d != out_d) {
            ESP_LOGW(kTag, "UAC2 independent duplex clocks require rate conversion");
            continue;
        }
        const auto rate_error = uac2::setRate48k(io, in_root, in_controls, in_n, in_d);
        if (rate_error != uac2::Error::None) {
            ESP_LOGW(kTag, "UAC2 48 kHz negotiation: %s", uac2::errorName(rate_error));
            continue;
        }
        open_feature_units(topology, pair.capture.control);
        if (pair.playback.control != pair.capture.control) {
            open_feature_units(topology, pair.playback.control);
        }
        if (!capture_codec.configure(pair.capture.format) ||
            !playback_codec.configure(pair.playback.format))
            continue;
        capture_interface = pair.capture.interface;
        capture_alt = pair.capture.alternate;
        playback_interface = pair.playback.interface;
        playback_alt = pair.playback.alternate;
        capture_endpoint = pair.capture.data.address;
        playback_endpoint = pair.playback.data.address;
        feedback_endpoint = pair.playback.feedback.address;
        capture_mps = pair.capture.data.maxBytes;
        playback_mps = pair.playback.data.maxBytes;
        feedback_mps = pair.playback.feedback.maxBytes;
        capture_frame_bytes = pair.capture.format.frameBytes();
        playback_frame_bytes = pair.playback.format.frameBytes();
        nominal_frames_per_packet = kFramesPerPacket;
        feedback_16_16 = kFramesPerPacket << 16;
        capture_paced_playback = feedback_endpoint == 0 && pair.playback.data.sync == 1;
        capture_packet_read = capture_packet_write = 0;
        transport_supported = true;
        ESP_LOGI(kTag, "UAC2 selected shared clock %u, 48 kHz; playback pacing=%s", in_root,
                 feedback_endpoint        ? "explicit feedback"
                 : capture_paced_playback ? "capture packets"
                                          : "USB frames");
        return true;
    }
    ESP_LOGE(kTag, "no supported UAC2 duplex alternate; descriptors follow");
    dump_raw_descriptors(config);
    return false;
}

void dump_raw_descriptors(const usb_config_desc_t* const config) noexcept {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(config);
    std::size_t offset = 0;
    while (offset + 2U <= config->wTotalLength) {
        const std::uint8_t length = bytes[offset];
        if (length < 2U || offset + length > config->wTotalLength) {
            ESP_LOGE(kTag, "invalid descriptor at offset %u: len=%u total=%u",
                     static_cast<unsigned>(offset), length, config->wTotalLength);
            return;
        }
        char line[3U * 64U + 1U]{};
        const std::size_t shown = std::min<std::size_t>(length, 64U);
        for (std::size_t i = 0; i < shown; ++i) {
            std::snprintf(line + i * 3U, sizeof(line) - i * 3U, "%02x ", bytes[offset + i]);
        }
        ESP_LOGI(kTag, "raw-desc off=%u type=0x%02x len=%u: %s", static_cast<unsigned>(offset),
                 bytes[offset + 1U], length, line);
        offset += length;
    }
}

void account_transfer_error(const usb_transfer_t* const transfer) noexcept {
    if (transfer->status != USB_TRANSFER_STATUS_NO_DEVICE &&
        transfer->status != USB_TRANSFER_STATUS_CANCELED) {
        ++transfer_errors;
        ESP_LOGW(kTag, "endpoint 0x%02x transfer status %d", transfer->bEndpointAddress,
                 static_cast<int>(transfer->status));
    }
}

// The host stack's share of every completion: what usb_host_transfer_resubmit
// costs on its own, so the window can tell decoding from queueing.
std::atomic<std::uint32_t> usb_resubmit_cycles{};
std::atomic<std::uint32_t> usb_resubmit_count{};

IRAM_ATTR esp_err_t submit_transfer(usb_transfer_t* const transfer) noexcept {
    urbs_in_flight.fetch_add(1, std::memory_order_relaxed);
    const bool collect_cycles = cycle_telemetry_enabled.load(std::memory_order_relaxed);
    const std::uint32_t start = collect_cycles ? esp_cpu_get_cycle_count() : 0U;
    const esp_err_t result = usb_host_transfer_resubmit(transfer);
    if (collect_cycles) {
        usb_resubmit_cycles.fetch_add(esp_cpu_get_cycle_count() - start, std::memory_order_relaxed);
        usb_resubmit_count.fetch_add(1U, std::memory_order_relaxed);
    }
    if (result != ESP_OK) {
        urbs_in_flight.fetch_sub(1, std::memory_order_relaxed);
        ++transfer_errors;
        ESP_LOGW(kTag, "endpoint 0x%02x submit failed: %s", transfer->bEndpointAddress,
                 esp_err_to_name(result));
    }
    return result;
}

// Signed PCM in three- or four-byte slots, one or two channels, from the ring's
// stereo words (each half a 32-bit left-aligned sample). The codec's general
// routine does the same through a function pointer per sample; a transport
// completion on the stage A core cannot afford that 96 times a millisecond.
IRAM_ATTR void encode_packed_pcm(std::uint8_t* out, const std::uint64_t* const frames,
                                 const unsigned count, const unsigned subslot,
                                 const unsigned channels) noexcept {
    for (unsigned n = 0; n < count; ++n) {
        const std::uint32_t left = static_cast<std::uint32_t>(frames[n]);
        const std::uint32_t right = static_cast<std::uint32_t>(frames[n] >> 32U);
        const std::uint32_t mono =
            static_cast<std::uint32_t>((static_cast<std::int64_t>(static_cast<std::int32_t>(left)) +
                                        static_cast<std::int32_t>(right)) /
                                       2);
        for (unsigned channel = 0; channel < channels; ++channel) {
            const std::uint32_t value = channels == 1  ? mono
                                        : channel == 0 ? left
                                        : channel == 1 ? right
                                                       : 0U;
            if (subslot == 3U) {
                out[0] = static_cast<std::uint8_t>(value >> 8U);
                out[1] = static_cast<std::uint8_t>(value >> 16U);
                out[2] = static_cast<std::uint8_t>(value >> 24U);
                out += 3;
            } else {
                std::memcpy(out, &value, 4U);
                out += 4;
            }
        }
    }
}

IRAM_ATTR void fill_playback_urb(usb_transfer_t* const transfer) noexcept {
    const bool paused = model_load_pause.load(std::memory_order_acquire);
    const bool account_audio = !paused && diagnostic_accounting_active();
    // Capture and playback share the interface's clock, and the callback phase
    // moves relative to the 64-frame DSP blocks. Each protocol trims only above
    // its own high-water mark, so that phase movement never discards valid
    // audio (which would reappear later as a silent gap).
    const std::size_t available = output_ring.available();
    const bool uac1_clamp =
        audio_protocol == AudioProtocol::Uac1 && available > kOutputTrimHighWaterFrames;
    const bool uac2_clamp =
        audio_protocol == AudioProtocol::Uac2 && available > kUac2OutputHighWaterFrames;
    if (uac1_clamp || uac2_clamp) {
        const std::size_t discarded = output_ring.discard(
            available - (uac1_clamp ? kTargetOutputRingFrames : kUac2OutputTargetFrames));
        if (account_audio) {
            trimmed_frames += discarded;
        }
    }
    std::array<std::uint32_t, kPacketsPerUrb> packet_frames{};
    std::uint32_t total_frames = 0;
    for (int packet = 0; packet < kPacketsPerUrb; ++packet) {
        std::uint32_t frames = kFramesPerPacket;
        if (audio_protocol == AudioProtocol::Uac2) {
            feedback_accumulator += feedback_16_16;
            frames = feedback_accumulator >> 16U;
            feedback_accumulator &= 0xffffU;
            const std::uint32_t maximum =
                static_cast<std::uint32_t>(playback_mps / static_cast<int>(playback_frame_bytes));
            if (frames + 4U < nominal_frames_per_packet ||
                frames > nominal_frames_per_packet + 8U || frames > maximum) {
                frames = std::min(nominal_frames_per_packet, maximum);
            }
        }
        if (capture_paced_playback) {
            if (capture_packet_read != capture_packet_write) {
                frames =
                    capture_packet_frames[capture_packet_read++ % capture_packet_frames.size()];
            } else if (account_audio && uac2_output_primed) {
                ++transfer_errors;
            }
            const unsigned maximum = std::min<unsigned>(56, playback_mps / playback_frame_bytes);
            if (frames > maximum) {
                ++transfer_errors;
                frames = maximum;
            }
        }
        packet_frames[packet] = frames;
        total_frames += frames;
        transfer->isoc_packet_desc[packet].num_bytes =
            static_cast<int>(frames * playback_frame_bytes);
    }

    // Pauses drain old samples while USB keeps clocking. On resume, refill
    // before taking real audio again; otherwise 48/49-frame USB requests can
    // catch a 64-frame producer with only a partial block in its output ring.
    if (paused)
        uac2_output_primed = false;
    else if (!uac2_output_primed && available >= kStartupFrames)
        uac2_output_primed = true;
    const bool consume = audio_protocol != AudioProtocol::Uac2 || paused || uac2_output_primed;
    std::size_t popped = 0;
    if (audio_protocol == AudioProtocol::Uac2 &&
        (playback_codec.nativeStereo && playback_codec.format.bits >= 16)) {
        // The ring already contains the device's final eight-byte UAC2 frames.
        // Copy them straight into the DMA buffer and publish the ring read once.
        if (consume)
            popped = output_ring.pop_some(reinterpret_cast<std::uint64_t*>(transfer->data_buffer),
                                          total_frames);
        std::memset(transfer->data_buffer + popped * sizeof(std::uint64_t), 0,
                    (total_frames - popped) * sizeof(std::uint64_t));
    }
    int total_bytes = 0;
    if (audio_protocol == AudioProtocol::Uac2 &&
        (playback_codec.nativeStereo && playback_codec.format.bits >= 16)) {
        total_bytes = static_cast<int>(total_frames * sizeof(std::uint64_t));
    } else {
        std::array<std::uint64_t, (kFramesPerPacket + 8U) * kPacketsPerUrb> stereo_frames;
        if (consume)
            popped = output_ring.pop_some(stereo_frames.data(), total_frames);
        std::fill_n(stereo_frames.data() + popped, total_frames - popped, 0U);
        std::size_t source_frame = 0;
        for (int packet = 0; packet < kPacketsPerUrb; ++packet) {
            const std::uint32_t frames = packet_frames[packet];
            std::uint8_t* const data = transfer->data_buffer + total_bytes;
            if (audio_protocol == AudioProtocol::Uac2 && playback_codec.packedPcm()) {
                encode_packed_pcm(data, stereo_frames.data() + source_frame, frames,
                                  playback_codec.format.subslot, playback_codec.format.channels);
            } else if (audio_protocol == AudioProtocol::Uac2) {
                playback_codec.playback(data, stereo_frames.data() + source_frame, frames);
            } else
                for (std::uint32_t frame = 0; frame < frames; ++frame) {
                    const std::uint64_t stereo = stereo_frames[source_frame + frame];
                    const std::uint16_t left = static_cast<std::uint16_t>(stereo >> 16U);
                    const std::uint16_t right = static_cast<std::uint16_t>(stereo >> 48U);
                    std::uint8_t* const f = data + frame * playback_frame_bytes;
                    f[0] = static_cast<std::uint8_t>(left);
                    f[1] = static_cast<std::uint8_t>(left >> 8U);
                    f[2] = static_cast<std::uint8_t>(right);
                    f[3] = static_cast<std::uint8_t>(right >> 8U);
                }
            source_frame += frames;
            total_bytes += static_cast<int>(frames * playback_frame_bytes);
        }
    }
    if (account_audio) {
        silent_frames += total_frames - popped;
    }
    if (account_audio) {
        played_frames += total_frames;
    }
    transfer->num_bytes = total_bytes;
}

IRAM_ATTR void playback_done(usb_transfer_t* const transfer) noexcept {
    urbs_in_flight.fetch_sub(1, std::memory_order_relaxed);
    const bool collect_cycles = cycle_telemetry_enabled.load(std::memory_order_relaxed);
    const std::uint32_t callback_start = collect_cycles ? esp_cpu_get_cycle_count() : 0U;
    if (!connected.load(std::memory_order_relaxed)) {
        return;
    }
    if (transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        account_transfer_error(transfer);
        return;
    }
    for (int packet = 0; packet < transfer->num_isoc_packets; ++packet) {
        if (transfer->isoc_packet_desc[packet].status != USB_TRANSFER_STATUS_COMPLETED)
            ++transfer_errors;
    }
    fill_playback_urb(transfer);
    submit_transfer(transfer);
    if (collect_cycles) {
        usb_playback_callback_cycles.fetch_add(esp_cpu_get_cycle_count() - callback_start,
                                               std::memory_order_relaxed);
        usb_playback_callback_count.fetch_add(1U, std::memory_order_relaxed);
    }
}

// IRAM: capture_done calls this for every packet, and the early return is
// the common case.
IRAM_ATTR void start_playback() noexcept {
    if (playback_started || output_ring.available() < kStartupFrames) {
        return;
    }
    playback_started = true;
    ESP_LOGI(kTag, "starting playback with %u buffered frames",
             static_cast<unsigned>(output_ring.available()));
    for (usb_transfer_t* const transfer : playback_urbs) {
        fill_playback_urb(transfer);
        if (submit_transfer(transfer) != ESP_OK) {
            playback_started = false;
            return;
        }
    }
}

IRAM_ATTR void capture_done(usb_transfer_t* const transfer) noexcept {
    urbs_in_flight.fetch_sub(1, std::memory_order_relaxed);
    const bool collect_cycles = cycle_telemetry_enabled.load(std::memory_order_relaxed);
    const std::uint32_t callback_start = collect_cycles ? esp_cpu_get_cycle_count() : 0U;
    if (!connected.load(std::memory_order_relaxed)) {
        return;
    }
    if (transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        account_transfer_error(transfer);
        return;
    }
    // A completed URB can contain failed individual isochronous packets.
    for (int packet = 0; packet < transfer->num_isoc_packets; ++packet) {
        if (transfer->isoc_packet_desc[packet].status != USB_TRANSFER_STATUS_COMPLETED)
            ++transfer_errors;
    }
    static unsigned initial_capture_packets{};
    if (initial_capture_packets < 4) {
        ++initial_capture_packets;
        ESP_LOGI(kTag,
                 "USB initial capture requested=%d actual=%d packet_status=%d frame_bytes=%u "
                 "raw_error=%u",
                 transfer->isoc_packet_desc[0].num_bytes,
                 transfer->isoc_packet_desc[0].actual_num_bytes,
                 int(transfer->isoc_packet_desc[0].status), unsigned(capture_frame_bytes),
                 pedalboard_usb_first_isoc_error(true));
    }
    if (capture_paced_playback) {
        for (int packet = 0; packet < kPacketsPerUrb; ++packet) {
            const auto& d = transfer->isoc_packet_desc[packet];
            if (d.status != USB_TRANSFER_STATUS_COMPLETED || d.actual_num_bytes < 0 ||
                d.actual_num_bytes > capture_mps || d.actual_num_bytes % capture_frame_bytes)
                continue;
            const auto frames = d.actual_num_bytes / capture_frame_bytes;
            if (frames > 56 ||
                capture_packet_write - capture_packet_read == capture_packet_frames.size()) {
                ++transfer_errors;
                continue;
            }
            capture_packet_frames[capture_packet_write++ % capture_packet_frames.size()] = frames;
        }
    }
    constexpr std::size_t kMaximumFramesPerPacket = 96U;
    std::array<float, kMaximumFramesPerPacket * kPacketsPerUrb> urb_samples;
    std::size_t urb_frames = 0U;
    std::size_t pushed = 0U;
    std::size_t input_frames_after = 0U;
    const bool paused = model_load_pause.load(std::memory_order_acquire);
    const bool account_audio = !paused && diagnostic_accounting_active();
    if constexpr (kPacketsPerUrb == 1) {
        const usb_isoc_packet_desc_t& descriptor = transfer->isoc_packet_desc[0];
        if (audio_protocol == AudioProtocol::Uac2 && capture_codec.nativeMono &&
            descriptor.status == USB_TRANSFER_STATUS_COMPLETED) {
            // Preserve the mono 32-bit fast path after descriptor validation.
            if (descriptor.actual_num_bytes < 0 || descriptor.actual_num_bytes > capture_mps ||
                descriptor.actual_num_bytes % 4 != 0) {
                ++transfer_errors;
                submit_transfer(transfer);
                return;
            }
            const std::size_t frames = static_cast<std::size_t>(descriptor.actual_num_bytes) >> 2U;
            if (!paused) {
                if (account_audio) {
                    captured_frames += frames;
                }
                pushed = input_ring.push_s32_scaled(
                    reinterpret_cast<const std::int32_t*>(transfer->data_buffer), frames,
                    1.0F / 2147483648.0F, &input_frames_after);
            }
            urb_frames = paused ? 0U : frames;
        }
    }
    bool decoded = false;
    if constexpr (kPacketsPerUrb == 1) {
        const usb_isoc_packet_desc_t& descriptor = transfer->isoc_packet_desc[0];
        if (audio_protocol == AudioProtocol::Uac2 && !capture_codec.nativeMono &&
            capture_codec.packedPcm() && descriptor.status == USB_TRANSFER_STATUS_COMPLETED) {
            // Signed PCM in 3- or 4-byte slots (the XTONE Pro's 24-bit
            // stereo, for example), 48 frames a millisecond on the core that
            // also runs stage A. The general loop below decodes each frame
            // through the codec's function pointer and updates the level meter
            // once per channel per frame, about 350 cycles a frame. This loop
            // keeps the packet's peaks in registers and publishes them once.
            const int bytes = descriptor.actual_num_bytes;
            const std::size_t stride = capture_frame_bytes;
            if (bytes < 0 || bytes > capture_mps || static_cast<std::size_t>(bytes) % stride != 0 ||
                static_cast<std::size_t>(bytes) / stride > kMaximumFramesPerPacket) {
                ++transfer_errors;
                submit_transfer(transfer);
                return;
            }
            const std::size_t frames = static_cast<std::size_t>(bytes) / stride;
            if (account_audio) {
                captured_frames += frames;
            }
            if (!paused) {
                const bool wide = capture_codec.format.subslot == 4;
                const bool levels = capture_output_levels.load(std::memory_order_relaxed);
                const bool stereo = capture_codec.format.channels > 1;
                constexpr float kScale = 1.0F / 2147483648.0F;
                const auto decode = [wide](const std::uint8_t* const slot) noexcept {
                    return wide ? static_cast<std::int32_t>(
                                      static_cast<std::uint32_t>(slot[0]) |
                                      (static_cast<std::uint32_t>(slot[1]) << 8U) |
                                      (static_cast<std::uint32_t>(slot[2]) << 16U) |
                                      (static_cast<std::uint32_t>(slot[3]) << 24U))
                                : static_cast<std::int32_t>(
                                      (static_cast<std::uint32_t>(slot[0]) << 8U) |
                                      (static_cast<std::uint32_t>(slot[1]) << 16U) |
                                      (static_cast<std::uint32_t>(slot[2]) << 24U));
                };
                const std::uint8_t* slot = transfer->data_buffer;
                float peak_left = 0.0F;
                float peak_right = 0.0F;
                for (std::size_t frame = 0; frame < frames; ++frame, slot += stride) {
                    const float sample = static_cast<float>(decode(slot)) * kScale;
                    urb_samples[frame] = sample;
                    if (levels) {
                        peak_left = std::max(peak_left, sample < 0.0F ? -sample : sample);
                        if (stereo) {
                            const float right =
                                static_cast<float>(decode(slot + (wide ? 4U : 3U))) * kScale;
                            peak_right = std::max(peak_right, right < 0.0F ? -right : right);
                        }
                    }
                }
                if (levels) {
                    record_peak(input_channel_peak_bits[0], peak_left);
                    if (stereo) {
                        record_peak(input_channel_peak_bits[1], peak_right);
                    }
                }
                pushed = input_ring.push_some(urb_samples.data(), frames, &input_frames_after);
                urb_frames = frames;
            }
            decoded = true;
        }
    }
    if (!decoded && (audio_protocol != AudioProtocol::Uac2 || !capture_codec.nativeMono ||
                     kPacketsPerUrb != 1)) {
        for (int packet = 0; packet < kPacketsPerUrb; ++packet) {
            const usb_isoc_packet_desc_t& descriptor = transfer->isoc_packet_desc[packet];
            if (descriptor.status != USB_TRANSFER_STATUS_COMPLETED) {
                continue;
            }
            const std::uint8_t* const data = transfer->data_buffer + packet * capture_mps;
            const int frames = descriptor.actual_num_bytes / static_cast<int>(capture_frame_bytes);
            if (descriptor.actual_num_bytes < 0 || descriptor.actual_num_bytes > capture_mps ||
                descriptor.actual_num_bytes % capture_frame_bytes ||
                frames > int(kMaximumFramesPerPacket)) {
                ++transfer_errors;
                continue;
            }
            if (account_audio) {
                captured_frames += static_cast<std::uint64_t>(frames);
            }
            if (paused) {
                continue;
            }
            for (int frame = 0; frame < frames; ++frame) {
                const std::uint8_t* const slot = data + frame * capture_frame_bytes;
                float sample{};
                if (audio_protocol == AudioProtocol::Uac2) {
                    sample =
                        static_cast<float>(capture_codec.capture(slot, 0)) * (1.0F / 2147483648.0F);
                    if (capture_output_levels.load(std::memory_order_relaxed)) {
                        for (unsigned channel = 0; channel < 2U; ++channel) {
                            const float value =
                                static_cast<float>(capture_codec.capture(slot, channel)) *
                                (1.0F / 2147483648.0F);
                            record_peak(input_channel_peak_bits[channel],
                                        value < 0.0F ? -value : value);
                        }
                    }
                } else {
                    // Sign-extend packed s24le and normalize to [-1, 1).
                    const std::int32_t s24 =
                        static_cast<std::int32_t>((slot[0] << 8) | (slot[1] << 16) |
                                                  (static_cast<std::uint32_t>(slot[2]) << 24)) >>
                        8;
                    sample = static_cast<float>(s24) * (1.0F / 8388608.0F);
                }
                urb_samples[urb_frames + static_cast<std::size_t>(frame)] = sample;
            }
            urb_frames += static_cast<std::size_t>(frames);
        }

        // Publish the whole URB to the PSRAM ring in one push.
        pushed =
            paused ? 0U : input_ring.push_some(urb_samples.data(), urb_frames, &input_frames_after);
    }
    if (account_audio && pushed != urb_frames) {
        input_dropped_frames.fetch_add(static_cast<std::uint32_t>(urb_frames - pushed),
                                       std::memory_order_relaxed);
    }
    // A Full-Speed packet contains 48 frames while the DSP consumes 64. Do not
    // wake stage A for the 250 packets/s that still leave a sub-block remainder.
    if (pushed != 0U && input_frames_after >= COYOPEDAL_PEDAL_BLOCK_FRAMES) {
        xTaskNotifyGive(stage_a_handle);
    }
    start_playback();
    submit_transfer(transfer);
    if (collect_cycles) {
        usb_capture_callback_cycles.fetch_add(esp_cpu_get_cycle_count() - callback_start,
                                              std::memory_order_relaxed);
        usb_capture_callback_count.fetch_add(1U, std::memory_order_relaxed);
    }
}

IRAM_ATTR void feedback_done(usb_transfer_t* const transfer) noexcept {
    urbs_in_flight.fetch_sub(1, std::memory_order_relaxed);
    const bool collect_cycles = cycle_telemetry_enabled.load(std::memory_order_relaxed);
    const std::uint32_t callback_start = collect_cycles ? esp_cpu_get_cycle_count() : 0U;
    if (!connected.load(std::memory_order_relaxed)) {
        return;
    }
    if (transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        account_transfer_error(transfer);
        return;
    }
    for (int index = 0; index < kFeedbackPacketsPerUrb; ++index) {
        const usb_isoc_packet_desc_t& packet = transfer->isoc_packet_desc[index];
        if (packet.status == USB_TRANSFER_STATUS_COMPLETED) {
            const std::uint8_t* const data = transfer->data_buffer + index * feedback_mps;
            std::uint32_t value{};
            if (packet.actual_num_bytes >= 4) {
                value = static_cast<std::uint32_t>(data[0]) |
                        static_cast<std::uint32_t>(data[1]) << 8U |
                        static_cast<std::uint32_t>(data[2]) << 16U |
                        static_cast<std::uint32_t>(data[3]) << 24U;
            } else if (packet.actual_num_bytes == 3) {
                // UAC feedback at full speed uses unsigned 10.14 samples/frame.
                value = (static_cast<std::uint32_t>(data[0]) |
                         static_cast<std::uint32_t>(data[1]) << 8U |
                         static_cast<std::uint32_t>(data[2]) << 16U)
                        << 2U;
            }
            const std::uint32_t low = (nominal_frames_per_packet - 4U) << 16U;
            const std::uint32_t high = (nominal_frames_per_packet + 8U) << 16U;
            if (value >= low && value <= high) {
                feedback_16_16 = value;
                ++feedback_packets;
            }
        }
        transfer->isoc_packet_desc[index].num_bytes = feedback_mps;
    }
    transfer->num_bytes = feedback_mps * kFeedbackPacketsPerUrb;
    submit_transfer(transfer);
    if (collect_cycles) {
        usb_feedback_callback_cycles.fetch_add(esp_cpu_get_cycle_count() - callback_start,
                                               std::memory_order_relaxed);
        usb_feedback_callback_count.fetch_add(1U, std::memory_order_relaxed);
    }
}

esp_err_t allocate_stream_transfers() noexcept {
    for (usb_transfer_t*& transfer : capture_urbs) {
        esp_err_t result =
            usb_host_transfer_alloc(capture_mps * kPacketsPerUrb, kPacketsPerUrb, &transfer);
        if (result != ESP_OK) {
            return result;
        }
        transfer->device_handle = device_handle;
        transfer->bEndpointAddress = capture_endpoint;
        transfer->callback = capture_done;
        for (int packet = 0; packet < kPacketsPerUrb; ++packet) {
            transfer->isoc_packet_desc[packet].num_bytes = capture_mps;
        }
        transfer->num_bytes = capture_mps * kPacketsPerUrb;
    }
    for (usb_transfer_t*& transfer : playback_urbs) {
        esp_err_t result =
            usb_host_transfer_alloc(playback_mps * kPacketsPerUrb, kPacketsPerUrb, &transfer);
        if (result != ESP_OK) {
            return result;
        }
        transfer->device_handle = device_handle;
        transfer->bEndpointAddress = playback_endpoint;
        transfer->callback = playback_done;
    }
    if (feedback_endpoint != 0) {
        for (usb_transfer_t*& transfer : feedback_urbs) {
            esp_err_t result = usb_host_transfer_alloc(feedback_mps * kFeedbackPacketsPerUrb,
                                                       kFeedbackPacketsPerUrb, &transfer);
            if (result != ESP_OK) {
                return result;
            }
            transfer->device_handle = device_handle;
            transfer->bEndpointAddress = feedback_endpoint;
            transfer->callback = feedback_done;
            for (int packet = 0; packet < kFeedbackPacketsPerUrb; ++packet) {
                transfer->isoc_packet_desc[packet].num_bytes = feedback_mps;
            }
            transfer->num_bytes = feedback_mps * kFeedbackPacketsPerUrb;
        }
    }
    return ESP_OK;
}

esp_err_t start_streams() noexcept {
    if (!transport_supported) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    esp_err_t result = ESP_OK;
    // Park both streaming interfaces at alternate setting 0 before selecting the
    // streaming one.
    //
    // A software reset (every OTA and every mode switch) restarts this host
    // without power-cycling the port, so the interface keeps the alternate
    // setting the previous session left it in. Selecting that same setting
    // again is a no-op for some devices: they never re-arm their isochronous
    // sink, and the session enumerates, captures and submits playback normally
    // while nothing is heard. Alternate setting 0 is the zero-bandwidth setting
    // every UAC interface must have, so going through it forces the device to
    // tear the stream down and build it back up.
    for (const std::uint8_t interface_number : {playback_interface, capture_interface}) {
        const esp_err_t parked = set_interface(interface_number, 0U);
        if (parked != ESP_OK) {
            // Not fatal: a device that refuses the zero-bandwidth setting is
            // simply one that needs no re-arming.
            ESP_LOGW(kTag, "interface %u would not park at alt 0: %s", interface_number,
                     esp_err_to_name(parked));
        }
    }
    result = set_interface(playback_interface, playback_alt);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "could not select playback alt: %s", esp_err_to_name(result));
        return result;
    }
    result = set_interface(capture_interface, capture_alt);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "could not select capture alt: %s", esp_err_to_name(result));
        return result;
    }
    if (audio_protocol == AudioProtocol::Uac1) {
        result = set_endpoint_rate(playback_endpoint);
        if (result != ESP_OK) {
            ESP_LOGW(kTag, "playback rate SET_CUR failed: %s", esp_err_to_name(result));
        }
        result = set_endpoint_rate(capture_endpoint);
        if (result != ESP_OK) {
            ESP_LOGW(kTag, "capture rate SET_CUR failed: %s", esp_err_to_name(result));
        }
    }
    result = allocate_stream_transfers();
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "could not allocate isochronous URBs: %s", esp_err_to_name(result));
        return result;
    }
    // Start duplex UAC2 output independently of capture. Some devices do not
    // produce input until OUT transactions arrive. Silence primes the transport;
    // callbacks retain the normal DSP prefill before consuming real samples.
    if (audio_protocol == AudioProtocol::Uac2) {
        std::array<uint64_t, kFramesPerPacket * kPacketsPerUrb> silence{};
        uac2_output_primed = false;
        playback_started = true;
        for (auto* transfer : playback_urbs) {
            playback_codec.playback(transfer->data_buffer, silence.data(), silence.size());
            transfer->num_bytes = silence.size() * playback_frame_bytes;
            for (int packet = 0; packet < kPacketsPerUrb; ++packet)
                transfer->isoc_packet_desc[packet].num_bytes =
                    kFramesPerPacket * playback_frame_bytes;
        }
    }
    connected.store(true, std::memory_order_relaxed);
    coyopedal_ui_set_usb(COYOPEDAL_UI_USB_STREAMING);
    s3_v1_ui_wake();
    for (usb_transfer_t* const transfer : feedback_urbs) {
        if (transfer != nullptr && submit_transfer(transfer) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    if (audio_protocol == AudioProtocol::Uac2) {
        for (auto* transfer : playback_urbs)
            if (submit_transfer(transfer) != ESP_OK)
                return ESP_FAIL;
        ESP_LOGI(kTag, "UAC2 playback primed with silence before capture");
    }
    for (usb_transfer_t* const transfer : capture_urbs) {
        if (submit_transfer(transfer) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    ESP_LOGI(kTag,
             "USB %s streaming: 48 kHz mono in -> NAM -> stereo out; "
             "%d packet/URB",
             audio_protocol == AudioProtocol::Uac2 ? "UAC2" : "iRig HD 2 UAC1", kPacketsPerUrb);
    return ESP_OK;
}

void setup_device(const std::uint8_t address) noexcept {
    if (setup_started.load(std::memory_order_relaxed)) {
        return;
    }
    esp_err_t result = usb_host_device_open(client_handle, address, &device_handle);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "could not open USB address %u: %s", address, esp_err_to_name(result));
        return;
    }
    const usb_device_desc_t* descriptor{};
    const usb_config_desc_t* config{};
    usb_device_info_t device_info{};
    result = usb_host_get_device_descriptor(device_handle, &descriptor);
    if (result == ESP_OK) {
        result = usb_host_get_active_config_descriptor(device_handle, &config);
    }
    if (result == ESP_OK) {
        result = usb_host_device_info(device_handle, &device_info);
    }
    if (result != ESP_OK || descriptor == nullptr || config == nullptr) {
        ESP_LOGE(kTag, "could not inspect USB descriptors: %s", esp_err_to_name(result));
        usb_host_device_close(client_handle, device_handle);
        device_handle = nullptr;
        return;
    }
    ESP_LOGI(kTag, "USB %04x:%04x address %u speed=%s", descriptor->idVendor, descriptor->idProduct,
             address,
             device_info.speed == USB_SPEED_HIGH   ? "high"
             : device_info.speed == USB_SPEED_FULL ? "full"
                                                   : "low");
    const bool is_hd2 =
        descriptor->idVendor == kHd2VendorId && descriptor->idProduct == kHd2ProductId;
    connected_vendor_id = descriptor->idVendor;
    connected_product_id = descriptor->idProduct;
    connected_speed = device_info.speed;
    device_present = true;
    transport_supported = false;
    if (!(is_hd2 ? adopt_hd2_layout(config) : adopt_uac2_layout(config, device_info.speed))) {
        usb_host_device_close(client_handle, device_handle);
        device_handle = nullptr;
        coyopedal_ui_set_usb(COYOPEDAL_UI_USB_MOUNTED);
        s3_v1_ui_wake();
        return;
    }
    setup_started.store(true, std::memory_order_relaxed);
    if (!transport_supported) {
        coyopedal_ui_set_usb(COYOPEDAL_UI_USB_MOUNTED);
        s3_v1_ui_wake();
        return;
    }

    // Clock requests use EP0 and do not require ownership of the AudioControl
    // interface. Claiming it would also allocate the interface's interrupt
    // endpoint, if it has one, consuming a host channel and two periodic DMA
    // descriptor lists.
    result =
        usb_host_interface_claim(client_handle, device_handle, playback_interface, playback_alt);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "playback interface claim failed");
    } else {
        playback_claimed = true;
    }
    if (result == ESP_OK) {
        result =
            usb_host_interface_claim(client_handle, device_handle, capture_interface, capture_alt);
        if (result != ESP_OK) {
            ESP_LOGE(kTag, "capture interface claim failed");
        } else {
            capture_claimed = true;
        }
    }
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "could not claim interfaces: %s", esp_err_to_name(result));
        return;
    }
    result = start_streams();
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "could not start USB audio streams: %s", esp_err_to_name(result));
    }
}

// Release everything the gone device owned so the next one can be adopted.
// Runs on the setup task, never inside the client event callback: the client
// task must stay in usb_host_client_handle_events to deliver the completions
// this waits for.
void teardown_device() noexcept {
    connected.store(false, std::memory_order_relaxed);
    playback_started = false;
    // Freeing a transfer the host library still owns corrupts its lists, so
    // wait for every outstanding URB to come back before releasing anything.
    for (int waited = 0;
         urbs_in_flight.load(std::memory_order_relaxed) > 0 && waited < kTeardownDrainTicks;
         ++waited) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    const int stranded = urbs_in_flight.exchange(0, std::memory_order_relaxed);
    if (stranded > 0) {
        ESP_LOGW(kTag, "%d isochronous transfers never completed", stranded);
    }
    for (usb_transfer_t*& transfer : capture_urbs) {
        if (transfer != nullptr) {
            usb_host_transfer_free(transfer);
            transfer = nullptr;
        }
    }
    for (usb_transfer_t*& transfer : playback_urbs) {
        if (transfer != nullptr) {
            usb_host_transfer_free(transfer);
            transfer = nullptr;
        }
    }
    for (usb_transfer_t*& transfer : feedback_urbs) {
        if (transfer != nullptr) {
            usb_host_transfer_free(transfer);
            transfer = nullptr;
        }
    }
    if (device_handle != nullptr) {
        if (playback_claimed) {
            usb_host_interface_release(client_handle, device_handle, playback_interface);
        }
        if (capture_claimed) {
            usb_host_interface_release(client_handle, device_handle, capture_interface);
        }
        const esp_err_t closed = usb_host_device_close(client_handle, device_handle);
        if (closed != ESP_OK) {
            ESP_LOGW(kTag, "could not close USB device: %s", esp_err_to_name(closed));
        }
        device_handle = nullptr;
    }
    playback_claimed = false;
    capture_claimed = false;
    // Restore the declared defaults; a later adopt_* overwrites what it uses,
    // and anything it leaves alone must not carry over from the last device.
    audio_protocol = AudioProtocol::None;
    connected_vendor_id = 0;
    connected_product_id = 0;
    connected_speed = USB_SPEED_LOW;
    device_present = false;
    transport_supported = false;
    capture_interface = kHd2CaptureInterface;
    capture_alt = kHd2CaptureAlt;
    playback_interface = kHd2PlaybackInterface;
    playback_alt = kHd2PlaybackAlt;
    capture_endpoint = kHd2CaptureEndpoint;
    playback_endpoint = kHd2PlaybackEndpoint;
    feedback_endpoint = 0;
    capture_mps = 288;
    playback_mps = 384;
    feedback_mps = 0;
    playback_frame_bytes = 4;
    capture_frame_bytes = 3;
    capture_paced_playback = false;
    uac2_output_primed = false;
    capture_packet_read = 0;
    capture_packet_write = 0;
    input_ring.discard_all();
    output_ring.discard_all();
    usb_audio_reset_diagnostics();
    coyopedal_ui_set_usb(COYOPEDAL_UI_USB_UNMOUNTED);
    s3_v1_ui_wake();
    // Adopting is re-armed last so a replug that races the teardown is picked
    // up by the queued setup that follows it, never by a half-released device.
    setup_started.store(false, std::memory_order_relaxed);
    ESP_LOGI(kTag, "USB audio released; ready for another interface");
}

void setup_task(void*) noexcept {
    while (true) {
        std::uint8_t address{};
        if (xQueueReceive(setup_queue, &address, portMAX_DELAY) == pdTRUE) {
            if (address == kTeardownRequest) {
                teardown_device();
            } else {
                setup_device(address);
            }
        }
    }
}

void client_event(const usb_host_client_event_msg_t* const event, void*) noexcept {
    if (event->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        // Always queued. setup_device re-checks the latch, so a hub enumerating
        // first still cannot use up the adoption, and a replug that arrives
        // while the previous device is still being released is handled by the
        // teardown already ahead of it in the queue instead of being dropped.
        const std::uint8_t address = event->new_dev.address;
        if (address != kTeardownRequest) {
            xQueueSend(setup_queue, &address, 0);
        }
        return;
    }
    if (event->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        // Only the adopted device's departure tears anything down; a hub or an
        // unrelated device on the same bus must not release our streams.
        if (device_handle == nullptr || event->dev_gone.dev_hdl != device_handle) {
            return;
        }
        // Stop every callback from resubmitting before anything is released.
        connected.store(false, std::memory_order_relaxed);
        device_present = false;
        coyopedal_ui_set_usb(COYOPEDAL_UI_USB_UNMOUNTED);
        s3_v1_ui_wake();
        ESP_LOGW(kTag, "USB audio disconnected; releasing for replug");
        const std::uint8_t request = kTeardownRequest;
        xQueueSend(setup_queue, &request, 0);
    }
}

// The audio heartbeat: one log line every five seconds, built from counters the
// USB callbacks and DSP stages already maintain.
//
// "No sound" has several distinct causes that look identical from the front
// panel, and this line separates them:
//   cap not moving              -> capture is not delivering; the fault is USB
//   cap moving, play == silence -> the DSP pipeline published nothing; the
//                                  output ring is starving, which is what a
//                                  stuck model_load_pause or a pipeline that
//                                  cannot get a slot looks like
//   play moving, in=0 out high  -> audio is flowing and the fault is downstream
// The log ring survives a reboot, so the numbers are readable after the fact
// from an audio boot that has no network at all.
void audio_heartbeat_task(void*) noexcept {
    const double ticks_per_us = static_cast<double>(esp_rom_get_cpu_ticks_per_us());
    std::uint64_t last_captured = 0U;
    std::uint64_t last_played = 0U;
    std::uint64_t last_silent = 0U;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(kStatsPeriodUs / 1000));
        if (!connected.load(std::memory_order_relaxed)) {
            continue;
        }
        const std::uint64_t captured = captured_frames;
        const std::uint64_t played = played_frames;
        const std::uint64_t silent = silent_frames;
        // Peaks for the interval, not since boot: a running maximum cannot tell
        // "the guitar stopped" from "the guitar played once". capture_output_levels
        // is on by default, so these cost nothing extra to read.
        const float in_peak = peak_from_bits(input_peak_bits);
        const float out_peak = peak_from_bits(output_peak_bits);
        const float ch0_peak = peak_from_bits(input_channel_peak_bits[0]);
        const float ch1_peak = peak_from_bits(input_channel_peak_bits[1]);
        input_peak_bits.store(0U, std::memory_order_relaxed);
        output_peak_bits.store(0U, std::memory_order_relaxed);
        input_channel_peak_bits[0].store(0U, std::memory_order_relaxed);
        input_channel_peak_bits[1].store(0U, std::memory_order_relaxed);
        // The timing half. A block is 1.33 ms of audio; a stage that takes
        // longer than that has already dropped a block, which is the crackle.
        // Means alone hide it -- a chain can average 60% and still miss on the
        // peaks -- so the maxima and the per-stage miss counts are what matter.
        const std::uint32_t blocks = dsp_interval_blocks.exchange(0U);
        const std::uint32_t cycles_a = dsp_interval_cycles.exchange(0U);
        const std::uint32_t cycles_b = dsp_stage_b_cycles.exchange(0U);
        const std::uint32_t max_a = dsp_stage_a_max_cycles.exchange(0U);
        const std::uint32_t max_b = dsp_stage_b_max_cycles.exchange(0U);
        const std::uint32_t miss_a = dsp_stage_a_deadline_misses.exchange(0U);
        const std::uint32_t miss_b = dsp_stage_b_deadline_misses.exchange(0U);
        dsp_interval_deadline_misses.store(0U, std::memory_order_relaxed);
        const double block_us =
            1.0e6 * COYOPEDAL_PEDAL_BLOCK_FRAMES / static_cast<double>(kSampleRate);
        const double mean_a_us =
            static_cast<double>(average_accumulated_cycles(cycles_a, blocks)) / ticks_per_us;
        const double mean_b_us =
            static_cast<double>(average_accumulated_cycles(cycles_b, blocks)) / ticks_per_us;
        ESP_LOGI(kTag,
                 "audio: cap=+%" PRIu64 " play=+%" PRIu64 " silence=+%" PRIu64
                 " rings=%u/%u err=%" PRIu64 " paused=%d bypass=%d amp=%d"
                 " in=%.5f out=%.5f ch=%.5f/%.5f"
                 " blk=%u C0=%.0f/%.0fus %.0f%% C1=%.0f/%.0fus %.0f%%"
                 " budget=%.0fus miss=%u/%u",
                 captured - last_captured, played - last_played, silent - last_silent,
                 static_cast<unsigned>(input_ring.available()),
                 static_cast<unsigned>(output_ring.available()), transfer_errors,
                 model_load_pause.load(std::memory_order_relaxed) ? 1 : 0,
                 pedal_bypassed.load(std::memory_order_relaxed) ? 1 : 0,
                 g_engine.bypassed() ? 0 : 1, static_cast<double>(in_peak),
                 static_cast<double>(out_peak), static_cast<double>(ch0_peak),
                 static_cast<double>(ch1_peak), blocks, mean_a_us,
                 static_cast<double>(max_a) / ticks_per_us, 100.0 * mean_a_us / block_us, mean_b_us,
                 static_cast<double>(max_b) / ticks_per_us, 100.0 * mean_b_us / block_us, block_us,
                 miss_a, miss_b);
        last_captured = captured;
        last_played = played;
        last_silent = silent;
    }
}

// IRAM: the loop around usb_host_client_handle_events runs once per packet on
// core 0 at a priority above stage A.
IRAM_ATTR void client_task(void*) noexcept {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    usb_host_client_config_t config{};
    config.is_synchronous = false;
    config.max_num_event_msg = 8;
    config.async.client_event_callback = client_event;
    config.async.callback_arg = nullptr;
    const esp_err_t result = usb_host_client_register(&config, &client_handle);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "could not register USB client: %s", esp_err_to_name(result));
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(kTag, "USB client ready");
    while (true) {
        usb_host_client_handle_events(client_handle, portMAX_DELAY);
    }
}

// Set once and never cleared: the host library is installed for the life of the
// boot, and the boot sequence only ever asks "may the graph take the rest of
// internal SRAM now?".
std::atomic<bool> usb_host_installed{false};

void usb_library_task(void* const client_task_handle) noexcept {
    usb_host_config_t config{};
    config.skip_phy_setup = false;
    config.intr_flags = ESP_INTR_FLAG_LOWMED;
    // The vendored host splits the usable FIFO depth reported by the controller.
    esp_err_t install_result = usb_host_install(&config);
    // The install competes with the amp's arenas for internal SRAM, and losing
    // it means a board with a loaded model and no sound. main.cpp installs the
    // host before placing the model; these retries cover the paths that cannot
    // control that order (a live mode switch, a recovery boot).
    for (unsigned attempt = 0; attempt < 6U && install_result == ESP_ERR_NO_MEM; ++attempt) {
        ESP_LOGW(kTag,
                 "USB host install is short of internal SRAM (attempt %u); free=%u largest=%u",
                 attempt + 1U, static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
        vTaskDelay(pdMS_TO_TICKS(2000));
        install_result = usb_host_install(&config);
    }
    if (install_result != ESP_OK) {
        ESP_LOGE(kTag, "could not install USB host library: %s; internal free=%u largest=%u",
                 esp_err_to_name(install_result),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
        vTaskDelete(nullptr);
        return;
    }
    usb_host_installed.store(true, std::memory_order_release);
    xTaskNotifyGive(static_cast<TaskHandle_t>(client_task_handle));
    while (true) {
        std::uint32_t event_flags{};
        const esp_err_t result = usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (result != ESP_OK) {
            ESP_LOGE(kTag, "USB host event loop stopped: %s", esp_err_to_name(result));
            vTaskDelete(nullptr);
            return;
        }
    }
}

} // namespace

bool usb_audio_host_installed() {
    return usb_host_installed.load(std::memory_order_acquire);
}

// Model replacement is task-owned and holds this recursive mutex. A remote
// PAUSE (usb_audio_begin_update) may outlive its requesting task, so it does not.
bool usb_audio_begin_model_update() {
    if (xSemaphoreTakeRecursive(model_control_mutex, 0) != pdTRUE)
        return false;
    if (usb_audio_begin_update())
        return true;
    xSemaphoreGiveRecursive(model_control_mutex);
    return false;
}
void usb_audio_end_model_update() {
    usb_audio_end_update();
    xSemaphoreGiveRecursive(model_control_mutex);
}

bool usb_audio_load_model(const std::uint8_t* const data, const std::size_t size, char* const error,
                          const std::size_t error_capacity) {
    if (!usb_audio_begin_model_update()) {
        if (error != nullptr && error_capacity != 0U) {
            std::snprintf(error, error_capacity, "audio pipeline busy");
        }
        return false;
    }
    const bool loaded = g_engine.load_namb(data, size, error, error_capacity);
    usb_audio_end_model_update();
    return loaded;
}

bool usb_audio_begin_update() {
    if (control_update_depth++ != 0U) {
        return true;
    }
    model_load_pause.store(true, std::memory_order_release);
    if (stage_a_handle == nullptr || dsp_task_handle == nullptr) {
        return true;
    }
    xTaskNotifyGive(stage_a_handle);
    for (int attempt = 0; attempt < 500; ++attempt) {
        if (free_slot_mask.load(std::memory_order_acquire) ==
                active_pipeline_mask.load(std::memory_order_acquire) &&
            free_scratch_mask.load(std::memory_order_acquire) == ((1U << kScratchSlots) - 1U) &&
            stage_b_queue.empty() && stage_c_queue.empty()) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    control_update_depth = 0U;
    model_load_pause.store(false, std::memory_order_release);
    xTaskNotifyGive(stage_a_handle);
    return false;
}

void usb_audio_end_update() {
    if (control_update_depth == 0U || --control_update_depth != 0U) {
        return;
    }
    // Clear the pause before anything can return early. The boot path pauses
    // before the stages exist, and a flag left set would mute the board for the
    // rest of the session: stage A sees the pause on every notification, no
    // block reaches the output ring, and playback pads silence. Bypass travels
    // through stage A too, so it would be silent as well.
    model_load_pause.store(false, std::memory_order_release);
    if (stage_a_handle == nullptr) {
        // Nothing is streaming and the rings may not even be allocated yet.
        return;
    }
    // Do not process the capture accumulated during a 200 ms model load. It is
    // already stale and would turn one intentional mute into ~40 ms of latency.
    input_ring.discard_all();
    xTaskNotifyGive(stage_a_handle);
}

void usb_audio_set_tuner(const bool active) {
    const bool changed = tuner_active.exchange(active, std::memory_order_relaxed) != active;
    if (changed) {
        coyopedal_tuner_reset();
    }
}

bool usb_audio_tuner_active() {
    return tuner_active.load(std::memory_order_relaxed);
}

void usb_audio_set_pedal_bypass(const bool bypass) {
    pedal_bypassed.store(bypass, std::memory_order_relaxed);
}

bool usb_audio_pedal_bypassed() {
    return pedal_bypassed.load(std::memory_order_relaxed);
}

void usb_audio_swap_stage_cores(const bool swapped) {
    stage_cores_swapped = swapped;
}

std::uint64_t usb_audio_captured_frames() {
    return captured_frames;
}

void usb_audio_window_reset() {
    for (auto& value : dsp_window_max_cycles) {
        value.store(0U, std::memory_order_relaxed);
    }
    for (auto& value : dsp_window_misses) {
        value.store(0U, std::memory_order_relaxed);
    }
    dsp_window_overruns.store(0U, std::memory_order_relaxed);
}

void usb_audio_window_stats(usb_audio_window_stats_t* const stats) {
    if (stats == nullptr) {
        return;
    }
    *stats = {};
    stats->stage_a_max_cycles = dsp_window_max_cycles[0].load(std::memory_order_relaxed);
    stats->stage_b_max_cycles = dsp_window_max_cycles[1].load(std::memory_order_relaxed);
    stats->stage_a_misses = dsp_window_misses[0].load(std::memory_order_relaxed);
    stats->stage_b_misses = dsp_window_misses[1].load(std::memory_order_relaxed);
    const unsigned kept = std::min<unsigned>(dsp_window_overruns.load(std::memory_order_relaxed),
                                             kWindowOverrunSlots);
    stats->overruns_kept = kept;
    for (unsigned slot = 0; slot < kept; ++slot) {
        stats->overrun_ms[slot] = dsp_window_overrun_ms[slot];
        stats->overrun_cycles[slot] = dsp_window_overrun_cycles[slot];
    }
}

void usb_audio_get_diagnostics(usb_audio_diagnostics_t* const diagnostics) {
    if (diagnostics == nullptr) {
        return;
    }
    const std::uint32_t blocks = dsp_interval_blocks.exchange(0U);
    const std::uint32_t stage_a_cycles = dsp_interval_cycles.exchange(0U);
    const std::uint32_t stage_b_cycles = dsp_stage_b_cycles.exchange(0U);
    const std::uint32_t capture_callback_cycles =
        usb_capture_callback_cycles.load(std::memory_order_relaxed);
    const std::uint32_t capture_callback_count =
        usb_capture_callback_count.load(std::memory_order_relaxed);
    const std::uint32_t playback_callback_cycles =
        usb_playback_callback_cycles.load(std::memory_order_relaxed);
    const std::uint32_t playback_callback_count =
        usb_playback_callback_count.load(std::memory_order_relaxed);
    const std::uint32_t feedback_callback_cycles =
        usb_feedback_callback_cycles.load(std::memory_order_relaxed);
    const std::uint32_t feedback_callback_count =
        usb_feedback_callback_count.load(std::memory_order_relaxed);
    const std::uint32_t stage_a_recent_cycles =
        dsp_stage_a_recent_cycles.load(std::memory_order_relaxed);
    const std::uint32_t stage_b_recent_cycles =
        dsp_stage_b_recent_cycles.load(std::memory_order_relaxed);
    std::uint32_t peak_bits = 0U;
    std::uint32_t input_bits = 0U;
    peak_bits = output_peak_bits.load(std::memory_order_relaxed);
    input_bits = input_peak_bits.load(std::memory_order_relaxed);
    float output_peak = 0.0F;
    std::memcpy(&output_peak, &peak_bits, sizeof output_peak);
    float input_peak = 0.0F;
    std::memcpy(&input_peak, &input_bits, sizeof input_peak);
    float channel_peak[2] = {0.0F, 0.0F};
    channel_peak[0] = peak_from_bits(input_channel_peak_bits[0]);
    channel_peak[1] = peak_from_bits(input_channel_peak_bits[1]);
    const std::uint64_t completed_blocks = captured_frames / COYOPEDAL_PEDAL_BLOCK_FRAMES;
    const std::uint32_t callback_cycles_per_block =
        completed_blocks == 0U
            ? 0U
            : static_cast<std::uint32_t>((static_cast<std::uint64_t>(capture_callback_cycles) +
                                          playback_callback_cycles + feedback_callback_cycles) /
                                         completed_blocks);
    *diagnostics = {
        .connected = connected.load(std::memory_order_relaxed),
        .device_present = device_present,
        .transport_supported = transport_supported,
        .vendor_id = connected_vendor_id,
        .product_id = connected_product_id,
        .capture_mps = static_cast<std::uint16_t>(capture_mps),
        .playback_mps = static_cast<std::uint16_t>(playback_mps),
        .usb_speed = static_cast<std::uint8_t>(connected_speed),
        .audio_protocol = static_cast<std::uint8_t>(audio_protocol),
        .captured_frames = captured_frames,
        .played_frames = played_frames,
        .silent_frames = silent_frames,
        .trimmed_frames = trimmed_frames,
        .transfer_errors = transfer_errors,
        .feedback_packets = feedback_packets,
        .feedback_16_16 = feedback_16_16,
        .input_dropped_frames = input_dropped_frames.load(std::memory_order_relaxed),
        .output_dropped_frames = output_dropped_frames.load(std::memory_order_relaxed),
        .deadline_misses = dsp_total_deadline_misses.load(std::memory_order_relaxed),
        .input_ring_frames = static_cast<std::uint32_t>(input_ring.available()),
        .output_ring_frames = static_cast<std::uint32_t>(output_ring.available()),
        .load_cycles_per_block = load_cycles_per_block(),
        .stage_a_recent_cycles = stage_a_recent_cycles,
        .stage_b_recent_cycles = stage_b_recent_cycles,
        .stage_a_stack_free =
            stage_a_handle == nullptr
                ? 0U
                : static_cast<std::uint32_t>(uxTaskGetStackHighWaterMark(stage_a_handle) *
                                             sizeof(StackType_t)),
        .stage_b_stack_free =
            dsp_task_handle == nullptr
                ? 0U
                : static_cast<std::uint32_t>(uxTaskGetStackHighWaterMark(dsp_task_handle) *
                                             sizeof(StackType_t)),
        .stage_a_cycles_per_block = average_accumulated_cycles(stage_a_cycles, blocks),
        .stage_b_cycles_per_block = average_accumulated_cycles(stage_b_cycles, blocks),
        .stage_a_max_cycles = dsp_stage_a_max_cycles.load(std::memory_order_relaxed),
        .stage_b_max_cycles = dsp_stage_b_max_cycles.load(std::memory_order_relaxed),
        .stage_a_deadline_misses = dsp_stage_a_deadline_misses.load(std::memory_order_relaxed),
        .stage_b_deadline_misses = dsp_stage_b_deadline_misses.load(std::memory_order_relaxed),
        .usb_capture_callback_cycles = capture_callback_cycles,
        .usb_capture_callback_count = capture_callback_count,
        .usb_playback_callback_cycles = playback_callback_cycles,
        .usb_playback_callback_count = playback_callback_count,
        .usb_feedback_callback_cycles = feedback_callback_cycles,
        .usb_feedback_callback_count = feedback_callback_count,
        .usb_callback_cycles_per_block = callback_cycles_per_block,
        .usb_resubmit_cycles = usb_resubmit_cycles.load(std::memory_order_relaxed),
        .usb_resubmit_count = usb_resubmit_count.load(std::memory_order_relaxed),
        .output_clipped_samples = output_clipped_samples.load(std::memory_order_relaxed),
        .input_peak = input_peak,
        .input_channel_peak = {channel_peak[0], channel_peak[1]},
        .output_peak = output_peak,
    };
}

void usb_audio_reset_diagnostics() {
    captured_frames = 0U;
    played_frames = 0U;
    silent_frames = 0U;
    trimmed_frames = 0U;
    transfer_errors = 0U;
    feedback_packets = 0U;
    input_dropped_frames.store(0U, std::memory_order_relaxed);
    output_dropped_frames.store(0U, std::memory_order_relaxed);
    dsp_interval_blocks.store(0U, std::memory_order_relaxed);
    dsp_interval_cycles.store(0U, std::memory_order_relaxed);
    dsp_stage_b_cycles.store(0U, std::memory_order_relaxed);
    dsp_interval_deadline_misses.store(0U, std::memory_order_relaxed);
    dsp_total_deadline_misses.store(0U, std::memory_order_relaxed);
    dsp_stage_a_deadline_misses.store(0U, std::memory_order_relaxed);
    dsp_stage_b_deadline_misses.store(0U, std::memory_order_relaxed);
    dsp_stage_a_max_cycles.store(0U, std::memory_order_relaxed);
    dsp_stage_b_max_cycles.store(0U, std::memory_order_relaxed);
    usb_capture_callback_cycles.store(0U, std::memory_order_relaxed);
    usb_capture_callback_count.store(0U, std::memory_order_relaxed);
    usb_playback_callback_cycles.store(0U, std::memory_order_relaxed);
    usb_playback_callback_count.store(0U, std::memory_order_relaxed);
    usb_feedback_callback_cycles.store(0U, std::memory_order_relaxed);
    usb_feedback_callback_count.store(0U, std::memory_order_relaxed);
    output_clipped_samples.store(0U, std::memory_order_relaxed);
    output_peak_bits.store(0U, std::memory_order_relaxed);
    input_peak_bits.store(0U, std::memory_order_relaxed);
    input_channel_peak_bits[0].store(0U, std::memory_order_relaxed);
    input_channel_peak_bits[1].store(0U, std::memory_order_relaxed);
}

void usb_audio_set_cycle_telemetry(const bool enabled) {
    cycle_telemetry_enabled.store(enabled, std::memory_order_release);
}

void usb_audio_set_diagnostic_accounting(const bool enabled) {
    diagnostic_accounting_enabled.store(enabled, std::memory_order_release);
}

void usb_audio_capture_output_levels(const bool enabled) {
    capture_output_levels.store(enabled, std::memory_order_release);
}

bool usb_audio_set_diagnostic_input(const bool enabled) {
    if (!enabled) {
        diagnostic_input_enabled.store(false, std::memory_order_release);
        return true;
    }
    static bool initialized = false;
    if (diagnostic_input == nullptr) {
        diagnostic_input = static_cast<float*>(heap_caps_aligned_alloc(
            16U, kDiagnosticInputFrames * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (diagnostic_input == nullptr) {
            return false;
        }
    }
    if (!initialized) {
        // Four decaying, harmonic-rich plucks exercise the same level-dependent
        // NAM and pedal paths as a guitar instead of benchmarking the closed
        // gate/silence case. This is generated outside the realtime workers.
        constexpr float kTwoPi = 6.2831853071795864769F;
        constexpr std::array<float, 4> kFrequencies = {
            82.4069F,
            110.0F,
            146.832F,
            195.998F,
        };
        constexpr std::size_t kNoteFrames = kSampleRate / kFrequencies.size();
        std::uint32_t noise = 0x74617572U;
        float filtered_noise = 0.0F;
        for (std::size_t frame = 0; frame < kDiagnosticInputFrames; ++frame) {
            const std::size_t note = frame / kNoteFrames;
            const std::size_t local_frame = frame % kNoteFrames;
            const float time = static_cast<float>(local_frame) / static_cast<float>(kSampleRate);
            const float phase = kTwoPi * kFrequencies[note] * time;
            const float envelope = std::exp(-5.0F * time);
            noise = noise * 1664525U + 1013904223U;
            const float white =
                static_cast<float>(static_cast<std::int32_t>(noise)) * (1.0F / 2147483648.0F);
            filtered_noise += 0.12F * (white - filtered_noise);
            const float attack = std::min(1.0F, static_cast<float>(local_frame) * (1.0F / 48.0F));
            diagnostic_input[frame] =
                attack * envelope *
                (0.18F * std::sin(phase) + 0.075F * std::sin(2.0F * phase + 0.31F) +
                 0.035F * std::sin(3.0F * phase + 0.73F) + 0.025F * filtered_noise);
        }
        initialized = true;
    }
    diagnostic_input_position = 0U;
    diagnostic_input_enabled.store(true, std::memory_order_release);
    return true;
}

void usb_audio_start() {
    if (stage_a_handle != nullptr)
        return;
    dsp_deadline_cycles =
        static_cast<std::uint32_t>(static_cast<std::uint64_t>(esp_rom_get_cpu_ticks_per_us()) *
                                   COYOPEDAL_PEDAL_BLOCK_FRAMES * 1000000ULL / kSampleRate);
    // The input ring is a sequential transport buffer, not DSP working state,
    // so its 8 KiB go to PSRAM; internal SRAM is kept for the amp, the pipeline
    // scratch and the USB DMA descriptors. The output ring uses its static
    // storage, so init() allocates nothing for it.
    output_ring.samples = output_ring_storage;
    if (!input_ring.init() || !output_ring.init()) {
        ESP_LOGE(kTag, "could not allocate USB audio rings in PSRAM");
        return;
    }
    setup_queue = xQueueCreate(4, sizeof(std::uint8_t));
    if (setup_queue == nullptr) {
        ESP_LOGE(kTag, "could not create setup queue");
        return;
    }
    stage_b_queue.reset();
    stage_c_queue.reset();
    active_pipeline_mask.store(kResidentPipelineMask, std::memory_order_release);
    free_slot_mask.store(kResidentPipelineMask, std::memory_order_release);
    free_scratch_mask.store((1U << kScratchSlots) - 1U, std::memory_order_release);
    TaskHandle_t client_task_handle{};
    const auto task_failed = [](const char* const name) {
        ESP_LOGE(kTag, "could not create %s task; internal free=%u largest=%u", name,
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    };
    stage_a_handle = xTaskCreateStaticPinnedToCore(
        stage_a_task, "nam_a", kDspTaskStackBytes, nullptr, kStageAPriority, stage_a_stack,
        &stage_a_tcb, stage_cores_swapped ? kDspCore : kUsbCore);
    if (stage_a_handle == nullptr) {
        task_failed("NAM stage A");
        return;
    }
    dsp_task_handle = xTaskCreateStaticPinnedToCore(
        stage_b_task, "nam_b", kDspTaskStackBytes, nullptr, kDspPriority, stage_b_stack,
        &stage_b_tcb, stage_cores_swapped ? kUsbCore : kDspCore);
    if (dsp_task_handle == nullptr) {
        task_failed("NAM stage B");
        return;
    }
    if (xTaskCreatePinnedToCoreWithCaps(client_task, "usb_client", 6144, nullptr, kUsbPriority,
                                        &client_task_handle, kUsbCore,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        task_failed("USB client");
        return;
    }
    if (xTaskCreatePinnedToCoreWithCaps(setup_task, "usb_setup", 6144, nullptr, kSetupPriority,
                                        nullptr, kUsbCore,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        task_failed("USB setup");
        return;
    }
    if (xTaskCreatePinnedToCoreWithCaps(audio_heartbeat_task, "usb_beat", 3072, nullptr,
                                        kStatsPriority, nullptr, kUsbCore,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        // Not fatal: this only reports, it does not carry audio.
        task_failed("audio heartbeat");
    }
    // The host-library task sleeps in usb_host_lib_handle_events; its stack is
    // cold. Keeping it in internal SRAM consumed the contiguous block needed
    // by the DWC isochronous pipe descriptors when the UI was also resident.
    if (xTaskCreatePinnedToCoreWithCaps(usb_library_task, "usb_host", kUsbLibraryTaskStackBytes,
                                        client_task_handle, kUsbPriority, nullptr, kUsbCore,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        task_failed("USB library");
    }
}
