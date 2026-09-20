#pragma once

// The pedalboard uses the ESP32-S3 A2-Full native engine.
#include "nam_a2_full_s3_native.hpp"

#include <cstddef>
#include <cstdint>

namespace coyopedal::pedal {

// One second-order section, in the direct form the RBJ cookbook's coefficients are
// written for. Three of them make the amp's tone stack.
struct Biquad {
    float b0 = 1.0F;
    float b1 = 0.0F;
    float b2 = 0.0F;
    float a1 = 0.0F;
    float a2 = 0.0F;
    float x1 = 0.0F;
    float x2 = 0.0F;
    float y1 = 0.0F;
    float y2 = 0.0F;

    void reset() noexcept;
    void low_shelf(float corner_hz, float gain_db, float sample_rate) noexcept;
    void peaking(float centre_hz, float gain_db, float q, float sample_rate) noexcept;
    void high_shelf(float corner_hz, float gain_db, float sample_rate) noexcept;

    [[nodiscard]] float process(float sample) noexcept {
        const float output = b0 * sample + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1;
        x1 = sample;
        y2 = y1;
        y1 = output;
        return output;
    }
};

// The amp block: input gain, the A2-Full profile, the tone stack and output
// level. Allocation-free.
class Processor {
  public:
    Processor() noexcept;
    void reset() noexcept;
    void set_input_gain(float gain) noexcept;
    void set_output_gain(float gain) noexcept;
    bool load_namb(const uint8_t* data, std::size_t size, char* error_message,
                   std::size_t error_message_capacity) noexcept;
    [[nodiscard]] bool model_loaded() const noexcept;
    void unload_model() noexcept {
        model_.release();
    }

    // A true bypass: the signal passes untouched, gains included. A pedal that
    // still applied its input and output trim while bypassed would change level
    // when switched, which is the one thing a bypass must not do.
    //
    // Bypassing does not unload the profile. The weights stay resident so
    // engaging again is instant, and the dilation history keeps running against
    // silence rather than being reset, so the first engaged block does not start
    // from a cold filter.
    void set_bypass(bool bypass) noexcept;
    [[nodiscard]] bool bypassed() const noexcept;

    // The amp's tone stack: bass, mid and treble in decibels, plus or minus twelve.
    //
    // It belongs to the amp rather than being a block of its own, and it runs after the
    // profile rather than before it. A real amp's tone stack is inside the preamp, ahead
    // of the distorting stages - but the profile is a capture of a whole rig with that
    // tone stack already in it, at the setting it was captured at. Putting another one in
    // front would drive the captured preamp differently, which is a change of gain
    // structure pretending to be an EQ. Behind it, these three do the job they are
    // actually wanted for: fitting a fixed capture to a real cabinet, a PA or a room.
    //
    // Three fixed corners rather than adjustable ones, because that is what a tone stack
    // is: 100 Hz, 650 Hz and 3.2 kHz, which is roughly where a Fender-style stack sits.
    void set_tone(float bass_db, float mid_db, float treble_db) noexcept;

    // The signal chain, split so the model's layers can run on two cores.
    // begin_block applies the input gain and starts the block; process_layers
    // runs any partition of the layers in order; finish_block completes the
    // model and applies tone and level. Bypass is decided in begin_block so a
    // block cannot change its mind halfway through.
    using Model = nam_bfp::A2FullS3Native;
    static constexpr std::size_t kStagedLayerCount = Model::kStagedLayerCount;

    using BlockScratch = Model::BlockScratch;

    bool begin_block(float* samples, std::size_t frames, BlockScratch& scratch) noexcept;
    void process_layers(BlockScratch& scratch, std::size_t first, std::size_t last) noexcept;
    void finish_block(BlockScratch& scratch, float* samples) noexcept;

  private:
    void refresh_tone() noexcept;

    float input_gain_ = 0.0F;
    float output_gain_ = 0.0F;
    float bass_db_ = 0.0F;
    float mid_db_ = 0.0F;
    float treble_db_ = 0.0F;
    // Whether any band is set at all. Three biquads is a trivial cost next to the
    // profile, but a tone stack sitting flat should be bit-transparent rather than
    // nearly so, and a filter chain at unity gain is not exactly unity.
    bool tone_active_ = false;
    Biquad bass_;
    Biquad mid_;
    Biquad treble_;
    bool bypass_ = false;
    Model model_;
};

} // namespace coyopedal::pedal
