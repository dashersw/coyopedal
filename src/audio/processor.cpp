#include "audio/processor.hpp"

// The per-block entry points run on both cores every block; on the ESP32-S3
// they live in IRAM so the shared instruction cache cannot lose them to the
// UI's PSRAM traffic (see the note on COYOPEDAL_S3_BLOCK_HOT in nam_a2_full_s3_native.cpp).
// A plain section attribute rather than esp_attr.h's IRAM_ATTR: this component
// builds with -Wpedantic -Werror and the IDF header chain trips it.
#if defined(ESP_PLATFORM)
#define COYOPEDAL_PEDAL_BLOCK_HOT __attribute__((section(".iram1.processor_hot")))
#else
#define COYOPEDAL_PEDAL_BLOCK_HOT
#endif

#include <cmath>

namespace coyopedal::pedal {

namespace {
constexpr float kSampleRate = 48000.0F;
constexpr float kTwoPi = 6.283185307F;
} // namespace

void Biquad::reset() noexcept {
    x1 = 0.0F;
    x2 = 0.0F;
    y1 = 0.0F;
    y2 = 0.0F;
}

// The three below are the Robert Bristow-Johnson cookbook forms, normalised by a0.
//
// The shelves use S = 1, the gentlest slope that does not overshoot, and at S = 1 the
// cookbook's shelf term collapses: 2*sqrt(A)*alpha is sin(w0) * sqrt((A^2+1)*(1/S - 1) +
// 2A), and (1/S - 1) is zero, leaving sin(w0)*sqrt(2A). Keeping the general form but
// dropping the (1/S - 1) factor instead gives a low shelf with several dB of gain at
// 8 kHz.
void Biquad::low_shelf(const float corner_hz, const float gain_db,
                       const float sample_rate) noexcept {
    const float amplitude = std::pow(10.0F, gain_db / 40.0F);
    const float omega = kTwoPi * corner_hz / sample_rate;
    const float cosine = std::cos(omega);
    const float root = std::sin(omega) * std::sqrt(2.0F * amplitude);
    const float a0 = (amplitude + 1.0F) + (amplitude - 1.0F) * cosine + root;
    b0 = amplitude * ((amplitude + 1.0F) - (amplitude - 1.0F) * cosine + root) / a0;
    b1 = 2.0F * amplitude * ((amplitude - 1.0F) - (amplitude + 1.0F) * cosine) / a0;
    b2 = amplitude * ((amplitude + 1.0F) - (amplitude - 1.0F) * cosine - root) / a0;
    a1 = -2.0F * ((amplitude - 1.0F) + (amplitude + 1.0F) * cosine) / a0;
    a2 = ((amplitude + 1.0F) + (amplitude - 1.0F) * cosine - root) / a0;
}

void Biquad::peaking(const float centre_hz, const float gain_db, const float q,
                     const float sample_rate) noexcept {
    const float amplitude = std::pow(10.0F, gain_db / 40.0F);
    const float omega = kTwoPi * centre_hz / sample_rate;
    const float cosine = std::cos(omega);
    const float alpha = std::sin(omega) / (2.0F * q);
    const float a0 = 1.0F + alpha / amplitude;
    b0 = (1.0F + alpha * amplitude) / a0;
    b1 = -2.0F * cosine / a0;
    b2 = (1.0F - alpha * amplitude) / a0;
    a1 = -2.0F * cosine / a0;
    a2 = (1.0F - alpha / amplitude) / a0;
}

void Biquad::high_shelf(const float corner_hz, const float gain_db,
                        const float sample_rate) noexcept {
    const float amplitude = std::pow(10.0F, gain_db / 40.0F);
    const float omega = kTwoPi * corner_hz / sample_rate;
    const float cosine = std::cos(omega);
    const float root = std::sin(omega) * std::sqrt(2.0F * amplitude);
    const float a0 = (amplitude + 1.0F) - (amplitude - 1.0F) * cosine + root;
    b0 = amplitude * ((amplitude + 1.0F) + (amplitude - 1.0F) * cosine + root) / a0;
    b1 = -2.0F * amplitude * ((amplitude - 1.0F) + (amplitude + 1.0F) * cosine) / a0;
    b2 = amplitude * ((amplitude + 1.0F) + (amplitude - 1.0F) * cosine - root) / a0;
    a1 = 2.0F * ((amplitude - 1.0F) - (amplitude + 1.0F) * cosine) / a0;
    a2 = ((amplitude + 1.0F) - (amplitude - 1.0F) * cosine - root) / a0;
}

Processor::Processor() noexcept {
    reset();
}

void Processor::reset() noexcept {
    requested_controls_ = {};
    bass_db_ = 0.0F;
    mid_db_ = 0.0F;
    treble_db_ = 0.0F;
    refresh_tone();
    bass_.reset();
    mid_.reset();
    treble_.reset();
    model_.reset();
}

void Processor::set_input_gain(const float gain) noexcept {
    requested_controls_.input_gain = gain;
    controls_.publish(requested_controls_);
}

void Processor::set_output_gain(const float gain) noexcept {
    requested_controls_.output_gain = gain;
    controls_.publish(requested_controls_);
}

bool Processor::load_namb(const uint8_t* const data, const std::size_t size,
                          char* const error_message,
                          const std::size_t error_message_capacity) noexcept {
    return model_.load_namb(data, size, error_message, error_message_capacity);
}

bool Processor::model_loaded() const noexcept {
    return model_.loaded();
}

void Processor::set_bypass(const bool bypass) noexcept {
    bypass_.store(bypass, std::memory_order_relaxed);
}

bool Processor::bypassed() const noexcept {
    return bypass_.load(std::memory_order_relaxed);
}

void Processor::set_tone(const float bass_db, const float mid_db, const float treble_db) noexcept {
    bass_db_ = bass_db;
    mid_db_ = mid_db;
    treble_db_ = treble_db;
    refresh_tone();
}

void Processor::set_controls(const float input_gain, const float output_gain, const float bass_db,
                             const float mid_db, const float treble_db) noexcept {
    requested_controls_.input_gain = input_gain;
    requested_controls_.output_gain = output_gain;
    set_tone(bass_db, mid_db, treble_db);
}

void Processor::refresh_tone() noexcept {
    // A twentieth of a decibel is below anything audible and below the panel's own half
    // decibel step, so a stack within that of flat is treated as flat and skipped.
    constexpr float kFlat = 0.05F;
    requested_controls_.tone_active =
        std::fabs(bass_db_) > kFlat || std::fabs(mid_db_) > kFlat || std::fabs(treble_db_) > kFlat;
    if (!requested_controls_.tone_active) {
        controls_.publish(requested_controls_);
        return;
    }
    // Corners of a Fender-style stack. The mid's Q is deliberately low: a tone control
    // shapes a region, and anything narrower reads as a notch rather than as tone.
    Biquad bass, mid, treble;
    bass.low_shelf(100.0F, bass_db_, kSampleRate);
    mid.peaking(650.0F, mid_db_, 0.7F, kSampleRate);
    treble.high_shelf(3200.0F, treble_db_, kSampleRate);
    requested_controls_.bass = bass;
    requested_controls_.mid = mid;
    requested_controls_.treble = treble;
    controls_.publish(requested_controls_);
}

COYOPEDAL_PEDAL_BLOCK_HOT
bool Processor::begin_block(float* const samples, const std::size_t frames,
                            BlockScratch& scratch) noexcept {
    if (bypassed() || !model_.loaded()) {
        return false;
    }
    if (const Controls* next = controls_.consume())
        current_controls_ = *next;
    scratch.controls = current_controls_;
    // The input converter applies gain before Q15 scaling, preserving the two
    // rounding steps while avoiding a separate buffer write/read pass.
    model_.begin_block(samples, frames, scratch, scratch.controls.input_gain);
    return true;
}

COYOPEDAL_PEDAL_BLOCK_HOT
void Processor::process_layers(BlockScratch& scratch, const std::size_t first,
                               const std::size_t last) noexcept {
    model_.process_layers(scratch, first, last);
}

COYOPEDAL_PEDAL_BLOCK_HOT
void Processor::finish_block(BlockScratch& scratch, float* const samples) noexcept {
    // Preserve the order of the two gain operations; combining their factors
    // would change floating-point rounding. Flat tone can finish in one pass.
    const Controls& control = scratch.controls;
    if (!control.tone_active) {
        model_.finish_block(scratch, samples, control.output_gain);
        return;
    }
    static_cast<BiquadCoefficients&>(bass_) = control.bass;
    static_cast<BiquadCoefficients&>(mid_) = control.mid;
    static_cast<BiquadCoefficients&>(treble_) = control.treble;
    model_.finish_block(scratch, samples);
    for (std::size_t index = 0; index < scratch.frames; ++index) {
        samples[index] =
            treble_.process(mid_.process(bass_.process(samples[index]))) * control.output_gain;
    }
}

} // namespace coyopedal::pedal
