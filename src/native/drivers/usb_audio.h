#pragma once

// Generic USB Audio host: any UAC2 interface found from its descriptors, plus a
// fixed UAC1 profile for the iRig HD 2. See docs/USB_AUDIO.md.

#include <cstddef>
#include <cstdint>

struct usb_audio_diagnostics_t {
    bool connected;
    bool device_present;
    bool transport_supported;
    std::uint16_t vendor_id;
    std::uint16_t product_id;
    std::uint16_t capture_mps;
    std::uint16_t playback_mps;
    std::uint8_t usb_speed;
    std::uint8_t audio_protocol;
    std::uint64_t captured_frames;
    std::uint64_t played_frames;
    std::uint64_t silent_frames;
    std::uint64_t trimmed_frames;
    std::uint64_t transfer_errors;
    std::uint64_t feedback_packets;
    std::uint32_t feedback_16_16;
    std::uint32_t input_dropped_frames;
    std::uint32_t output_dropped_frames;
    std::uint32_t deadline_misses;
    std::uint32_t input_ring_frames;
    std::uint32_t output_ring_frames;
    std::uint32_t load_cycles_per_block;
    std::uint32_t stage_a_recent_cycles;
    std::uint32_t stage_b_recent_cycles;
    std::uint32_t stage_a_stack_free;
    std::uint32_t stage_b_stack_free;
    std::uint32_t stage_a_cycles_per_block;
    std::uint32_t stage_b_cycles_per_block;
    std::uint32_t stage_a_max_cycles;
    std::uint32_t stage_b_max_cycles;
    std::uint32_t stage_a_deadline_misses;
    std::uint32_t stage_b_deadline_misses;
    std::uint32_t usb_capture_callback_cycles;
    std::uint32_t usb_capture_callback_count;
    std::uint32_t usb_playback_callback_cycles;
    std::uint32_t usb_playback_callback_count;
    std::uint32_t usb_feedback_callback_cycles;
    std::uint32_t usb_feedback_callback_count;
    std::uint32_t usb_callback_cycles_per_block;
    std::uint32_t usb_resubmit_cycles;
    std::uint32_t usb_resubmit_count;
    std::uint64_t output_clipped_samples;
    // Absolute peak of the block as it left the input ring (before any DSP) and
    // as it entered the output ring. Together they say which side of the DSP a
    // silence lives on: input 0 means nothing arrives from the interface, input
    // non-zero with output 0 means the chain swallows it.
    float input_peak;
    // Peak of each interface channel as received, before the mono fold.
    float input_channel_peak[2];
    float output_peak;
};

// Creates the DSP stages and the USB host tasks. Idempotent.
void usb_audio_start();
// Put stage A on core 1 and stage B on core 0 when the stages are next
// created (the audio window's SWAP option).
void usb_audio_swap_stage_cores(bool swapped);
// Frames the transport has captured so far; unlike usb_audio_get_diagnostics()
// this resets nothing.
std::uint64_t usb_audio_captured_frames();
// Stage maxima and misses since usb_audio_window_reset(), with the first stage
// A overruns' timestamps (ms since boot) and cycle counts. The heartbeat task
// resets the interval counters in usb_audio_diagnostics_t; these it leaves alone.
struct usb_audio_window_stats_t {
    std::uint32_t stage_a_max_cycles;
    std::uint32_t stage_b_max_cycles;
    std::uint32_t stage_a_misses;
    std::uint32_t stage_b_misses;
    unsigned overruns_kept;
    std::uint32_t overrun_ms[8];
    std::uint32_t overrun_cycles[8];
};
void usb_audio_window_reset();
void usb_audio_window_stats(usb_audio_window_stats_t* stats);
// True once usb_host_install() has succeeded. The install needs contiguous
// DMA-capable internal SRAM, so the boot sequence has to be able to wait for
// it before anything else claims that memory. See src/native/main/main.cpp.
bool usb_audio_host_installed();
// Pauses the DSP pipeline at a block boundary. Calls nest; returns false if the
// in-flight blocks did not drain in time.
bool usb_audio_begin_update();
void usb_audio_end_update();
// The same pause, also holding the model mutex, for replacing the profile.
// Fails immediately if another model update is in progress.
bool usb_audio_begin_model_update();
void usb_audio_end_model_update();
bool usb_audio_load_model(const std::uint8_t* data, std::size_t size, char* error,
                          std::size_t error_capacity);
void usb_audio_set_tuner(bool active);
bool usb_audio_tuner_active();
// The footswitch bypass: the whole chain, effects included.
void usb_audio_set_pedal_bypass(bool bypass);
bool usb_audio_pedal_bypassed();
void usb_audio_get_diagnostics(usb_audio_diagnostics_t* diagnostics);
void usb_audio_reset_diagnostics();
void usb_audio_set_diagnostic_accounting(bool enabled);
void usb_audio_set_cycle_telemetry(bool enabled);
void usb_audio_capture_output_levels(bool enabled);
// Call while the DSP pipeline is paused; changing the stimulus rewrites its buffer.
bool usb_audio_set_diagnostic_input(bool enabled);
