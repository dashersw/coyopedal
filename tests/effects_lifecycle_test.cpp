#include "audio/effects.h"
#include <array>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <vector>

using Memory = std::unique_ptr<void, decltype(&std::free)>;
Memory allocate(std::size_t bytes) {
    auto memory = Memory(std::aligned_alloc(16, (bytes + 15) & ~std::size_t(15)), &std::free);
    assert(memory);
    return memory;
}

int main() {
    assert(coyopedal_fx_reverb_state_size() != 0);
    std::array<float, 4096> reference{};
    for (unsigned cycle = 0; cycle < 4; ++cycle) {
        auto reverb = allocate(coyopedal_fx_reverb_state_size());
        assert(coyopedal_fx_reverb_attach(reverb.get(), coyopedal_fx_reverb_state_size()));
        std::vector<Memory> lines;
        const auto predelay_bytes = coyopedal_fx_reverb_predelay_state_size();
        lines.push_back(allocate(predelay_bytes));
        assert(coyopedal_fx_reverb_predelay_attach(lines.back().get(), predelay_bytes));
        for (unsigned line = 0; line < coyopedal_fx_reverb_line_count(); ++line) {
            const auto bytes = coyopedal_fx_reverb_line_state_size(line);
            lines.push_back(allocate(bytes));
            assert(coyopedal_fx_reverb_line_attach(line, lines.back().get(), bytes));
        }
        coyopedal_fx_init();
        auto delay = allocate(96000 * sizeof(float));
        assert(coyopedal_fx_delay_attach(static_cast<float*>(delay.get()), 96000));
        for (unsigned block = 0; block < COYOPEDAL_FX_BLOCK_COUNT; ++block)
            coyopedal_fx_set_enabled(static_cast<coyopedal_fx_block_t>(block), true);
        std::array<float, 4096> audio{};
        std::array<float, 64> right{};
        for (unsigned i = 0; i < audio.size(); ++i)
            audio[i] = .25f * std::sin(float(i) * .13f);
        for (unsigned i = 0; i < audio.size(); i += 64) {
            coyopedal_fx_process_pre(audio.data() + i, 64);
            coyopedal_fx_process_delay(audio.data() + i, 64);
            coyopedal_fx_process_reverb_stereo(audio.data() + i, right.data(), 64);
        }
        for (float sample : audio)
            assert(std::isfinite(sample));
        if (cycle == 0)
            reference = audio;
        else
            assert(reference == audio);
        coyopedal_fx_detach();
        coyopedal_fx_detach();
        for (unsigned block = 0; block < COYOPEDAL_FX_BLOCK_COUNT; ++block)
            assert(!coyopedal_fx_enabled(static_cast<coyopedal_fx_block_t>(block)));
        // The next cycle must not retain pointers into these freed allocations.
    }
}
