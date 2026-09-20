#include "audio/effects.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <new>

namespace {

#if defined(__XTENSA__)
std::uint32_t reverb_profile_input_cycles{};
std::uint32_t reverb_profile_tank_cycles{};
std::uint32_t reverb_profile_blocks{};
bool reverb_profile_enabled{};

[[gnu::always_inline]] inline std::uint32_t read_cycle_count() noexcept {
    std::uint32_t cycles;
    asm volatile("rsr.ccount %0" : "=a"(cycles));
    return cycles;
}
#endif

constexpr float kSampleRate = 48000.0F;
constexpr float kInverseSampleRate = 1.0F / kSampleRate;

// Cheap base-2 log and exponential, taken from the float's own exponent field.
// The compressor needs a decibel value per envelope update and a linear gain
// back; a libm log10/pow pair costs more than the whole reverb, and neither needs
// more accuracy than this, since the result is smoothed over 16 samples anyway.
float fast_log2(const float value) noexcept {
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    const float exponent = static_cast<float>(static_cast<int>((bits >> 23U) & 0xFFU) - 127);
    // Mantissa mapped back into [1, 2), then a quadratic fit of log2 over it.
    const float mantissa = std::bit_cast<float>((bits & 0x007FFFFFU) | 0x3F800000U);
    return exponent + (-0.3358287811F * mantissa + 2.0F) * mantissa - 1.65871759F;
}

float fast_exp2(const float value) noexcept {
    const float clamped = std::clamp(value, -126.0F, 126.0F);
    int whole = static_cast<int>(clamped);
    whole -= static_cast<float>(whole) > clamped;
    const float fraction = clamped - static_cast<float>(whole);
    // Quadratic fit of 2^f over [0, 1), then scaled by the integer part.
    const float mantissa = 1.0F + fraction * (0.6565848F + fraction * 0.3432120F);
    const std::uint32_t bits = static_cast<std::uint32_t>(whole + 127) << 23U;
    return mantissa * std::bit_cast<float>(bits);
}

constexpr float kDecibelsPerLog2 = 6.020599913F; // 20 * log10(2)

float linear_to_decibels(const float value) noexcept {
    return kDecibelsPerLog2 * fast_log2(std::max(value, 1.0e-9F));
}

float decibels_to_linear(const float decibels) noexcept {
    return fast_exp2(decibels * (1.0F / kDecibelsPerLog2));
}

[[gnu::always_inline]] inline float realtime_reciprocal(const float denominator) noexcept {
#if defined(__XTENSA__)
    // LX7 supplies a reciprocal estimate but no hardware scalar divide. Two
    // Newton steps give float precision without calling libgcc's __divsf3.
    float reciprocal;
    asm("recip0.s %0, %1" : "=f"(reciprocal) : "f"(denominator));
    reciprocal *= 2.0F - denominator * reciprocal;
    reciprocal *= 2.0F - denominator * reciprocal;
    return reciprocal;
#else
    return 1.0F / denominator;
#endif
}

[[maybe_unused, gnu::always_inline]] inline std::int32_t
realtime_round_to_int32(const float value) noexcept {
#if defined(__XTENSA__)
    std::int32_t rounded;
    asm("round.s %0, %1, 0" : "=a"(rounded) : "f"(value));
    return rounded;
#else
    return static_cast<std::int32_t>(value + (value >= 0.0F ? 0.5F : -0.5F));
#endif
}

// A small global trim leaves headroom for the calibrated tank. Keeping it here
// costs nothing in the sample loop: configure() stores the already-trimmed gain.
constexpr float kReverbWetTrim = 0.5011872F; // -6.00 dB total.

// Below noon the reverb MIX control behaves like an aux send: it adds ambience
// without turning the direct guitar down. The parabolic send reaches unity at
// noon, giving useful resolution to the 3-35% range used by factory presets rather
// than burying an already-diffuse tail another 9-30 dB. Above noon the send remains
// at its trimmed maximum while the direct path crosses progressively toward wet-only.
void reverb_mix_gains(const float mix, float& dry, float& wet) noexcept {
    const float clamped = std::clamp(mix, 0.0F, 1.0F);
    if (clamped <= 0.5F) {
        dry = 1.0F;
        wet = kReverbWetTrim * 4.0F * clamped * (1.0F - clamped);
    } else {
        dry = 2.0F * (1.0F - clamped);
        wet = kReverbWetTrim;
    }
}

// One-pole smoothing coefficient for a time constant in milliseconds.
float pole_from_milliseconds(const float milliseconds) noexcept {
    const float samples = std::max(milliseconds, 0.01F) * 0.001F * kSampleRate;
    return std::exp(-1.0F / samples);
}

//--------------------------------------------------------------------+
// Parameter tables
//--------------------------------------------------------------------+

using Info = coyopedal_fx_param_info_t;

// One table per block, each with exactly the controls its pedal uses, so the
// panel never shows a knob that does nothing.

constexpr Info kGateHard[] = {
    {"THRESH", COYOPEDAL_FX_UNIT_DECIBEL_TENTHS, -800, -200, 10, -600, -600},
    {"RELEASE", COYOPEDAL_FX_UNIT_MILLISECONDS, 20, 500, 10, 120, 120},
};

constexpr Info kCompStudio[] = {
    {"THRESH", COYOPEDAL_FX_UNIT_DECIBEL_TENTHS, -400, 0, 10, -180, -180},
    {"RATIO", COYOPEDAL_FX_UNIT_RATIO_TENTHS, 15, 200, 5, 40, 40},
    {"ATTACK", COYOPEDAL_FX_UNIT_MILLISECONDS, 1, 100, 1, 10, 10},
    {"RELEASE", COYOPEDAL_FX_UNIT_MILLISECONDS, 20, 1000, 10, 150, 150},
    {"MAKEUP", COYOPEDAL_FX_UNIT_DECIBEL_TENTHS, 0, 240, 5, 0, 0},
};

constexpr Info kDriveKlon[] = {
    {"DRIVE", COYOPEDAL_FX_UNIT_DECIBEL_TENTHS, 0, 400, 5, 200, 200},
    {"TONE", COYOPEDAL_FX_UNIT_HERTZ, 1500, 8000, 100, 4000, 4000},
    {"LEVEL", COYOPEDAL_FX_UNIT_DECIBEL_TENTHS, -200, 60, 5, 0, 0},
};

constexpr Info kVerbSpring[] = {
    {"OUTPUT", COYOPEDAL_FX_UNIT_OUTPUT_MODE, 0, 1, 1, 0, 0},
    {"MIX", COYOPEDAL_FX_UNIT_PERCENT, 0, 100, 1, 25, 25},
    {"DECAY", COYOPEDAL_FX_UNIT_MILLISECONDS, 200, 8000, 100, 1800, 1800},
    {"DAMPING", COYOPEDAL_FX_UNIT_PERCENT, 0, 100, 1, 50, 50},
    {"PREDELAY", COYOPEDAL_FX_UNIT_MILLISECONDS, 0, 120, 5, 20, 20},
};

// The rate is in tenths of a hertz: a chorus at its slowest is well under 1 Hz, which
// whole hertz cannot express.
constexpr Info kModChorus[] = {
    {"RATE", COYOPEDAL_FX_UNIT_HERTZ_TENTHS, 1, 100, 1, 8, 8},
    {"DEPTH", COYOPEDAL_FX_UNIT_PERCENT, 0, 100, 1, 40, 40},
    {"DELAY", COYOPEDAL_FX_UNIT_MILLISECONDS, 5, 35, 1, 16, 16},
    {"MIX", COYOPEDAL_FX_UNIT_PERCENT, 0, 100, 1, 45, 45},
};

// The delay's controls. TIME goes to two seconds, which the attached buffer has to be
// able to hold; the block clamps to what it actually has rather than pretending.
constexpr Info kDelayDigital[] = {
    {"TIME", COYOPEDAL_FX_UNIT_MILLISECONDS, 20, 2000, 10, 400, 400},
    {"FEEDBK", COYOPEDAL_FX_UNIT_PERCENT, 0, 95, 1, 35, 35},
    {"MIX", COYOPEDAL_FX_UNIT_PERCENT, 0, 100, 1, 30, 30},
};

struct BlockInfo {
    const char* name;
    const Info* params;
    uint8_t count;
};

// One pedal per block, each with its own control table.
constexpr BlockInfo kBlocks[COYOPEDAL_FX_BLOCK_COUNT] = {
    {"Hard Gate", kGateHard, static_cast<uint8_t>(std::size(kGateHard))},
    {"Studio VCA", kCompStudio, static_cast<uint8_t>(std::size(kCompStudio))},
    {"Klon", kDriveKlon, static_cast<uint8_t>(std::size(kDriveKlon))},
    {"Spring", kVerbSpring, static_cast<uint8_t>(std::size(kVerbSpring))},
    {"Chorus", kModChorus, static_cast<uint8_t>(std::size(kModChorus))},
    {"Digital", kDelayDigital, static_cast<uint8_t>(std::size(kDelayDigital))},
};

constexpr uint8_t kMaximumParams = 5;

//--------------------------------------------------------------------+
// Gate
//--------------------------------------------------------------------+

// Every member here is zero-initialised on purpose. A single non-zero default
// initialiser puts the whole Chain object in .data instead of .bss, which spends
// flash storing values that are zero. coyopedal_fx_init() sets the real values.
struct Gate {
    float open_threshold;
    float close_threshold;
    float release_pole;
    float attack_pole;
    int hold_samples;
    float envelope;
    float gain;
    int hold_remaining;
    bool open;

    // Fast enough to catch a pick attack without clipping its front edge.
    static constexpr float kAttackPole = 0.9F;

    void configure(const int16_t* const values) noexcept {
        attack_pole = kAttackPole;
        hold_samples = static_cast<int>(0.030F * kSampleRate);
        const float threshold_db = static_cast<float>(values[0]) * 0.1F;
        open_threshold = decibels_to_linear(threshold_db);
        // Six decibels of hysteresis: without it a note decaying through the
        // threshold makes the gate chatter open and shut.
        close_threshold = decibels_to_linear(threshold_db - 6.0F);
        release_pole = pole_from_milliseconds(static_cast<float>(values[1]));
    }

    void process(float* const samples, const std::size_t count) noexcept {
        // `samples` belongs to the audio pipeline and never aliases this state,
        // but the C ABI does not let the optimiser prove that once this method is
        // inlined through the global Chain. Keep the recursive state in registers
        // for the whole block and commit it once at the end.
        const float open_level = open_threshold;
        const float close_level = close_threshold;
        float envelope_state = envelope;
        float gain_state = gain;
        int hold = hold_remaining;
        bool is_open = open;
        for (std::size_t index = 0; index < count; ++index) {
            const float magnitude = std::fabs(samples[index]);
            envelope_state = magnitude > envelope_state
                                 ? magnitude + attack_pole * (envelope_state - magnitude)
                                 : magnitude + release_pole * (envelope_state - magnitude);

            if (is_open) {
                if (envelope_state < close_level) {
                    if (hold > 0) {
                        --hold;
                    } else {
                        is_open = false;
                    }
                } else {
                    hold = hold_samples;
                }
            } else if (envelope_state > open_level) {
                is_open = true;
                hold = hold_samples;
            }

            const float target = is_open ? 1.0F : 0.0F;
            gain_state = target + release_pole * (gain_state - target);
            samples[index] *= gain_state;
        }
        envelope = envelope_state;
        gain = gain_state;
        hold_remaining = hold;
        open = is_open;
    }
};

//--------------------------------------------------------------------+
// Compressor
//--------------------------------------------------------------------+

struct Compressor {
    float threshold_db;
    float slope; // 1 - 1/ratio
    float attack_pole;
    float release_pole;
    float makeup;
    float envelope;
    float gain;
    float gain_step;
    float reduction_db;
    int until_update;

    // The gain computer runs here rather than per sample: the envelope moves at
    // millisecond scale, so recomputing every 16 samples and sliding the gain
    // between updates is inaudible and turns the log/exp pair from a per-sample
    // cost into a sixteenth of one.
    static constexpr int kControlInterval = 16;
    static constexpr float kKneeDb = 6.0F;

    void configure(const int16_t* const values) noexcept {
        // The Studio VCA, with everything exposed.
        threshold_db = static_cast<float>(values[0]) * 0.1F;
        slope = 1.0F - (1.0F / std::max(static_cast<float>(values[1]) * 0.1F, 1.0F));
        attack_pole = pole_from_milliseconds(static_cast<float>(values[2]));
        release_pole = pole_from_milliseconds(static_cast<float>(values[3]));
        makeup = decibels_to_linear(static_cast<float>(values[4]) * 0.1F);
    }

    void process(float* const samples, const std::size_t count) noexcept {
        // Keep the recursive control state in registers for the whole block.
        // `samples` is the pipeline buffer and cannot alias the global Chain, but
        // that fact is lost at this C ABI boundary; updating members in the loop
        // otherwise forces repeated state loads/stores on LX7.
        const float attack = attack_pole;
        const float release = release_pole;
        const float output_makeup = makeup;
        const float threshold = threshold_db;
        const float compression_slope = slope;
        float envelope_state = envelope;
        float gain_state = gain;
        float gain_delta = gain_step;
        float reduction_state = reduction_db;
        int update_countdown = until_update;

        for (std::size_t index = 0; index < count; ++index) {
            const float magnitude = std::fabs(samples[index]);
            envelope_state = magnitude > envelope_state
                                 ? magnitude + attack * (envelope_state - magnitude)
                                 : magnitude + release * (envelope_state - magnitude);

            if (update_countdown <= 0) {
                const float level_db = linear_to_decibels(envelope_state);
                const float over = level_db - threshold;
                float reduction = 0.0F;
                if (over >= kKneeDb * 0.5F) {
                    reduction = -compression_slope * over;
                } else if (over > -kKneeDb * 0.5F) {
                    // Soft knee: quadratic through the corner so the onset of
                    // compression is not audible as a step.
                    const float knee = over + kKneeDb * 0.5F;
                    reduction = -compression_slope * knee * knee * (1.0F / (2.0F * kKneeDb));
                }
                reduction_state = reduction;
                const float target = decibels_to_linear(reduction);
                gain_delta = (target - gain_state) * (1.0F / static_cast<float>(kControlInterval));
                update_countdown = kControlInterval;
            }
            --update_countdown;

            gain_state += gain_delta;
            samples[index] *= gain_state * output_makeup;
        }

        envelope = envelope_state;
        gain = gain_state;
        gain_step = gain_delta;
        reduction_db = reduction_state;
        until_update = update_countdown;
    }
};

//--------------------------------------------------------------------+
// Overdrive
//--------------------------------------------------------------------+

struct Overdrive {
    float drive;
    float blend;
    float level;
    float tone_pole;
    float tone_state;
    float highpass_state;
    float previous_input;

    // Klon-style, from the circuit rather than from anyone's code.
    //
    // What makes a Centaur sound "transparent" is not its clipping - it is that
    // the clipped path is summed with a clean path, so the dry guitar stays
    // present underneath and the drive control moves the balance rather than
    // replacing one with the other. The clipping itself is a soft asymmetric pair,
    // germanium-ish: the two halves saturate at different points, which is what
    // puts even harmonics in.
    //
    // The shaper is evaluated through its antiderivative. A static nonlinearity at
    // 48 kHz throws harmonics past Nyquist that fold back as inharmonic grit; the
    // textbook fix is 4x oversampling, which means resampling filters and four
    // times the arithmetic. Averaging the shaper across the interval between two
    // samples gets most of the same benefit for one extra evaluation and a divide,
    // with no added latency.
    //
    // The curve is the cubic x - x^3/3: soft and gradual, the Centaur's. The
    // antialiasing needs its antiderivative as well as its transfer function, and
    // both are closed-form and cheap.
    // Index zero is positive, one is negative. Reading the IEEE-754 sign bit
    // selects the coefficient directly; a ternary would emit a float compare and
    // branch at every shaper evaluation on LX7.
    std::array<float, 2> knee;
    std::array<float, 2> knee_inverse;
    std::array<float, 2> cubic_inverse_square;
    std::array<float, 2> cubic_saturation;

    [[gnu::always_inline]] static inline std::size_t sign_index(const float x) noexcept {
        return std::bit_cast<std::uint32_t>(x) >> 31U;
    }

    float knee_for(const float x) const noexcept {
        return knee[sign_index(x)];
    }

    float knee_inverse_for(const float x) const noexcept {
        return knee_inverse[sign_index(x)];
    }

    float shape(const float x) const noexcept {
        const float knee = knee_for(x);
        const float u = std::clamp(x * knee_inverse_for(x), -1.0F, 1.0F);
        return knee * (u - (u * u * u) * (1.0F / 3.0F));
    }

    float antiderivative(const float x) const noexcept {
        const float knee = knee_for(x);
        const float raw = x * knee_inverse_for(x);
        const float square = knee * knee;
        const float u = std::clamp(raw, -1.0F, 1.0F);
        const bool saturated = (raw < -1.0F) || (raw > 1.0F);
        if (!saturated) {
            return square * (0.5F * u * u - (u * u * u * u) * (1.0F / 12.0F));
        }
        const float edge = square * (0.5F - 1.0F / 12.0F);
        return edge + (2.0F / 3.0F) * knee * (std::fabs(x) - knee);
    }

    float highpass_pole;
    [[gnu::always_inline]] static inline float
    divide_antiderivative(const float numerator, const float denominator) noexcept {
#if defined(__XTENSA__)
        // ESP32-S3 has no precise scalar divide instruction, and libgcc's
        // __divsf3 would cost more than the rest of the antiderivative shaper,
        // once for every audio sample. LX7 does have an initial reciprocal
        // estimate; two Newton steps recover single-precision accuracy without
        // leaving the realtime hot loop.
        float reciprocal;
        asm("recip0.s %0, %1" : "=f"(reciprocal) : "f"(denominator));
        reciprocal *= 2.0F - denominator * reciprocal;
        reciprocal *= 2.0F - denominator * reciprocal;
        return numerator * reciprocal;
#else
        return numerator / denominator;
#endif
    }

    [[gnu::always_inline]] inline float
    antialiased_cubic_shape(const float input, const float previous) const noexcept {
        const float difference = input - previous;

        // Inside one polynomial segment the divided difference has a closed
        // form. Test the signed intervals directly: a sign-bit array lookup plus
        // two fabs operations would cost extra loads and spills in the LX7
        // loop. The coefficient and saturated value are configuration-time
        // constants.
        float boundary = knee[0];
        if (input >= 0.0F && previous >= 0.0F) {
            if (input <= boundary && previous <= boundary) {
                const float sum = input + previous;
                return sum * (0.5F - (input * input + previous * previous) *
                                         cubic_inverse_square[0] * (1.0F / 12.0F));
            }
            if (input >= boundary && previous >= boundary) {
                return cubic_saturation[0];
            }
        } else if (input < 0.0F && previous < 0.0F) {
            boundary = -knee[1];
            if (input >= boundary && previous >= boundary) {
                const float sum = input + previous;
                return sum * (0.5F - (input * input + previous * previous) *
                                         cubic_inverse_square[1] * (1.0F / 12.0F));
            }
            if (input <= boundary && previous <= boundary) {
                return -cubic_saturation[1];
            }
        }

        // A knee/sign crossing is uncommon and spans different polynomial
        // pieces, so retain the general antiderivative quotient there.
        if (std::fabs(difference) < 1.0e-5F) {
            return shape(0.5F * (input + previous));
        }
        return divide_antiderivative(antiderivative(input) - antiderivative(previous), difference);
    }

    void configure(const int16_t* const values) noexcept {
        // The Klon, where the clean path is the point. Corner of the pre-clipper highpass, in
        // hertz.
        const float highpass_hz = 35.0F;
        knee = {1.0F, 0.7F};
        knee_inverse = {1.0F / knee[0], 1.0F / knee[1]};
        cubic_inverse_square = {
            knee_inverse[0] * knee_inverse[0],
            knee_inverse[1] * knee_inverse[1],
        };
        cubic_saturation = {
            (2.0F / 3.0F) * knee[0],
            (2.0F / 3.0F) * knee[1],
        };

        drive = decibels_to_linear(static_cast<float>(values[0]) * 0.1F);
        // Drive moves gain and balance together, as the original does - at the
        // bottom you hear the clean path, at the top the clipped one.
        blend = std::clamp(static_cast<float>(values[0]) / 400.0F, 0.0F, 1.0F);
        level = decibels_to_linear(static_cast<float>(values[2]) * 0.1F);
        const float tone_omega =
            2.0F * 3.14159265F * static_cast<float>(values[1]) * kInverseSampleRate;
        tone_pole = std::clamp(1.0F - std::exp(-tone_omega), 0.01F, 1.0F);
        const float hp_omega = 2.0F * 3.14159265F * highpass_hz * kInverseSampleRate;
        highpass_pole = std::clamp(1.0F - std::exp(-hp_omega), 0.0001F, 1.0F);
    }

#if defined(__XTENSA__)
    [[gnu::section(".iram1"), gnu::noinline]]
#endif
    void process(float* const samples, const std::size_t count) noexcept {
        const float highpass_coefficient = highpass_pole;
        const float drive_gain = drive;
        const float tone_coefficient = tone_pole;
        const float wet_gain = blend;
        const float dry_gain = 1.0F - wet_gain;
        const float output_gain = level;
        float highpass = highpass_state;
        float previous = previous_input;
        float tone = tone_state;

        // A gentle highpass ahead of the clipper, as the circuit has: clipping the
        // full low end turns chords to mud.
        for (std::size_t index = 0; index < count; ++index) {
            const float clean = samples[index];
            highpass += highpass_coefficient * (clean - highpass);
            const float input = (clean - highpass) * drive_gain;

            const float shaped = antialiased_cubic_shape(input, previous);
            previous = input;

            tone += tone_coefficient * (shaped - tone);
            samples[index] = (dry_gain * clean + wet_gain * tone) * output_gain;
        }

        highpass_state = highpass;
        previous_input = previous;
        tone_state = tone;
    }
};

//--------------------------------------------------------------------+
// Reverb
//--------------------------------------------------------------------+

// The reverb: a Dattorro-style pair of cross-coupled tanks, voiced as a spring -
// short, bright, dispersive loops with a tap-led splash.
//
// The tank runs at 24 kHz behind a small anti-aliasing FIR. Reverb has little
// useful guitar energy above that band's edge. Long lines use signed Q15 storage
// while all filters and gains remain float. The quantisation floor is below the
// pedal's 16-bit USB output, yet the short diffusers stay internal. The target may
// put the long, linearly advancing tank delays in PSRAM when its display needs the
// DMA-capable internal pool.
//
// The hot loops stay in .iram1. Moving them to flash frees DRAM on paper, since
// IRAM and DRAM share the same SRAM on the ESP32-S3, but a bypass or preset
// change then re-fetches the newly selected loops from flash while audio runs,
// which is audible and can hang the board.
struct CompactReverb {
    struct WetPair {
        float left;
        float right;
    };

    static constexpr std::size_t kLineCount = 8;
    static constexpr float kTankRate = kSampleRate * 0.5F;
    static constexpr float kQ15Scale = 1.0F / 32768.0F;
    static constexpr std::array<std::size_t, kLineCount> kMaximum = {
        569, 3671, 1481, 3041, 761, 3461, 2179, 2593,
    };
    static constexpr std::array<std::size_t, kLineCount> kLengths = {
        211, 1201, 443, 953, 281, 1429, 557, 1103,
    };
    // The spring voicing decays in this fraction of the time DECAY asks for.
    static constexpr float kDecayScale = 0.65F;
    static constexpr std::size_t kPredelay = 1024;
    static constexpr std::size_t kInputAllpassOne = 115;
    static constexpr std::size_t kInputAllpassTwo = 87;
    static constexpr std::size_t kInputAllpassThree = 307;
    static constexpr std::size_t kInputAllpassFour = 223;
    // Symmetric 15-tap Blackman-windowed decimator, 10.8 kHz passband edge.
    static constexpr std::array<float, 7> kDecimatorSide = {
        0.0F,           0.0008325984F, 0.0040727242F, -0.0110721575F,
        -0.0434200558F, 0.0351205277F, 0.2894172127F,
    };
    static constexpr float kDecimatorCentre = 0.4500983007F;

    std::array<int16_t*, kLineCount> lines{};
    // Target-owned, like the tank lines. One write and one read per sample at
    // strictly increasing addresses: of everything the reverb touches this is
    // the kindest to the PSRAM cache, so it is the first thing to give up
    // internal SRAM and the last thing worth moving back.
    float* predelay{};
    std::array<float, kInputAllpassOne> input_allpass_one{};
    std::array<float, kInputAllpassTwo> input_allpass_two{};
    std::array<float, kInputAllpassThree> input_allpass_three{};
    std::array<float, kInputAllpassFour> input_allpass_four{};
    // Mirror the 16-sample FIR history once. The active window is then
    // contiguous and every tap has a fixed negative offset from `newest`,
    // avoiding thirteen modulo/address sequences in the full-rate hot loop.
    std::array<float, 32> decimator{};
    std::array<std::size_t, kLineCount> write{};
    std::array<std::size_t, kLineCount> length{};
    // The tank's integer delays advance in lockstep with their writes. Keep
    // their read cursors moving too instead of rebuilding
    // write + maximum - delay, plus its wrap branch, for every tank tick.
    std::array<std::size_t, kLineCount> tank_delay_read{};
    // Tap positions and the two modulated base delays only change when a
    // preset is configured. Keeping them here avoids rebuilding the same four
    // integer ratios and two int-to-float values at every 24 kHz tank tick.
    std::array<std::size_t, 4> tank_tap_offset{};
    // Next sample to read from each output-tap line. Keeping the cursor in
    // read-before-increment form lets the block kernel normalize a wrap between
    // contiguous chunks instead of paying four wrap branches on a tank tick.
    std::array<std::size_t, 4> tank_tap_read{};
    float tank_modulated_delay_one{};
    float tank_modulated_delay_two{};
    std::size_t predelay_write{};
    std::size_t predelay_delay{1};
    std::size_t input_allpass_one_write{};
    std::size_t input_allpass_two_write{};
    std::size_t input_allpass_three_write{};
    std::size_t input_allpass_four_write{};
    std::size_t decimator_write{};
    float left_loop_feedback{};
    float right_loop_feedback{};
    float damping_coefficient{0.5F};
    float input_diffusion_one{0.75F};
    float input_diffusion_two{0.625F};
    float tank_diffusion_one{-0.7F};
    float tank_diffusion_two{0.5F};
    float modulation_depth_one{5.0F};
    float modulation_depth_two{7.0F};
    float modulation_rate_one{0.17F};
    float modulation_rate_two{0.23F};
    float tank_output_gain{0.35F};
    float tap_output_gain{0.22F};
    float early_output_gain{};
    float wet{};
    float dry{1.0F};
    float input_highpass{};
    float left_damping{};
    float right_damping{};
    float left_feedback{};
    float right_feedback{};
    WetPair previous_wet{};
    WetPair current_wet{};
    float modulation_phase_one{};
    float modulation_phase_two{0.37F};
    bool half_rate_phase{};
    bool stereo{true};

    static std::size_t delay_position(const std::size_t write_position, const std::size_t maximum,
                                      const std::size_t delay) noexcept {
        std::size_t position = write_position + maximum - delay;
        if (position >= maximum) {
            position -= maximum;
        }
        return position;
    }

    void reset_tank_read_positions() noexcept {
        for (std::size_t line = 0; line < kLineCount; ++line) {
            tank_delay_read[line] = delay_position(write[line], kMaximum[line], length[line]);
        }
        constexpr std::array<std::size_t, 4> kTapLines = {1U, 3U, 5U, 7U};
        for (std::size_t tap_index = 0; tap_index < kTapLines.size(); ++tap_index) {
            const std::size_t line = kTapLines[tap_index];
            std::size_t position = delay_position(
                write[line], kMaximum[line], std::max<std::size_t>(tank_tap_offset[tap_index], 1U));
            if (++position == kMaximum[line]) {
                position = 0U;
            }
            tank_tap_read[tap_index] = position;
        }
    }

    void clear() noexcept {
        for (std::size_t line = 0; line < kLineCount; ++line) {
            if (lines[line] != nullptr) {
                std::fill_n(lines[line], kMaximum[line], int16_t{0});
            }
        }
        if (predelay != nullptr) {
            std::fill_n(predelay, kPredelay, 0.0F);
        }
        input_allpass_one.fill(0.0F);
        input_allpass_two.fill(0.0F);
        input_allpass_three.fill(0.0F);
        input_allpass_four.fill(0.0F);
        decimator.fill(0.0F);
        write.fill(0U);
        reset_tank_read_positions();
        predelay_write = 0U;
        input_allpass_one_write = 0U;
        input_allpass_two_write = 0U;
        input_allpass_three_write = 0U;
        input_allpass_four_write = 0U;
        decimator_write = 0U;
        input_highpass = 0.0F;
        left_damping = 0.0F;
        right_damping = 0.0F;
        left_feedback = 0.0F;
        right_feedback = 0.0F;
        previous_wet = {};
        current_wet = {};
        modulation_phase_one = 0.0F;
        modulation_phase_two = 0.37F;
        half_rate_phase = false;
    }

    void configure(const int16_t* const values) noexcept {
        stereo = values[0] == 0;
        const float mix = static_cast<float>(values[1]) * 0.01F;
        reverb_mix_gains(mix, dry, wet);
        length = kLengths;
        tank_tap_offset = {
            length[1] / 3U,
            (length[3] * 2U) / 5U,
            length[5] / 4U,
            (length[7] * 3U) / 5U,
        };
        tank_modulated_delay_one = static_cast<float>(length[0]);
        tank_modulated_delay_two = static_cast<float>(length[4]);
        const float left_loop = static_cast<float>(length[0] + length[1] + length[2] + length[3]);
        const float right_loop = static_cast<float>(length[4] + length[5] + length[6] + length[7]);
        const float requested_decay_samples =
            std::max(static_cast<float>(values[2]), 1.0F) * 0.001F * kTankRate;
        const float damping = std::clamp(static_cast<float>(values[3]) * 0.01F, 0.0F, 0.98F);
        const float base_damping = 0.02F + 0.9F * (1.0F - damping);
        damping_coefficient = std::clamp(base_damping * 1.12F, 0.01F, 0.98F);
        input_diffusion_one = 0.82F;
        input_diffusion_two = 0.70F;
        tank_diffusion_one = -0.84F;
        tank_diffusion_two = 0.68F;
        modulation_depth_one = 1.75F;
        modulation_depth_two = 2.75F;
        modulation_rate_one = 1.37F;
        modulation_rate_two = 1.71F;
        tank_output_gain = 1.00F;
        tap_output_gain = 2.20F;
        early_output_gain = 0.65F;

        const float decay_samples = requested_decay_samples * kDecayScale;
        // The two tanks have different loop times. One gain for both would make
        // the shorter side decay too quickly and the longer side hang over it.
        // Loss follows the path that produced each feedback sample, preserving
        // the requested RT60 on both sides.
        left_loop_feedback =
            std::clamp(decibels_to_linear(-60.0F * left_loop / decay_samples), 0.05F, 0.985F);
        right_loop_feedback =
            std::clamp(decibels_to_linear(-60.0F * right_loop / decay_samples), 0.05F, 0.985F);
        predelay_delay = std::clamp<std::size_t>(static_cast<std::size_t>(values[4]) * 48U + 1U, 1U,
                                                 kPredelay - 1U);
        reset_tank_read_positions();
    }

    template <std::size_t Size>
    static float allpass(const float input, std::array<float, Size>& line, std::size_t& position,
                         const float amount) noexcept {
        const float delayed = line[position];
        const float output = delayed - amount * input;
        line[position] = input + amount * output;
        if (++position == line.size()) {
            position = 0U;
        }
        return output;
    }

    static float modulation_wave(const float phase) noexcept {
        const float centred = 2.0F * phase - 1.0F;
        const float parabola = 4.0F * centred * (1.0F - std::fabs(centred));
        return parabola * (0.775F + 0.225F * std::fabs(parabola));
    }

    static float from_q15(const int16_t value) noexcept {
        return static_cast<float>(value) * kQ15Scale;
    }

    void store_q15(const std::size_t line, const std::size_t position, const float value) noexcept {
        // The tank is rounded to Q15 at every delay write. Straight rounding
        // keeps the storage error below one Q15 LSB; error-feedback shaping would
        // add a dependent float load/add and an int-to-float reconstruction for
        // all eight lines on every tank tick.
        // ROUND.S plus integer saturation is the same finite-range Q15 mapping
        // without two float compares, two conditional moves and a sign branch.
        const std::int32_t rounded =
            std::clamp(realtime_round_to_int32(value * 32768.0F), static_cast<std::int32_t>(-32768),
                       static_cast<std::int32_t>(32767));
        const int16_t stored = static_cast<int16_t>(rounded);
        lines[line][position] = stored;
    }

    float read_delay(const std::size_t line, const float delay) const noexcept {
        const std::size_t whole = static_cast<std::size_t>(delay);
        std::size_t read = write[line] + kMaximum[line] - whole;
        if (read >= kMaximum[line]) {
            read -= kMaximum[line];
        }
        const float delayed = from_q15(lines[line][read]);
        const float fraction = delay - static_cast<float>(whole);
        // Both allpass delays are modulated continuously. The interpolation is
        // valid when fraction is exactly zero as well, so the hot loop does not
        // need a float compare and branch per read.
        const std::size_t older = (read == 0U) ? kMaximum[line] - 1U : read - 1U;
        return delayed + (from_q15(lines[line][older]) - delayed) * fraction;
    }

    template <bool NoWrap = false>
    float external_allpass(const float input, const std::size_t line, const float delay,
                           const float amount) noexcept {
        const float delayed = read_delay(line, delay);
        const float output = delayed - amount * input;
        store_q15(line, write[line], input + amount * output);
        ++write[line];
        if constexpr (!NoWrap) {
            if (write[line] == kMaximum[line]) {
                write[line] = 0U;
            }
        }
        return output;
    }

    template <bool NoWrap = false>
    float external_allpass_integer(const float input, const std::size_t line,
                                   const std::size_t delay, const float amount) noexcept {
        (void)delay;
        const float delayed = from_q15(lines[line][tank_delay_read[line]]);
        const float output = delayed - amount * input;
        store_q15(line, write[line], input + amount * output);
        ++write[line];
        ++tank_delay_read[line];
        if constexpr (!NoWrap) {
            if (write[line] == kMaximum[line]) {
                write[line] = 0U;
            }
            if (tank_delay_read[line] == kMaximum[line]) {
                tank_delay_read[line] = 0U;
            }
        }
        return output;
    }

    template <bool NoWrap = false>
    float external_delay(const float input, const std::size_t line) noexcept {
        const float delayed = from_q15(lines[line][tank_delay_read[line]]);
        store_q15(line, write[line], input);
        ++write[line];
        ++tank_delay_read[line];
        if constexpr (!NoWrap) {
            if (write[line] == kMaximum[line]) {
                write[line] = 0U;
            }
            if (tank_delay_read[line] == kMaximum[line]) {
                tank_delay_read[line] = 0U;
            }
        }
        return delayed;
    }

    template <bool NoWrap = false>
    float tap(const std::size_t line, const std::size_t tap_index) noexcept {
        std::size_t& position = tank_tap_read[tap_index];
        const float delayed = from_q15(lines[line][position]);
        ++position;
        if constexpr (!NoWrap) {
            if (position == kMaximum[line]) {
                position = 0U;
            }
        }
        return delayed;
    }

    const float* push_decimator(const float input) noexcept {
        decimator[decimator_write] = input;
        decimator[decimator_write + 16U] = input;
        const float* const newest = decimator.data() + decimator_write + 16U;
        decimator_write = (decimator_write + 1U) & 15U;
        return newest;
    }

    float decimator_output(const float* const newest) const noexcept {
        float output = kDecimatorCentre * newest[-7];
        for (std::size_t side = 1U; side < kDecimatorSide.size(); ++side) {
            output += kDecimatorSide[side] * (newest[-static_cast<std::ptrdiff_t>(side)] +
                                              newest[-static_cast<std::ptrdiff_t>(14U - side)]);
        }
        return output;
    }

    template <bool NoWrap = false>
    [[gnu::always_inline]] inline WetPair process_tank_tick_with_lfo(const float excitation,
                                                                     const float lfo_one,
                                                                     const float lfo_two) noexcept {
        float left = 0.35F * excitation + right_loop_feedback * right_feedback;
        float right = 0.35F * excitation + left_loop_feedback * left_feedback;

        left = external_allpass<NoWrap>(left, 0U,
                                        tank_modulated_delay_one + modulation_depth_one * lfo_one,
                                        tank_diffusion_one);
        left = external_delay<NoWrap>(left, 1U);
        left_damping += damping_coefficient * (left - left_damping);
        left = external_allpass_integer<NoWrap>(left_damping, 2U, length[2], tank_diffusion_two);
        left = external_delay<NoWrap>(left, 3U);

        right = external_allpass<NoWrap>(right, 4U,
                                         tank_modulated_delay_two - modulation_depth_two * lfo_two,
                                         tank_diffusion_one);
        right = external_delay<NoWrap>(right, 5U);
        right_damping += damping_coefficient * (right - right_damping);
        right = external_allpass_integer<NoWrap>(right_damping, 6U, length[6], tank_diffusion_two);
        right = external_delay<NoWrap>(right, 7U);

        left_feedback = left;
        right_feedback = right;

        // Read several points from both tanks. These taps arrive at unrelated
        // times and signs, hiding the two physical loop periods in the mono sum.
        const float tap_one = tap<NoWrap>(1U, 0U);
        const float tap_two = tap<NoWrap>(3U, 1U);
        const float tap_three = tap<NoWrap>(5U, 2U);
        const float tap_four = tap<NoWrap>(7U, 3U);
        return {
            tank_output_gain * (left + 0.18F * right) +
                tap_output_gain * (tap_one - tap_two + tap_three - tap_four) +
                early_output_gain * excitation,
            tank_output_gain * (right - 0.18F * left) +
                tap_output_gain * (tap_two + tap_three - tap_one - tap_four) -
                early_output_gain * excitation,
        };
    }

    template <bool NoWrap = false>
    [[gnu::always_inline]] inline WetPair process_tank_tick(const float excitation) noexcept {
        constexpr float kInverseTankRate = 1.0F / kTankRate;
        const float lfo_one = modulation_wave(modulation_phase_one);
        const float lfo_two = modulation_wave(modulation_phase_two);
        // Writing these as multiplies is intentional. Under strict IEEE flags,
        // a source-level division by the constant otherwise becomes __divsf3 on
        // LX7, twice for every 24 kHz tank tick.
        modulation_phase_one += modulation_rate_one * kInverseTankRate;
        modulation_phase_two += modulation_rate_two * kInverseTankRate;
        if (modulation_phase_one >= 1.0F) {
            modulation_phase_one -= 1.0F;
        }
        if (modulation_phase_two >= 1.0F) {
            modulation_phase_two -= 1.0F;
        }
        return process_tank_tick_with_lfo<NoWrap>(excitation, lfo_one, lfo_two);
    }

#if defined(__XTENSA__)
    [[gnu::section(".iram1"), gnu::noinline]]
#endif
    WetPair process_tank(const float excitation) noexcept {
        return process_tank_tick<false>(excitation);
    }

    [[gnu::always_inline]] inline void process_samples(float* const samples,
                                                       float* const right_samples,
                                                       const std::size_t count) noexcept {
        for (std::size_t index = 0; index < count; ++index) {
            const float input = samples[index];
            predelay[predelay_write] = input;
            const std::size_t read = (predelay_write + kPredelay - predelay_delay) % kPredelay;
            const float predelayed = predelay[read];
            if (++predelay_write == kPredelay) {
                predelay_write = 0U;
            }
            const float* const newest = push_decimator(predelayed);
            if (half_rate_phase) {
                // The history advances at 48 kHz, but only this phase enters
                // the 24 kHz tank. Computing the FIR on the discarded phase
                // would double the anti-aliasing arithmetic for no output.
                float excitation = decimator_output(newest);
                input_highpass += 0.02F * (excitation - input_highpass);
                excitation -= input_highpass;
                excitation = allpass(excitation, input_allpass_one, input_allpass_one_write,
                                     input_diffusion_one);
                excitation = allpass(excitation, input_allpass_two, input_allpass_two_write,
                                     input_diffusion_one);
                excitation = allpass(excitation, input_allpass_three, input_allpass_three_write,
                                     input_diffusion_two);
                excitation = allpass(excitation, input_allpass_four, input_allpass_four_write,
                                     input_diffusion_two);
                previous_wet = current_wet;
                current_wet = process_tank(excitation);
            }
            const WetPair tail = half_rate_phase
                                     ? WetPair{0.5F * (previous_wet.left + current_wet.left),
                                               0.5F * (previous_wet.right + current_wet.right)}
                                     : current_wet;
            half_rate_phase = !half_rate_phase;
            if (right_samples == nullptr) {
                samples[index] = dry * input + wet * 0.5F * (tail.left + tail.right);
            } else if (stereo) {
                samples[index] = dry * input + wet * tail.left;
                right_samples[index] = dry * input + wet * tail.right;
            } else {
                // Equal-power fold-down: decorrelated tank projections keep their
                // total wet energy when centered rather than becoming 3 dB quieter.
                const float mono_wet = 0.7071067811865475F * (tail.left + tail.right);
                const float output = dry * input + wet * mono_wet;
                samples[index] = output;
                right_samples[index] = output;
            }
        }
    }

#if defined(__XTENSA__)
    // Keep every specialized hot loop in ordinary IRAM; RTC FAST instruction
    // fetch measured roughly five times slower on ESP32-S3.
    template <bool NoWrap>
    [[gnu::always_inline]] inline void
    process_tank_output_ticks(float* const samples, float* const right_samples,
                              const float* const excitations, const std::size_t first_tick,
                              const std::size_t tick_count, WetPair& running, WetPair& prior,
                              float& lfo_one, float& lfo_two, const float lfo_one_step,
                              const float lfo_two_step) noexcept {
        const float wet_gain = wet;
        const std::size_t end_tick = first_tick + tick_count;
        for (std::size_t tank_tick = first_tick; tank_tick < end_tick; ++tank_tick) {
            // Read before writing this pair: from the halfway point onward the
            // destination overlaps an excitation consumed by an earlier tick.
            const float excitation = excitations[tank_tick];
            prior = running;
            const WetPair next = process_tank_tick_with_lfo<NoWrap>(excitation, lfo_one, lfo_two);
            const WetPair midpoint = {
                0.5F * (prior.left + next.left),
                0.5F * (prior.right + next.right),
            };
            const std::size_t first_index = tank_tick << 1U;
            const std::size_t second_index = first_index + 1U;
            const float first_dry = samples[first_index];
            const float second_dry = samples[second_index];
            samples[first_index] = first_dry + wet_gain * prior.left;
            right_samples[first_index] = first_dry + wet_gain * prior.right;
            samples[second_index] = second_dry + wet_gain * midpoint.left;
            right_samples[second_index] = second_dry + wet_gain * midpoint.right;
            running = next;
            lfo_one += lfo_one_step;
            lfo_two += lfo_two_step;
        }
    }

    [[gnu::always_inline]] inline void
    process_tank_output_block(float* const samples, float* const right_samples,
                              const float* const excitations) noexcept {
        constexpr std::size_t kFrames = COYOPEDAL_PEDAL_BLOCK_FRAMES;
        constexpr std::size_t kTicks = kFrames / 2U;
        WetPair running = current_wet;
        WetPair prior = previous_wet;
        constexpr float kInverseTankRate = 1.0F / kTankRate;
        const float phase_one_step = modulation_rate_one * kInverseTankRate;
        const float phase_two_step = modulation_rate_two * kInverseTankRate;
        const float lfo_one_start = modulation_wave(modulation_phase_one);
        const float lfo_two_start = modulation_wave(modulation_phase_two);
        float phase_one_end = modulation_phase_one + phase_one_step * static_cast<float>(kTicks);
        float phase_two_end = modulation_phase_two + phase_two_step * static_cast<float>(kTicks);
        if (phase_one_end >= 1.0F) {
            phase_one_end -= 1.0F;
        }
        if (phase_two_end >= 1.0F) {
            phase_two_end -= 1.0F;
        }
        const float lfo_one_step =
            (modulation_wave(phase_one_end) - lfo_one_start) / static_cast<float>(kTicks);
        const float lfo_two_step =
            (modulation_wave(phase_two_end) - lfo_two_start) / static_cast<float>(kTicks);
        float lfo_one = lfo_one_start;
        float lfo_two = lfo_two_start;

        // The dry path is independent of the recursive tank. Do these multiplies
        // in a compact pass so dry gain and the unscaled input do not stay live
        // through the much larger tank loop and force extra register spills.
        const float dry_gain = dry;
        for (std::size_t index = 0; index < kFrames; ++index) {
            samples[index] *= dry_gain;
        }

        // All tank cursors advance by one per 24 kHz tick. Split a block at
        // ring boundaries instead of running the entire 32-tick block through
        // the checked fallback. Cursors hold the next address to read or write,
        // so every wrap can be normalized between chunks without changing any
        // sample arithmetic.
        std::size_t processed = 0U;
        while (processed < kTicks) {
            std::size_t safe = kTicks - processed;
            for (std::size_t line = 0; line < kLineCount; ++line) {
                safe = std::min(safe, kMaximum[line] - write[line]);
            }
            constexpr std::array<std::size_t, 6> kIntegerReadLines = {
                1U, 2U, 3U, 5U, 6U, 7U,
            };
            for (const std::size_t line : kIntegerReadLines) {
                safe = std::min(safe, kMaximum[line] - tank_delay_read[line]);
            }
            constexpr std::array<std::size_t, 4> kTapLines = {1U, 3U, 5U, 7U};
            for (std::size_t tap_index = 0; tap_index < kTapLines.size(); ++tap_index) {
                const std::size_t line = kTapLines[tap_index];
                safe = std::min(safe, kMaximum[line] - tank_tap_read[tap_index]);
            }

            process_tank_output_ticks<true>(samples, right_samples, excitations, processed, safe,
                                            running, prior, lfo_one, lfo_two, lfo_one_step,
                                            lfo_two_step);
            processed += safe;
            for (std::size_t line = 0; line < kLineCount; ++line) {
                if (write[line] == kMaximum[line]) {
                    write[line] = 0U;
                }
            }
            for (const std::size_t line : kIntegerReadLines) {
                if (tank_delay_read[line] == kMaximum[line]) {
                    tank_delay_read[line] = 0U;
                }
            }
            for (std::size_t tap_index = 0; tap_index < kTapLines.size(); ++tap_index) {
                const std::size_t line = kTapLines[tap_index];
                if (tank_tap_read[tap_index] == kMaximum[line]) {
                    tank_tap_read[tap_index] = 0U;
                }
            }
        }
        modulation_phase_one = phase_one_end;
        modulation_phase_two = phase_two_end;
        previous_wet = prior;
        current_wet = running;
    }

    [[gnu::section(".iram1"), gnu::noinline]] void
    process_tank_stereo_block(float* const __restrict samples,
                              float* const __restrict right_samples, const std::size_t) noexcept {
        // The 48 kHz diffuser and the 24 kHz tank only share the excitation
        // sequence. Build that sequence first, then run the recursive tank as
        // one tight loop. This gives the compiler a chance to retain tank state
        // across all 32 ticks instead of spilling it around every other sample.
        // The upper half of the expendable right-input block is safe scratch:
        // output pair i can only overwrite excitations that were already read.
        constexpr std::size_t kFrames = COYOPEDAL_PEDAL_BLOCK_FRAMES;
        constexpr std::size_t kTicks = kFrames / 2U;
        static_assert(kFrames >= 2U && (kFrames & 1U) == 0U);
#if defined(__XTENSA__)
        const bool collect_profile = reverb_profile_enabled;
        const std::uint32_t profile_start = collect_profile ? read_cycle_count() : 0U;
#endif
        float* const excitations = right_samples + kTicks;

        std::size_t predelay_position = predelay_write;
        std::size_t decimator_position = decimator_write;
        std::size_t diffuser_one_position = input_allpass_one_write;
        std::size_t diffuser_two_position = input_allpass_two_write;
        std::size_t diffuser_three_position = input_allpass_three_write;
        std::size_t diffuser_four_position = input_allpass_four_write;
        float highpass = input_highpass;
        const std::size_t predelay_offset = predelay_delay;
        const float diffusion_one = input_diffusion_one;
        const float diffusion_two = input_diffusion_two;

        const auto push_input = [&](const float input) {
            predelay[predelay_position] = input;
            const std::size_t read =
                (predelay_position + kPredelay - predelay_offset) & (kPredelay - 1U);
            const float predelayed = predelay[read];
            predelay_position = (predelay_position + 1U) & (kPredelay - 1U);
            decimator[decimator_position] = predelayed;
            decimator[decimator_position + 16U] = predelayed;
            const float* const newest = decimator.data() + decimator_position + 16U;
            decimator_position = (decimator_position + 1U) & 15U;
            return newest;
        };
        const auto run_allpass = [](const float input, auto& line, std::size_t& position,
                                    const float amount) {
            const float delayed = line[position];
            const float output = delayed - amount * input;
            line[position] = input + amount * output;
            if (++position == line.size()) {
                position = 0U;
            }
            return output;
        };

        for (std::size_t tank_tick = 0; tank_tick < kTicks; ++tank_tick) {
            const std::size_t first_index = tank_tick << 1U;
            (void)push_input(samples[first_index]);
            const float* const newest = push_input(samples[first_index + 1U]);
            float excitation = decimator_output(newest);
            highpass += 0.02F * (excitation - highpass);
            excitation -= highpass;
            excitation =
                run_allpass(excitation, input_allpass_one, diffuser_one_position, diffusion_one);
            excitation =
                run_allpass(excitation, input_allpass_two, diffuser_two_position, diffusion_one);
            // The tank uses all four input diffusers.
            excitation = run_allpass(excitation, input_allpass_three, diffuser_three_position,
                                     diffusion_two);
            excitation =
                run_allpass(excitation, input_allpass_four, diffuser_four_position, diffusion_two);
            excitations[tank_tick] = excitation;
        }
        predelay_write = predelay_position;
        decimator_write = decimator_position;
        input_allpass_one_write = diffuser_one_position;
        input_allpass_two_write = diffuser_two_position;
        input_allpass_three_write = diffuser_three_position;
        input_allpass_four_write = diffuser_four_position;
        input_highpass = highpass;

#if defined(__XTENSA__)
        const std::uint32_t profile_tank_start = collect_profile ? read_cycle_count() : 0U;
#endif
        process_tank_output_block(samples, right_samples, excitations);
#if defined(__XTENSA__)
        if (collect_profile) {
            const std::uint32_t profile_end = read_cycle_count();
            reverb_profile_input_cycles += profile_tank_start - profile_start;
            reverb_profile_tank_cycles += profile_end - profile_tank_start;
            ++reverb_profile_blocks;
        }
#endif
        half_rate_phase = false;
    }

    [[gnu::section(".iram1"), gnu::noinline]] void
    process_tank_mode(float* const samples, float* const right_samples,
                      const std::size_t count) noexcept {
        if (right_samples != nullptr && stereo && !half_rate_phase &&
            count == COYOPEDAL_PEDAL_BLOCK_FRAMES) {
            process_tank_stereo_block(samples, right_samples, count);
        } else {
            process_samples(samples, right_samples, count);
        }
    }
#endif

    void process_impl(float* const samples, float* const right_samples,
                      const std::size_t count) noexcept {
        // The two-tank topology, whose configured coefficients and lengths carry
        // the sound.
#if defined(__XTENSA__)
        process_tank_mode(samples, right_samples, count);
#else
        process_samples(samples, right_samples, count);
#endif
    }

    void process_stereo(float* const left_samples, float* const right_samples,
                        const std::size_t count) noexcept {
        process_impl(left_samples, right_samples, count);
    }
};

//--------------------------------------------------------------------+
// Modulation
//--------------------------------------------------------------------+

// The chorus runs on one interpolating delay line: two reads and one write per
// sample. Targets with PSRAM may place the two 16 KB lines there while leaving the
// small, frequently touched control state internal.
//
// The size is set by the longest chorus *plus its sweep*, not by the delay control's
// maximum: 35 ms of delay sweeping +-40% peaks at 49 ms, and a line sized for 35 would
// clamp there - the sweep would flatten at one end and the chorus would go still at
// exactly the setting asked to be deepest.
constexpr std::size_t kModulationSamples = 4096; // 85 ms at 48 kHz

#if defined(ESP_PLATFORM)
[[gnu::section(".ext_ram.bss")]]
#endif
float external_modulation_line[kModulationSamples];

// The LFO is a phase accumulator with a parabolic sine rather than std::sin or a
// resonator. A resonator drifts in amplitude and cannot be retuned without a click;
// libm's sine costs more per sample than the chorus it steers. This is accurate to
// about 0.2% after the refinement step, which is far below what a modulation depth
// control can resolve.
float parabolic_sine(const float phase) noexcept {
    const float centred = 2.0F * phase - 1.0F;
    const float parabola = 4.0F * centred * (1.0F - std::fabs(centred));
    return parabola * (0.775F + 0.225F * std::fabs(parabola));
}

// 0.001F is not 1/1000 in binary, so multiplying by it turns 4 ms into 192.0000072
// samples, and the interpolator blends two samples to represent the extra seven
// millionths. Dividing instead is exact for every delay here: 4 * 48000 is 192000,
// and 192000/1000 is 192 with no rounding at all. It matters because a delay that is
// a whole number of samples should be bit-exact, and a test checks that it is.
float samples_from_milliseconds(const float milliseconds) noexcept {
    return milliseconds * kSampleRate / 1000.0F;
}

struct Modulation {
    // Every member zero-initialised, for the .data/.bss reason above.
    float* line;

    std::size_t write;
    float phase;
    float phase_step;
    float depth;
    float wet;
    float dry;
    float centre; // samples
    float sweep;  // samples, peak deviation from centre

    void clear() noexcept {
        std::fill(line, line + kModulationSamples, 0.0F);
        write = 0;
        phase = 0.0F;
    }

    // Linear interpolation. A chorus sweeps its read pointer continuously, so the
    // fractional part is where the pitch modulation actually lives; truncating it
    // would quantise the sweep into steps and buzz.
    [[gnu::always_inline]] static inline float read_unchecked_at(const float* const delay_line,
                                                                 const std::size_t write_position,
                                                                 const float delay) noexcept {
        const std::size_t whole = static_cast<std::size_t>(delay);
        const float fraction = delay - static_cast<float>(whole);
        static_assert((kModulationSamples & (kModulationSamples - 1U)) == 0U);
        constexpr std::size_t kLineMask = kModulationSamples - 1U;
        const std::size_t first = (write_position + kModulationSamples - whole) & kLineMask;
        const std::size_t second = (first - 1U) & kLineMask;
        return delay_line[first] + fraction * (delay_line[second] - delay_line[first]);
    }

    void configure(const int16_t* const values) noexcept {
        const float rate_hz = static_cast<float>(values[0]) * 0.1F;
        phase_step = rate_hz * kInverseSampleRate;
        depth = std::clamp(static_cast<float>(values[1]) * 0.01F, 0.0F, 1.0F);
        centre = samples_from_milliseconds(static_cast<float>(values[2]));
        // Deviation scales with the delay, which is what keeps a long, slow
        // chorus from sounding detuned and a short one from sounding static.
        sweep = depth * centre * 0.4F;
        const float mix = std::clamp(static_cast<float>(values[3]) * 0.01F, 0.0F, 1.0F);
        wet = mix;
        dry = 1.0F - mix;
    }

#if defined(__XTENSA__)
    [[gnu::section(".iram1")]]
#endif
    void process(float* const samples, const std::size_t count) noexcept {
        if (count == 0U) {
            return;
        }
        float* const delay_line = line;
        constexpr std::size_t line_mask = kModulationSamples - 1U;
        std::size_t write_position = write;
        const float dry_gain = dry;
        const float wet_gain = wet;
        const auto wrapped_phase = [](float value) noexcept {
            if (value >= 1.0F) {
                value -= 1.0F;
            }
            return value;
        };
        // Even at the fastest setting the LFO advances only a small fraction of
        // one cycle in 64 audio frames. Evaluate its deliberately cheap curve at
        // the block endpoints and advance a linear ramp in the sample loop. Delay
        // reads remain sample-accurate; only the sub-control-rate curve evaluation
        // is interpolated.
        const float first_phase = wrapped_phase(phase + phase_step);
        const float final_phase = wrapped_phase(phase + phase_step * static_cast<float>(count));
        const float lfo = parabolic_sine(first_phase);
        float lfo_step = 0.0F;
        float second_lfo = 0.0F;
        float second_lfo_step = 0.0F;
        if (count > 1U) {
            const float inverse_intervals = realtime_reciprocal(static_cast<float>(count - 1U));
            lfo_step = (parabolic_sine(final_phase) - lfo) * inverse_intervals;
            // The second voice runs a third of a cycle behind the first.
            const float first_second = wrapped_phase(first_phase + 0.333F);
            const float final_second = wrapped_phase(final_phase + 0.333F);
            second_lfo = parabolic_sine(first_second);
            second_lfo_step = (parabolic_sine(final_second) - second_lfo) * inverse_intervals;
        } else {
            second_lfo = parabolic_sine(wrapped_phase(first_phase + 0.333F));
        }
        phase = final_phase;

        float delay_one = centre + sweep * lfo;
        const float delay_one_step = sweep * lfo_step;
        float delay_two = centre + sweep * second_lfo;
        const float delay_two_step = sweep * second_lfo_step;

        // At guitar-chorus rates each read delay normally stays inside one
        // integer-sample interval for the entire block. In that case the
        // two fractional read windows and the write window all advance by
        // one together. Walk them as straight pointers and avoid repeating
        // float/integer conversions and four ring masks per sample.
        const std::size_t whole_one = static_cast<std::size_t>(delay_one);
        const std::size_t whole_two = static_cast<std::size_t>(delay_two);
        const float fraction_one = delay_one - static_cast<float>(whole_one);
        const float fraction_two = delay_two - static_cast<float>(whole_two);
        const float intervals = static_cast<float>(count - 1U);
        // Cover accumulated float rounding as well as the linear ramp. A
        // boundary-near block simply uses the exact general path below.
        constexpr float kBoundaryGuard = 0.02F;
        const float travel_one = std::fabs(delay_one_step) * intervals + kBoundaryGuard;
        const float travel_two = std::fabs(delay_two_step) * intervals + kBoundaryGuard;
        const bool whole_one_stable =
            delay_one_step >= 0.0F ? fraction_one + travel_one < 1.0F : fraction_one > travel_one;
        const bool whole_two_stable =
            delay_two_step >= 0.0F ? fraction_two + travel_two < 1.0F : fraction_two > travel_two;

        const std::size_t first_one = (write_position + kModulationSamples - whole_one) & line_mask;
        const std::size_t second_one = (first_one - 1U) & line_mask;
        const std::size_t first_two = (write_position + kModulationSamples - whole_two) & line_mask;
        const std::size_t second_two = (first_two - 1U) & line_mask;
        const bool contiguous =
            count <= kModulationSamples - write_position &&
            count <= kModulationSamples - first_one && count <= kModulationSamples - second_one &&
            count <= kModulationSamples - first_two && count <= kModulationSamples - second_two;

        if (whole_one_stable && whole_two_stable && contiguous) {
            const float* first_one_pointer = delay_line + first_one;
            const float* second_one_pointer = delay_line + second_one;
            const float* first_two_pointer = delay_line + first_two;
            const float* second_two_pointer = delay_line + second_two;
            float* write_pointer = delay_line + write_position;
            for (std::size_t index = 0; index < count; ++index) {
                const float input = samples[index];
                const float first = first_one_pointer[index] +
                                    (delay_one - static_cast<float>(whole_one)) *
                                        (second_one_pointer[index] - first_one_pointer[index]);
                const float other = first_two_pointer[index] +
                                    (delay_two - static_cast<float>(whole_two)) *
                                        (second_two_pointer[index] - first_two_pointer[index]);
                write_pointer[index] = input;
                samples[index] = dry_gain * input + wet_gain * 0.5F * (first + other);
                delay_one += delay_one_step;
                delay_two += delay_two_step;
            }
            write = write_position + count;
            if (write == kModulationSamples) {
                write = 0U;
            }
            return;
        }

        for (std::size_t index = 0; index < count; ++index) {
            const float input = samples[index];
            const float first = read_unchecked_at(delay_line, write_position, delay_one);
            const float other = read_unchecked_at(delay_line, write_position, delay_two);
            delay_line[write_position] = input;
            write_position = (write_position + 1U) & line_mask;
            samples[index] = dry_gain * input + wet_gain * 0.5F * (first + other);
            delay_one += delay_one_step;
            delay_two += delay_two_step;
        }
        write = write_position;
    }
};

//--------------------------------------------------------------------+
// Delay
//--------------------------------------------------------------------+

// The only block that does not own its memory: two seconds of mono is 384 KB, and the
// buffer it is given lives in PSRAM. One sequential write and one read per sample,
// which is what that memory is good at.
struct Delay {
    float* line;
    std::size_t capacity;
    std::size_t write;

    float time_samples;
    float target_samples;
    float feedback;
    float wet;
    float dry;

    bool attach(float* const memory, const std::size_t samples) noexcept {
        // A quarter of a second is the floor. Below that the TIME control could not
        // reach its own minimum and the block would silently be something else.
        if (memory == nullptr || samples < static_cast<std::size_t>(kSampleRate) / 4U) {
            line = nullptr;
            capacity = 0;
            return false;
        }
        line = memory;
        capacity = samples;
        clear();
        return true;
    }

    bool ready() const noexcept {
        return line != nullptr && capacity > 0U;
    }

    void clear() noexcept {
        if (!ready()) {
            return;
        }
        for (std::size_t index = 0; index < capacity; ++index) {
            line[index] = 0.0F;
        }
        write = 0;
        time_samples = 0.0F;
    }

    float read(const float delay) const noexcept {
        const float clamped = std::clamp(delay, 1.0F, static_cast<float>(capacity - 2U));
        const std::size_t whole = static_cast<std::size_t>(clamped);
        const float fraction = clamped - static_cast<float>(whole);
        std::size_t first = write + capacity - whole;
        if (first >= capacity) {
            first -= capacity;
        }
        const std::size_t second = (first == 0U) ? capacity - 1U : first - 1U;
        return line[first] + fraction * (line[second] - line[first]);
    }

    void configure(const int16_t* const values) noexcept {
        if (!ready()) {
            return;
        }
        // Clamped to the buffer rather than to the control's range: the same firmware
        // runs with whatever memory it was given, and a TIME longer than the line would
        // otherwise read the write pointer's own future.
        const float requested = samples_from_milliseconds(static_cast<float>(values[0]));
        target_samples = std::min(requested, static_cast<float>(capacity - 4U));
        if (time_samples <= 0.0F) {
            time_samples = target_samples;
        }
        feedback = std::clamp(static_cast<float>(values[1]) * 0.01F, 0.0F, 0.95F);
        const float mix = std::clamp(static_cast<float>(values[2]) * 0.01F, 0.0F, 1.0F);
        wet = mix;
        dry = 1.0F - mix;
    }

#if defined(__XTENSA__)
    [[gnu::section(".iram1")]]
#endif
    void process(float* const samples, const std::size_t count) noexcept {
        if (!ready()) {
            return;
        }

        // Millisecond delay settings map to exact integer samples at 48 kHz.
        // Once the click-free glide has settled, the delay needs one line read,
        // not a clamp, float conversion, fractional neighbour read and lerp
        // for a fraction that is exactly zero.
        if (std::fabs(target_samples - time_samples) < 1.0e-4F) {
            time_samples = target_samples;
            const std::size_t delay = static_cast<std::size_t>(target_samples);
            float* const delay_line = line;
            const std::size_t line_capacity = capacity;
            std::size_t write_position = write;
            std::size_t read_position = write_position + line_capacity - delay;
            if (read_position >= line_capacity) {
                read_position -= line_capacity;
            }
            const float feedback_gain = feedback;
            const float dry_gain = dry;
            const float wet_gain = wet;

            // A delay line wraps only once every tens of thousands of
            // samples. Keep the ordinary 64-sample block on straight
            // pointers so the hot loop does not pay two ring-boundary
            // tests per sample. The boundary block uses the general loop
            // below and produces the same sequence of reads and writes.
            if (count <= line_capacity - read_position && count <= line_capacity - write_position) {
                const float* read_pointer = delay_line + read_position;
                float* write_pointer = delay_line + write_position;
                for (std::size_t index = 0; index < count; ++index) {
                    const float input = samples[index];
                    const float tail = read_pointer[index];
                    write_pointer[index] = input + feedback_gain * tail;
                    samples[index] = dry_gain * input + wet_gain * tail;
                }
                write = write_position + count;
                if (write == line_capacity) {
                    write = 0U;
                }
                return;
            }

            for (std::size_t index = 0; index < count; ++index) {
                const float input = samples[index];
                const float tail = delay_line[read_position];
                delay_line[write_position] = input + feedback_gain * tail;
                if (++read_position == line_capacity) {
                    read_position = 0U;
                }
                if (++write_position == line_capacity) {
                    write_position = 0U;
                }
                samples[index] = dry_gain * input + wet_gain * tail;
            }
            write = write_position;
            return;
        }

        for (std::size_t index = 0; index < count; ++index) {
            const float input = samples[index];

            // TIME glides rather than jumps. A delay line whose length changes in one
            // sample steps over its own contents, which is a click on every turn of the
            // knob - and the knob is turned while playing.
            time_samples += 0.0004F * (target_samples - time_samples);

            const float tail = read(time_samples);
            line[write] = input + feedback * tail;
            ++write;
            if (write == capacity) {
                write = 0;
            }
            samples[index] = dry * input + wet * tail;
        }
    }
};

//--------------------------------------------------------------------+
// State
//--------------------------------------------------------------------+

struct Chain {
    std::array<std::array<int16_t, kMaximumParams>, COYOPEDAL_FX_BLOCK_COUNT> values{};
    std::array<bool, COYOPEDAL_FX_BLOCK_COUNT> enabled{};
    Gate gate;
    Compressor compressor;
    Overdrive overdrive;
    Modulation modulation;
    Delay delay;
};

// The target allocates the reverb's hot float core internally and attaches its
// sequential Q15 tank lines from PSRAM; it owns that placement.
CompactReverb* compact_reverb = nullptr;

CompactReverb& reverb_state() noexcept {
    return *compact_reverb;
}

Chain chain;

void apply(const coyopedal_fx_block_t block) noexcept {
    const int16_t* const values = chain.values[block].data();
    switch (block) {
    case COYOPEDAL_FX_GATE:
        chain.gate.configure(values);
        break;
    case COYOPEDAL_FX_COMPRESSOR:
        chain.compressor.configure(values);
        break;
    case COYOPEDAL_FX_OVERDRIVE:
        chain.overdrive.configure(values);
        break;
    case COYOPEDAL_FX_REVERB:
        reverb_state().configure(values);
        break;
    case COYOPEDAL_FX_MODULATION:
        chain.modulation.configure(values);
        break;
    case COYOPEDAL_FX_DELAY:
        chain.delay.configure(values);
        break;
    default:
        break;
    }
}

bool valid(const coyopedal_fx_block_t block, const uint8_t index) noexcept {
    return block < COYOPEDAL_FX_BLOCK_COUNT && index < kBlocks[block].count;
}

} // namespace

extern "C" void coyopedal_fx_reverb_profile_reset(void) {
#if defined(__XTENSA__)
    reverb_profile_input_cycles = 0U;
    reverb_profile_tank_cycles = 0U;
    reverb_profile_blocks = 0U;
    reverb_profile_enabled = true;
#endif
}

extern "C" void coyopedal_fx_reverb_profile_read(std::uint32_t* const input_cycles,
                                                 std::uint32_t* const tank_cycles,
                                                 std::uint32_t* const blocks) {
#if defined(__XTENSA__)
    if (input_cycles != nullptr) {
        *input_cycles = reverb_profile_input_cycles;
    }
    if (tank_cycles != nullptr) {
        *tank_cycles = reverb_profile_tank_cycles;
    }
    if (blocks != nullptr) {
        *blocks = reverb_profile_blocks;
    }
    reverb_profile_enabled = false;
#else
    if (input_cycles != nullptr) {
        *input_cycles = 0U;
    }
    if (tank_cycles != nullptr) {
        *tank_cycles = 0U;
    }
    if (blocks != nullptr) {
        *blocks = 0U;
    }
#endif
}

extern "C" void coyopedal_fx_init(void) {
    chain.modulation.line = external_modulation_line;
    for (uint8_t block = 0; block < COYOPEDAL_FX_BLOCK_COUNT; ++block) {
        const BlockInfo& info = kBlocks[block];
        for (uint8_t index = 0; index < info.count; ++index) {
            chain.values[block][index] = info.params[index].initial;
        }
        apply(static_cast<coyopedal_fx_block_t>(block));
    }
    // Only the gate helps the profiles as shipped; the rest wait to be asked for.
    chain.gate.envelope = 0.0F;
    chain.gate.gain = 1.0F;
    chain.gate.hold_remaining = 0;
    chain.gate.open = false;
    chain.compressor.envelope = 0.0F;
    chain.compressor.gain = 1.0F;
    chain.compressor.gain_step = 0.0F;
    chain.compressor.reduction_db = 0.0F;
    chain.compressor.until_update = 0;
    chain.overdrive.tone_state = 0.0F;
    chain.overdrive.highpass_state = 0.0F;
    chain.overdrive.previous_input = 0.0F;
    chain.enabled[COYOPEDAL_FX_GATE] = true;
    chain.enabled[COYOPEDAL_FX_COMPRESSOR] = false;
    chain.enabled[COYOPEDAL_FX_OVERDRIVE] = false;
    chain.enabled[COYOPEDAL_FX_REVERB] = false;
    chain.enabled[COYOPEDAL_FX_MODULATION] = false;
    chain.enabled[COYOPEDAL_FX_DELAY] = false;
    reverb_state().clear();
    chain.modulation.clear();
    chain.delay.clear();
}

extern "C" void coyopedal_fx_detach(void) {
    chain.enabled.fill(false);
    if (compact_reverb)
        compact_reverb->~CompactReverb();
    compact_reverb = nullptr;
    chain.delay.attach(nullptr, 0);
    chain.modulation.line = external_modulation_line;
}

extern "C" size_t coyopedal_fx_reverb_state_size(void) {
    return sizeof(CompactReverb);
}

extern "C" bool coyopedal_fx_reverb_attach(void* const memory, const size_t bytes) {
    if (memory == nullptr || bytes < sizeof(CompactReverb)) {
        return false;
    }
    compact_reverb = new (memory) CompactReverb{};
    return true;
}

extern "C" size_t coyopedal_fx_reverb_predelay_state_size(void) {
    return CompactReverb::kPredelay * sizeof(float);
}

extern "C" bool coyopedal_fx_reverb_predelay_attach(void* const memory, const size_t bytes) {
    if (compact_reverb == nullptr || memory == nullptr ||
        bytes < coyopedal_fx_reverb_predelay_state_size()) {
        return false;
    }
    compact_reverb->predelay = static_cast<float*>(memory);
    return true;
}

extern "C" size_t coyopedal_fx_reverb_line_count(void) {
    return CompactReverb::kLineCount;
}

extern "C" size_t coyopedal_fx_reverb_line_state_size(const size_t line) {
    return (line < CompactReverb::kLineCount) ? CompactReverb::kMaximum[line] * sizeof(int16_t)
                                              : 0U;
}

extern "C" bool coyopedal_fx_reverb_line_attach(const size_t line, void* const memory,
                                                const size_t bytes) {
    if (compact_reverb == nullptr || line >= CompactReverb::kLineCount || memory == nullptr ||
        bytes < coyopedal_fx_reverb_line_state_size(line)) {
        return false;
    }
    compact_reverb->lines[line] = static_cast<int16_t*>(memory);
    return true;
}

extern "C" size_t coyopedal_fx_modulation_line_state_size(void) {
    return kModulationSamples * sizeof(float);
}

extern "C" bool coyopedal_fx_modulation_line_migrate(void* const memory, const size_t bytes) {
    if (memory == nullptr || bytes < coyopedal_fx_modulation_line_state_size()) {
        return false;
    }
    auto* const destination = static_cast<float*>(memory);
    std::copy_n(chain.modulation.line, kModulationSamples, destination);
    chain.modulation.line = destination;
    return true;
}

extern "C" void coyopedal_fx_set_enabled(const coyopedal_fx_block_t block, const bool enabled) {
    if (block < COYOPEDAL_FX_BLOCK_COUNT) {
        chain.enabled[block] = enabled;
    }
}

extern "C" bool coyopedal_fx_enabled(const coyopedal_fx_block_t block) {
    return block < COYOPEDAL_FX_BLOCK_COUNT && chain.enabled[block];
}

extern "C" bool coyopedal_fx_any_enabled(void) {
    for (const bool enabled : chain.enabled) {
        if (enabled) {
            return true;
        }
    }
    return false;
}

extern "C" const char* coyopedal_fx_name(const coyopedal_fx_block_t block) {
    return block < COYOPEDAL_FX_BLOCK_COUNT ? kBlocks[block].name : "";
}

extern "C" uint8_t coyopedal_fx_param_count(const coyopedal_fx_block_t block) {
    return block < COYOPEDAL_FX_BLOCK_COUNT ? kBlocks[block].count : 0U;
}

extern "C" const coyopedal_fx_param_info_t*
coyopedal_fx_param_info(const coyopedal_fx_block_t block, const uint8_t index) {
    return valid(block, index) ? &kBlocks[block].params[index] : nullptr;
}

extern "C" int16_t coyopedal_fx_param(const coyopedal_fx_block_t block, const uint8_t index) {
    return valid(block, index) ? chain.values[block][index] : 0;
}

extern "C" void coyopedal_fx_set_param(const coyopedal_fx_block_t block, const uint8_t index,
                                       const int16_t value) {
    if (!valid(block, index)) {
        return;
    }
    const Info& info = kBlocks[block].params[index];
    chain.values[block][index] = std::clamp(value, info.minimum, info.maximum);
    apply(block);
}

extern "C" bool coyopedal_fx_delay_attach(float* const memory, const size_t samples) {
    const bool attached = chain.delay.attach(memory, samples);
    if (attached) {
        // The TIME clamp depends on the capacity, so the setting has to be re-derived
        // now that there is one.
        apply(COYOPEDAL_FX_DELAY);
    }
    return attached;
}

extern "C" bool coyopedal_fx_delay_ready(void) {
    return chain.delay.ready();
}

extern "C" bool coyopedal_fx_gate_closed(void) {
    return chain.enabled[COYOPEDAL_FX_GATE] && !chain.gate.open;
}

extern "C" int16_t coyopedal_fx_compressor_reduction(void) {
    if (!chain.enabled[COYOPEDAL_FX_COMPRESSOR]) {
        return 0;
    }
    const float tenths = chain.compressor.reduction_db * 10.0F;
    return static_cast<int16_t>(std::clamp(tenths, -600.0F, 0.0F));
}

#if defined(__XTENSA__)
extern "C" void coyopedal_fx_process_pre(float* samples, size_t frame_count)
    __attribute__((section(".iram1")));
extern "C" void coyopedal_fx_process_delay(float* samples, size_t frame_count)
    __attribute__((section(".iram1")));
extern "C" void coyopedal_fx_process_reverb_stereo(float* left, float* right, size_t frame_count)
    __attribute__((section(".iram1")));
#endif

extern "C" void coyopedal_fx_process_pre(float* const samples, const size_t frame_count) {
    if (samples == nullptr) {
        return;
    }
    if (chain.enabled[COYOPEDAL_FX_GATE]) {
        chain.gate.process(samples, frame_count);
    }
    if (chain.enabled[COYOPEDAL_FX_COMPRESSOR]) {
        chain.compressor.process(samples, frame_count);
    }
    // The modulation block is in front of the amp, between the compressor and the
    // drive, which is where a rig puts its modulation pedals.
    if (chain.enabled[COYOPEDAL_FX_MODULATION]) {
        chain.modulation.process(samples, frame_count);
    }
    if (chain.enabled[COYOPEDAL_FX_OVERDRIVE]) {
        chain.overdrive.process(samples, frame_count);
    }
}

extern "C" void coyopedal_fx_process_delay(float* const samples, const size_t frame_count) {
    if (samples != nullptr && chain.enabled[COYOPEDAL_FX_DELAY]) {
        chain.delay.process(samples, frame_count);
    }
}

extern "C" void coyopedal_fx_process_reverb_stereo(float* const left, float* const right,
                                                   const size_t frame_count) {
    if (left == nullptr || right == nullptr) {
        return;
    }
    if (chain.enabled[COYOPEDAL_FX_REVERB]) {
        reverb_state().process_stereo(left, right, frame_count);
        return;
    }
    std::copy_n(left, frame_count, right);
}
