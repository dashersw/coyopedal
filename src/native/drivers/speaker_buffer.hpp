#pragma once
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>

// Single producer (completed DSP blocks), single consumer (I2S worker).
// Caller owns PSRAM storage. Consumer adjusts its rate for the independent USB
// and I2S clocks; neither output ever waits for the other.
struct SpeakerBuffer {
    static constexpr std::uint32_t capacity = 2048;
    std::int16_t* samples{};
    std::atomic<std::uint32_t> read{}, write{};
    float phase{};
    std::size_t available() const {
        return write.load(std::memory_order_acquire) - read.load(std::memory_order_acquire);
    }
    std::size_t push(const std::uint64_t* frames, std::size_t count) {
        const auto w = write.load(std::memory_order_relaxed);
        const auto r = read.load(std::memory_order_acquire);
        count = std::min<std::size_t>(count, capacity - (w - r));
        for (std::size_t i = 0; i < count; ++i) {
            const auto left = static_cast<std::int16_t>(frames[i] >> 16);
            const auto right = static_cast<std::int16_t>(frames[i] >> 48);
            samples[(w + i) & (capacity - 1)] = (static_cast<std::int32_t>(left) + right) / 2;
        }
        write.store(w + count, std::memory_order_release);
        return count;
    }
    void clear() {
        read.store(write.load(std::memory_order_acquire), std::memory_order_release);
        phase = 0;
    }
    bool render(std::int16_t* out, std::size_t count) {
        auto r = read.load(std::memory_order_relaxed);
        const auto w = write.load(std::memory_order_acquire);
        const auto queued = w - r;
        // Leave enough samples for a complete batch even at the slow-clock
        // equilibrium (100 samples below the target at -1000 ppm).
        const float target = static_cast<float>(std::max<std::size_t>(384, count + 128));
        const float step =
            1.0F + std::clamp((static_cast<float>(queued) - target) * 0.00001F, -0.003F, 0.003F);
        if (queued < static_cast<std::size_t>(phase + count * step) + 2)
            return false;
        for (std::size_t i = 0; i < count; ++i) {
            const auto offset = static_cast<std::uint32_t>(phase);
            const float fraction = phase - offset;
            const auto a = samples[(r + offset) & (capacity - 1)];
            const auto b = samples[(r + offset + 1) & (capacity - 1)];
            out[i] = static_cast<std::int16_t>(a + (b - a) * fraction);
            phase += step;
        }
        const auto consumed = static_cast<std::uint32_t>(phase);
        phase -= consumed;
        read.store(r + consumed, std::memory_order_release);
        return true;
    }
};
