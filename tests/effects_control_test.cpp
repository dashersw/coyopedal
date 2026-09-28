#include "audio/effects.h"
#include "reverb_storage.hpp"
#include <array>
#include <cassert>
#include <cmath>
#include <vector>

int main() {
    attach_reverb_storage();
    std::vector<float> delay(96000);
    std::vector<float> reference;
    for (unsigned run = 0; run < 2; ++run) {
        coyopedal_fx_init();
        assert(coyopedal_fx_delay_attach(delay.data(), delay.size()));
        for (unsigned block = 0; block < 512; ++block) {
            for (unsigned fx = 0; fx < COYOPEDAL_FX_BLOCK_COUNT; ++fx)
                coyopedal_fx_set_enabled(static_cast<coyopedal_fx_block_t>(fx), true);
            const unsigned mask = coyopedal_fx_enabled_mask();
            assert(mask == 63U);
            std::array<float, 64> left{}, right{};
            for (unsigned i = 0; i < 64; ++i)
                left[i] = .1F * std::sin(float(block * 64 + i) * .13F);
            coyopedal_fx_process_pre_masked(left.data(), 64, mask);
            // A UI toggle after stage A must not change this block's routing.
            if (run == 1) {
                for (unsigned fx = 0; fx < COYOPEDAL_FX_BLOCK_COUNT; ++fx)
                    coyopedal_fx_set_enabled(static_cast<coyopedal_fx_block_t>(fx), false);
                assert(coyopedal_fx_enabled_mask() == 0U);
            }
            coyopedal_fx_process_delay_masked(left.data(), 64, mask);
            coyopedal_fx_process_reverb_stereo_masked(left.data(), right.data(), 64, mask);
            for (unsigned i = 0; i < 64; ++i) {
                assert(std::isfinite(left[i]) && std::isfinite(right[i]));
                if (run == 0) {
                    reference.push_back(left[i]);
                    reference.push_back(right[i]);
                } else {
                    assert(reference[(block * 64 + i) * 2] == left[i]);
                    assert(reference[(block * 64 + i) * 2 + 1] == right[i]);
                }
            }
        }
    }
}
