// Called at the end of the real target frame, including the display flush.
// Warm up for 300 frames, collect 3600 frames, then log once outside the sample.
#include <cstdint>
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "ui/node_model.h"

namespace {
constexpr unsigned kWarmup = 300;
constexpr unsigned kSamples = 3600;
constexpr unsigned kBins = 1024;
constexpr unsigned kBinUs = 250;
struct Distribution {
    std::uint32_t bins[kBins]{};
    std::int64_t sum{};
    std::int64_t maximum{};
    unsigned over_budget{};
    void add(std::int64_t value) {
        sum += value;
        if (value > maximum)
            maximum = value;
        if (value > 16667)
            ++over_budget;
        const auto bin = static_cast<unsigned>(value / kBinUs);
        ++bins[bin < kBins ? bin : kBins - 1];
    }
    unsigned percentile(unsigned percent) const {
        unsigned count = 0;
        for (unsigned i = 0; i < kBins; ++i) {
            count += bins[i];
            if (count * 100 >= kSamples * percent)
                return (i + 1) * kBinUs;
        }
        return kBins * kBinUs;
    }
};
EXT_RAM_BSS_ATTR Distribution work;
EXT_RAM_BSS_ATTR Distribution cadence;
unsigned frames{};
std::int64_t previous_done{};
} // namespace

extern "C" void gea_frame_benchmark_sample(std::int64_t start, std::int64_t done) {
    ++frames;
    if (frames > kWarmup && frames <= kWarmup + kSamples) {
        work.add(done - start);
        cadence.add(done - previous_done);
    }
    previous_done = done;
    if (frames != kWarmup + kSamples + 1)
        return;
    ESP_LOGI("ui_bench",
             "RESULT frames=%u node=%u style=%u work_avg=%lld work_p99_le=%u work_max=%lld "
             "work_over16667=%u done_avg=%lld done_p99_le=%u done_max=%lld done_over16667=%u "
             "fps_milli=%lld psram_free=%u internal_free=%u",
             kSamples, unsigned(sizeof(gea::embedded::ui::Node)),
             unsigned(sizeof(gea::embedded::ui::ComputedStyle)), work.sum / kSamples,
             work.percentile(99), work.maximum, work.over_budget, cadence.sum / kSamples,
             cadence.percentile(99), cadence.maximum, cadence.over_budget,
             1000000000LL * kSamples / cadence.sum,
             unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
             unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
}
