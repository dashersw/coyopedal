#include "audio/tuner.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#if defined(ESP_PLATFORM)
#define COYOPEDAL_EXT_RAM_BSS __attribute__((section(".ext_ram.bss")))
#else
#define COYOPEDAL_EXT_RAM_BSS
#endif

namespace {

constexpr float kSampleRate = 48000.0F;

// Decimated by two, to 24 kHz. The guitar's fundamentals run 82 Hz to about
// 1.3 kHz, so 12 kHz of bandwidth is generous, and the difference function costs
// lag-count times window - both of which fall with the rate.
//
// Decimating by four would be too coarse at the top of the range. The lag grid is
// the pitch resolution: at 12 kHz, A440 is a 27-sample period, and interpolation
// is left carrying a 1.4-cent bias. At 24 kHz the same note is 55 samples and the
// bias falls below the resolution anyone tunes to. A search costs about 320
// thousand multiply-accumulates.
constexpr int kDecimation = 2;
constexpr float kDecimatedRate = kSampleRate / kDecimation;

// Anti-aliasing before the decimation. Without it a string's upper harmonics fold
// back into the band the fundamental lives in, and the detector is then looking
// for a period in a signal that has acquired partials the instrument never played.
// Eleven taps of a windowed sinc at 0.22 of the input rate: down about 40 dB by
// 6 kHz, which is where folding would start to matter.
constexpr int kFirTaps = 11;
constexpr std::array<float, kFirTaps> kFir = {
    0.0060F, 0.0203F, 0.0553F, 0.1090F, 0.1592F, 0.1804F,
    0.1592F, 0.1090F, 0.0553F, 0.0203F, 0.0060F,
};

// Half the analysis window is the longest lag the difference function can look at
// without running off the end, so the window is twice the longest period of
// interest. 2048 samples at 24 kHz is 85 ms, seven periods of a low E.
constexpr int kWindow = 2048;
constexpr int kHalf = kWindow / 2;

// 75 Hz to 1333 Hz. Below the low E with room for a dropped tuning, above the high
// E at the 24th fret.
constexpr int kMinLag = static_cast<int>(kDecimatedRate / 1333.0F); // 18
constexpr int kMaxLag = static_cast<int>(kDecimatedRate / 75.0F);   // 320

static_assert(kMaxLag < kHalf, "the longest lag has to fit inside half the window");

// How often to search, in samples of the decimated stream. One whole window, so a
// search consumes fresh data rather than re-deciding on data it has already seen.
constexpr int kHopSamples = kWindow; // at most twelve searches a second

// Lags computed per call. A whole search is 302 lags over 1024 samples, which is
// milliseconds of straight-line work. Sixteen lags is about 16k operations,
// comfortably inside a UI frame, so a search spans a few dozen passes of the UI
// task and never holds it for long.
constexpr int kLagsPerCall = 16;

// YIN's threshold on the cumulative mean normalised difference. Below this a lag
// is periodic enough to believe. 0.15 is the value from the paper and it behaves:
// tighter starts refusing real notes on a decaying string, looser starts accepting
// the room.
constexpr float kAperiodicity = 0.15F;

// About -60 dBFS. A plucked string arrives far above this even as it decays; a
// quiet room does not reach it. This is what stops the panel naming a note when
// nobody is playing.
constexpr float kRmsFloor = 0.001F;

struct State {
    std::array<float, kWindow> window;
    int write;  // next write position in the ring
    int filled; // samples collected, saturating at kWindow
    int since_search;

    // A search in progress. The window is copied out when it starts, so the audio
    // path can keep filling the ring underneath without disturbing it.
    bool searching;
    int search_lag;
    float search_level;

    std::array<float, kFirTaps> fir_history;
    int fir_write;
    int phase; // decimation counter

    coyopedal_tuner_reading_t reading;

    // Scratch for the search, in static storage (PSRAM on the device) rather than
    // on the caller's stack: the linearised window alone is 8 KB.
    std::array<float, kWindow> linear;
    std::array<float, kMaxLag + 1> diff;
    std::array<float, kMaxLag + 1> normalised;
};

COYOPEDAL_EXT_RAM_BSS State state;

const char* const kNoteNames[12] = {"C",  "C#", "D",  "D#", "E",  "F",
                                    "F#", "G",  "G#", "A",  "A#", "B"};

// The difference function over a slice of lags, so a search can be spread across
// several passes of the UI task instead of owning one.
void difference_range(const float* const w, const int first, const int last) {
    for (int lag = first; lag <= last; ++lag) {
        float sum = 0.0F;
        for (int i = 0; i < kHalf; ++i) {
            const float d = w[i] - w[i + lag];
            sum += d * d;
        }
        state.diff[lag] = sum;
    }
}

// YIN's step 3. Dividing each difference by the running mean of those before it is
// what removes the zero-lag minimum, and it is the whole reason this does not pick
// the octave below: a true period and its double both dip, but the double dips
// later and by then the running mean has caught up with it.
void normalise() {
    float running = 0.0F;
    state.normalised[kMinLag - 1 < 0 ? 0 : kMinLag - 1] = 1.0F;
    for (int lag = kMinLag; lag <= kMaxLag; ++lag) {
        running += state.diff[lag];
        const int count = lag - kMinLag + 1;
        state.normalised[lag] =
            (running > 0.0F) ? (state.diff[lag] * static_cast<float>(count) / running) : 1.0F;
    }
}

// The first lag that dips below the threshold, not the smallest one overall. Taking
// the global minimum is what picks the octave below, because a signal periodic at T
// is also periodic at 2T and the deeper dip is often the wrong one.
int pick_lag(float* const out_value) {
    int best = -1;
    for (int lag = kMinLag; lag <= kMaxLag; ++lag) {
        if (state.normalised[lag] < kAperiodicity) {
            // Walk down to the bottom of this dip rather than taking its first
            // sample, which lands the parabola on the true minimum.
            while (lag + 1 <= kMaxLag && state.normalised[lag + 1] < state.normalised[lag]) {
                ++lag;
            }
            best = lag;
            break;
        }
    }
    if (best < 0) {
        return -1;
    }
    *out_value = state.normalised[best];
    return best;
}

// Sub-sample refinement. Even at 24 kHz the lag grid is whole samples, which at
// 330 Hz is about 14 cents from one lag to the next - far too coarse to tune with -
// so the minimum is interpolated against its neighbours.
float refine(const int lag) {
    if (lag <= kMinLag || lag >= kMaxLag) {
        return static_cast<float>(lag);
    }
    const float a = state.normalised[lag - 1];
    const float b = state.normalised[lag];
    const float c = state.normalised[lag + 1];
    const float denominator = a - 2.0F * b + c;
    if (std::fabs(denominator) < 1e-12F) {
        return static_cast<float>(lag);
    }
    return static_cast<float>(lag) + 0.5F * (a - c) / denominator;
}

float rms(const float* const w, const int n) {
    float sum = 0.0F;
    for (int i = 0; i < n; ++i) {
        sum += w[i] * w[i];
    }
    return std::sqrt(sum / static_cast<float>(n));
}

} // namespace

extern "C" void coyopedal_tuner_reset(void) {
    state = State{};
}

extern "C" void coyopedal_tuner_feed(const float* const samples, const size_t frame_count) {
    if (samples == nullptr) {
        return;
    }
    for (size_t i = 0; i < frame_count; ++i) {
        state.fir_history[state.fir_write] = samples[i];
        state.fir_write = (state.fir_write + 1) % kFirTaps;

        if (++state.phase < kDecimation) {
            continue;
        }
        state.phase = 0;

        // Oldest sample first, so the taps line up with the kernel.
        float filtered = 0.0F;
        for (int t = 0; t < kFirTaps; ++t) {
            filtered += kFir[t] * state.fir_history[(state.fir_write + t) % kFirTaps];
        }

        state.window[state.write] = filtered;
        state.write = (state.write + 1) % kWindow;
        if (state.filled < kWindow) {
            ++state.filled;
        }
        ++state.since_search;
    }
}

extern "C" bool coyopedal_tuner_update(void) {
    float* const w = state.linear.data();

    if (!state.searching) {
        if (state.filled < kWindow || state.since_search < kHopSamples) {
            return false;
        }
        state.since_search = 0;

        // Linearise the ring, oldest first, so the difference function can walk it.
        // Copied rather than indexed through the ring because the audio path keeps
        // writing to it while the search runs across the passes that follow.
        for (int i = 0; i < kWindow; ++i) {
            w[i] = state.window[(state.write + i) % kWindow];
        }

        state.search_level = rms(w, kWindow);
        if (state.search_level < kRmsFloor) {
            coyopedal_tuner_reading_t quiet{};
            quiet.level = state.search_level;
            quiet.clarity = 1.0F;
            state.reading = quiet; // not voiced
            return true;
        }
        state.searching = true;
        state.search_lag = kMinLag;
    }

    // A slice of the difference function, then hand the loop back.
    const int until =
        (state.search_lag + kLagsPerCall > kMaxLag) ? kMaxLag : state.search_lag + kLagsPerCall;
    difference_range(w, state.search_lag, until);
    state.search_lag = until + 1;
    if (state.search_lag <= kMaxLag) {
        return false; // more to do next pass
    }
    state.searching = false;

    coyopedal_tuner_reading_t next{};
    next.level = state.search_level;
    next.clarity = 1.0F;

    normalise();

    // The best score in range, whether or not it passed. Reported so a rejection can
    // be attributed rather than guessed at.
    float best_seen = 1.0F;
    for (int lag = kMinLag; lag <= kMaxLag; ++lag) {
        if (state.normalised[lag] < best_seen) {
            best_seen = state.normalised[lag];
        }
    }
    next.clarity = best_seen;

    float quality = 1.0F;
    const int lag = pick_lag(&quality);
    if (lag < 0) {
        state.reading = next; // periodic enough for nothing
        return true;
    }

    const float refined = refine(lag);
    if (refined <= 0.0F) {
        state.reading = next;
        return true;
    }

    const float frequency = kDecimatedRate / refined;

    // Equal temperament against A440, as a MIDI number so the note, the octave and
    // the cents all fall out of the same figure.
    const float midi = 69.0F + 12.0F * std::log2(frequency / 440.0F);
    const int nearest = static_cast<int>(std::lround(midi));

    next.voiced = true;
    next.frequency = frequency;
    next.cents = 100.0F * (midi - static_cast<float>(nearest));
    next.note = ((nearest % 12) + 12) % 12;
    next.octave = nearest / 12 - 1;

    state.reading = next;
    return true;
}

extern "C" void coyopedal_tuner_read(coyopedal_tuner_reading_t* const out) {
    if (out != nullptr) {
        *out = state.reading;
    }
}

extern "C" const char* coyopedal_tuner_note_name(const int note) {
    if (note < 0 || note > 11) {
        return "-";
    }
    return kNoteNames[note];
}
