#pragma once

#include <cstddef>
#include <atomic>

namespace coyopedal::pedal {

// Mono adaptation of the P4 cabinet: 64-tap partitions and 128-point overlap-add.
// A packed 64-point complex FFT implements each 128-point real transform.
// Coefficients, history and workspace share one cache-resident PSRAM allocation.
class CabinetIR {
  public:
    static constexpr unsigned kTaps = 1024;
    static constexpr unsigned kFrames = 64;
    static constexpr unsigned kPartitions = kTaps / kFrames;
    static constexpr unsigned kBins = 65;
    static constexpr unsigned kBinGroup = 4;
    static constexpr unsigned kStoredBins = 68;
    // All history reads finish on stage B before the slot returns to stage A.
    static constexpr unsigned kHistorySlots = 16;
    // Group four adjacent bins; pad rows to avoid cache-set collisions.
    static constexpr unsigned kHistoryStride = kHistorySlots + 1;
    static constexpr unsigned kSpectrumBytes = (kPartitions + kHistoryStride) * kStoredBins * 8;
    static constexpr unsigned kResidentBytes = kSpectrumBytes + (64 + 64 + 32 + 48) * 8 + 64 * 4;

    CabinetIR() = default;
    ~CabinetIR();
    CabinetIR(const CabinetIR&) = delete;
    CabinetIR& operator=(const CabinetIR&) = delete;
    // Called only with the pipeline drained. Failure retains the previous IR.
    bool load(const float* taps, unsigned count);
    void reset();
    void release();
    // Control thread only, with the DSP pipeline drained.
    void cache_residency(bool enabled);
    bool loaded() const {
        return memory_ != nullptr;
    }
    void process(float* samples, float gain);
    // Reuse the transport slot's two channels for the current output and tail.
    // begin is ordered on stage B; finish is ordered on stage A. Up to three
    // begin calls may precede their corresponding finish calls.
    unsigned begin(float* samples, float* imaginary);
    void finish(float* samples, float* imaginary, unsigned cursor, float gain);
    void timing(unsigned* cycles) const;

  private:
    struct Complex {
        float r, i;
    };
    struct Workspace {
        Complex work[64];
        Complex inverse_work[64];
        Complex twiddle[32];
        Complex fft_twiddle[48];
    };
    struct Output {
        float overlap[64];
    };
    void fft(Complex* data, bool inverse) const;
    void inverse(float* samples, float* imaginary);
    void forward(const float* samples, unsigned count, Complex* spectrum, unsigned stride) const;
    void sum(unsigned cursor, float* real, float* imaginary) const;
    Complex* memory_{};
    Workspace* workspace_{};
    Output* output_{};
    unsigned cursor_{};
    std::atomic<unsigned> phase_cycles_[4]{};
    std::atomic<unsigned> phase_blocks_[2]{};
    bool cache_locked_{};
};

} // namespace coyopedal::pedal
