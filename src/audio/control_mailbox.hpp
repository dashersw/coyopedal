#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace coyopedal::pedal {

// Latest-value handoff from one serialized control writer to one audio reader.
// Each side owns a buffer; the atomic middle buffer transfers ownership. Updates
// may coalesce; the reader never waits for a coefficient calculation or reads
// a partial value. Keep these mailboxes in internal SRAM on the S3: ESP-IDF
// routes those 32-bit atomic exchanges through S32C1I, while PSRAM uses a lock.
// Reset/detach must still happen with the audio pipeline stopped.
template <typename T> class ControlMailbox {
  public:
    void publish(const T& value) noexcept {
        values_[back_] = value;
        back_ = middle_.exchange(back_ | kDirty, std::memory_order_acq_rel) & kIndex;
    }

    // The returned value remains reader-owned until its next successful consume.
    const T* consume() noexcept {
        if ((middle_.load(std::memory_order_relaxed) & kDirty) == 0U) {
            return nullptr;
        }
        front_ = middle_.exchange(front_, std::memory_order_acq_rel) & kIndex;
        return &values_[front_];
    }

  private:
#if !defined(ESP_PLATFORM)
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
#endif
    static constexpr std::uint32_t kDirty = 4U;
    static constexpr std::uint32_t kIndex = 3U;
    std::array<T, 3> values_{};
    std::atomic<std::uint32_t> middle_{1U};
    std::uint32_t front_ = 0U;
    std::uint32_t back_ = 2U;
};

} // namespace coyopedal::pedal
