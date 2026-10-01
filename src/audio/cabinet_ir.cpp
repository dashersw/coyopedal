#include "cabinet_ir.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_cpu.h"
#include "esp32s3/rom/cache.h"
#define CABINET_NOINLINE __attribute__((noinline, noclone))
#else
#define IRAM_ATTR
#define CABINET_NOINLINE __attribute__((noinline))
#endif

namespace coyopedal::pedal {
#ifdef ESP_PLATFORM
extern "C" void pedal_cabinet_fft64(float* data, const float* twiddle, float sign);
extern "C" void pedal_cabinet_sum64(const void* coefficients, const void* history, unsigned cursor,
                                    float* real, float* imaginary);
#endif
static inline unsigned cycle_count() {
#ifdef ESP_PLATFORM
    return esp_cpu_get_cycle_count();
#else
    return 0;
#endif
}
void CabinetIR::timing(unsigned* cycles) const {
    for (unsigned i = 0; i < 4; ++i) {
        const unsigned blocks = phase_blocks_[i >= 2].load(std::memory_order_relaxed);
        cycles[i] = blocks ? phase_cycles_[i].load(std::memory_order_relaxed) / blocks : 0;
    }
}
CabinetIR::~CabinetIR() {
    release();
}
void CabinetIR::release() {
    cache_residency(false);
    std::free(memory_);
    memory_ = nullptr;
    workspace_ = nullptr;
    output_ = nullptr;
}
void CabinetIR::cache_residency(bool enabled) {
    if (!memory_ || cache_locked_ == enabled)
        return;
#ifdef ESP_PLATFORM
    static_assert(CONFIG_ESP32S3_DATA_CACHE_LINE_SIZE == 64);
    static_assert(kResidentBytes < CONFIG_ESP32S3_DATA_CACHE_SIZE * 3 / 4);
    if (enabled) {
        // Pin only our aligned, padded allocation. This leaves at least one
        // of the four ways available in every set for the rest of the app.
        for (unsigned offset = 0; offset < kResidentBytes; offset += 64) {
            const volatile auto word =
                *reinterpret_cast<volatile uint32_t*>(reinterpret_cast<char*>(memory_) + offset);
            (void)word;
        }
        Cache_Lock_DCache_Items(reinterpret_cast<uint32_t>(memory_), (kResidentBytes + 63) / 64);
    } else {
        Cache_Unlock_DCache_Items(reinterpret_cast<uint32_t>(memory_), (kResidentBytes + 63) / 64);
    }
#endif
    cache_locked_ = enabled;
}

bool CabinetIR::load(const float* taps, unsigned count) {
    if (!taps || !count || count > kTaps)
        return false;
    for (unsigned i = 0; i < count; ++i)
        if (!std::isfinite(taps[i]) || std::fabs(taps[i]) > 32.0F)
            return false;
    static_assert(sizeof(Workspace) + sizeof(Output) == kResidentBytes - kSpectrumBytes);
#ifdef ESP_PLATFORM
    auto* next = static_cast<Complex*>(heap_caps_aligned_calloc(
        64, 1, (kResidentBytes + 63) & ~63U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#else
    auto* next = static_cast<Complex*>(std::calloc(1, kResidentBytes));
#endif
    if (!next)
        return false;
    workspace_ = reinterpret_cast<Workspace*>(reinterpret_cast<char*>(next) + kSpectrumBytes);
    output_ = reinterpret_cast<Output*>(workspace_ + 1);
    for (unsigned i = 0; i < 32; ++i) {
        const float angle = -6.2831853071795864769F * i / 128;
        workspace_->twiddle[i] = {std::cos(angle), std::sin(angle)};
    }
    for (unsigned i = 0; i < 48; ++i) {
        const float angle = 6.2831853071795864769F * i / 64;
        workspace_->fft_twiddle[i] = {std::cos(angle), std::sin(angle)};
    }
    for (unsigned p = 0; p < kPartitions; ++p) {
        if (p * kFrames < count)
            forward(taps + p * kFrames, std::min(kFrames, count - p * kFrames),
                    next + p * kBinGroup, kPartitions);
    }
    cache_residency(false);
    std::free(memory_);
    memory_ = next;
    reset();
    cache_residency(true);
    return true;
}

void CabinetIR::reset() {
    if (memory_)
        std::memset(memory_ + kPartitions * kStoredBins, 0,
                    kHistoryStride * kStoredBins * sizeof(Complex));
    if (output_)
        std::memset(output_->overlap, 0, sizeof output_->overlap);
    cursor_ = 0;
    for (auto& value : phase_cycles_)
        value.store(0, std::memory_order_relaxed);
    for (auto& value : phase_blocks_)
        value.store(0, std::memory_order_relaxed);
}

IRAM_ATTR CABINET_NOINLINE void CabinetIR::fft(Complex* data, bool inverse) const {
#ifdef ESP_PLATFORM
    pedal_cabinet_fft64(reinterpret_cast<float*>(data),
                        reinterpret_cast<const float*>(workspace_->fft_twiddle),
                        inverse ? -1.0F : 1.0F);
#else
    // Same decimation-in-frequency transform as the S3 assembly kernel.
    const float sign = inverse ? -1.0F : 1.0F;
    for (unsigned length = 64; length >= 4; length >>= 2) {
        const unsigned quarter = length / 4, stride = 64 / length;
        for (unsigned base = 0; base < 64; base += length)
            for (unsigned j = 0; j < quarter; ++j) {
                const auto a = data[base + j], b = data[base + j + quarter],
                           c = data[base + j + 2 * quarter], d = data[base + j + 3 * quarter];
                const Complex u0{a.r + c.r, a.i + c.i}, u1{a.r - c.r, a.i - c.i},
                    u2{b.r + d.r, b.i + d.i}, u3{(b.i - d.i) * sign, (d.r - b.r) * sign};
                data[base + j] = {u0.r + u2.r, u0.i + u2.i};
                const Complex values[3]{{u1.r + u3.r, u1.i + u3.i},
                                        {u0.r - u2.r, u0.i - u2.i},
                                        {u1.r - u3.r, u1.i - u3.i}};
                for (unsigned p = 1; p <= 3; ++p) {
                    if (quarter == 1) {
                        data[base + p] = values[p - 1];
                        continue;
                    }
                    auto t = workspace_->fft_twiddle[p * j * stride];
                    t.i *= sign;
                    const auto v = values[p - 1];
                    data[base + j + p * quarter] = {t.r * v.r + t.i * v.i, t.r * v.i - t.i * v.r};
                }
            }
    }
#endif
    // Both callers read digit-reversed work directly. No permutation pass.
}

IRAM_ATTR CABINET_NOINLINE void CabinetIR::forward(const float* samples, unsigned count,
                                                   Complex* spectrum, unsigned stride) const {
    auto& w = *workspace_;
    for (unsigned i = 0; i < 32; ++i)
        workspace_->work[i] = {2 * i < count ? samples[2 * i] : 0,
                               2 * i + 1 < count ? samples[2 * i + 1] : 0};
    std::memset(workspace_->work + 32, 0, 32 * sizeof(Complex));
    fft(workspace_->work, false);
    spectrum[0] = {workspace_->work[0].r + workspace_->work[0].i, 0};
    spectrum[64 * stride] = {workspace_->work[0].r - workspace_->work[0].i, 0};
    for (unsigned k = 1; k < 32; ++k) {
        const unsigned j = ((k & 3) << 4) | (k & 12) | ((k & 48) >> 4);
        const unsigned mirror_k = 64 - k;
        const unsigned mirror_j = ((mirror_k & 3) << 4) | (mirror_k & 12) | ((mirror_k & 48) >> 4);
        const auto a = workspace_->work[j], b = workspace_->work[mirror_j], t = w.twiddle[k];
        const float dr = a.r - b.r, di = a.i + b.i;
        const float sr = a.r + b.r, si = a.i - b.i;
        const float tr = t.r * di + t.i * dr, ti = t.i * di - t.r * dr;
        spectrum[(k & ~3U) * stride + (k & 3)] = {0.5F * (sr + tr), 0.5F * (si + ti)};
        const unsigned mirror = 64 - k;
        spectrum[(mirror & ~3U) * stride + (mirror & 3)] = {0.5F * (sr - tr), 0.5F * (ti - si)};
    }
    spectrum[32 * stride] = {workspace_->work[2].r, -workspace_->work[2].i};
}

IRAM_ATTR CABINET_NOINLINE void CabinetIR::sum(unsigned cursor, float* real,
                                               float* imaginary) const {
    auto* history = memory_ + kPartitions * kStoredBins;
#ifdef ESP_PLATFORM
    pedal_cabinet_sum64(memory_, history, cursor, real, imaginary);
#else
    for (unsigned k = 0; k < 64; k += kBinGroup) {
        const auto* h = memory_ + k * kPartitions;
        const auto* row = history + k * kHistoryStride;
        unsigned position = cursor;
        float r0 = 0, i0 = 0, r1 = 0, i1 = 0, r2 = 0, i2 = 0, r3 = 0, i3 = 0;
        for (unsigned p = 0; p < kPartitions;) {
            const unsigned count = std::min(kPartitions - p, position + 1);
            const auto* c = h;
            const auto* x = row + position * kBinGroup;
            for (unsigned j = 0; j < count; ++j) {
                r0 = r0 + c[0].r * x[0].r - c[0].i * x[0].i;
                i0 = i0 + c[0].r * x[0].i + c[0].i * x[0].r;
                r1 = r1 + c[1].r * x[1].r - c[1].i * x[1].i;
                i1 = i1 + c[1].r * x[1].i + c[1].i * x[1].r;
                r2 = r2 + c[2].r * x[2].r - c[2].i * x[2].i;
                i2 = i2 + c[2].r * x[2].i + c[2].i * x[2].r;
                r3 = r3 + c[3].r * x[3].r - c[3].i * x[3].i;
                i3 = i3 + c[3].r * x[3].i + c[3].i * x[3].r;
                c += kBinGroup;
                x -= kBinGroup;
            }

            h += count * kBinGroup;
            p += count;
            position = kHistorySlots - 1;
        }
        real[k] = r0;
        if (k)
            imaginary[k] = i0;
        real[k + 1] = r1;
        imaginary[k + 1] = i1;
        real[k + 2] = r2;
        imaginary[k + 2] = i2;
        real[k + 3] = r3;
        imaginary[k + 3] = i3;
    }
#endif
    // DC and Nyquist are real. The latter occupies imaginary[0] in the slot.
    const auto* h = memory_ + 64 * kPartitions;
    const auto* row = history + 64 * kHistoryStride;
    float nyquist = 0;
    unsigned position = cursor;
    for (unsigned p = 0; p < kPartitions; ++p) {
        nyquist += h[p * kBinGroup].r * row[position * kBinGroup].r;
        position = position ? position - 1 : kHistorySlots - 1;
    }
    imaginary[0] = nyquist;
}

IRAM_ATTR CABINET_NOINLINE void CabinetIR::inverse(float* samples, float* imaginary) {
    // Stage A's work must not alias the next block's forward FFT on stage B.
    auto* work = workspace_->inverse_work;
    work[0] = {0.5F * (samples[0] + imaginary[0]), 0.5F * (samples[0] - imaginary[0])};
    for (unsigned k = 1; k < 32; ++k) {
        const Complex a{samples[k], imaginary[k]}, b{samples[64 - k], imaginary[64 - k]};
        const auto t = workspace_->twiddle[k];
        const float dr = a.r - b.r, di = a.i + b.i;
        const float sr = a.r + b.r, si = a.i - b.i;
        const float tr = t.i * dr - t.r * di, ti = t.r * dr + t.i * di;
        work[k] = {0.5F * (sr + tr), 0.5F * (si + ti)};
        work[64 - k] = {0.5F * (sr - tr), 0.5F * (ti - si)};
    }
    work[32] = {samples[32], -imaginary[32]};
    fft(work, true);
    for (unsigned i = 0; i < 32; ++i) {
        const unsigned j = ((i & 3) << 4) | (i & 12) | ((i & 48) >> 4);
        samples[2 * i] = work[j].r;
        samples[2 * i + 1] = work[j].i;
        imaginary[2 * i] = work[j + 2].r;
        imaginary[2 * i + 1] = work[j + 2].i;
    }
}

IRAM_ATTR unsigned CabinetIR::begin(float* samples, float* imaginary) {
    if (!memory_)
        return 0;
    auto* history = memory_ + kPartitions * kStoredBins;
    const unsigned cursor = cursor_;
    const unsigned start = cursor == 0 ? cycle_count() : 0;
    forward(samples, kFrames, history + cursor * kBinGroup, kHistoryStride);
    const unsigned transformed = cursor == 0 ? cycle_count() : 0;
    sum(cursor, samples, imaginary);
    if (cursor == 0) {
        phase_cycles_[0].fetch_add(transformed - start, std::memory_order_relaxed);
        phase_cycles_[1].fetch_add(cycle_count() - transformed, std::memory_order_relaxed);
        phase_blocks_[0].fetch_add(1, std::memory_order_relaxed);
    }
    cursor_ = cursor + 1 == kHistorySlots ? 0 : cursor + 1;
    return cursor;
}

IRAM_ATTR void CabinetIR::finish(float* samples, float* imaginary, unsigned cursor, float gain) {
    if (!memory_)
        return;
    const unsigned start = cursor == 0 ? cycle_count() : 0;
    inverse(samples, imaginary);
    const unsigned inverted = cursor == 0 ? cycle_count() : 0;
    for (unsigned i = 0; i < kFrames; ++i) {
        samples[i] = ((samples[i] + output_->overlap[i]) * (1.0F / 64)) * gain;
        output_->overlap[i] = imaginary[i];
    }
    if (cursor == 0) {
        phase_cycles_[2].fetch_add(inverted - start, std::memory_order_relaxed);
        phase_cycles_[3].fetch_add(cycle_count() - inverted, std::memory_order_relaxed);
        phase_blocks_[1].fetch_add(1, std::memory_order_relaxed);
    }
}

IRAM_ATTR void CabinetIR::process(float* samples, float gain) {
    if (!memory_)
        return;
    float imaginary[kFrames];
    const unsigned cursor = begin(samples, imaginary);
    finish(samples, imaginary, cursor, gain);
}
} // namespace coyopedal::pedal
