#pragma once

// The compact reverb's state is owned by the target, which attaches it before
// coyopedal_fx_init. The host tests stand in for the target with plain heap blocks
// that live for the whole process.
#include "audio/effects.h"
#include <cassert>
#include <cstdlib>

inline void attach_reverb_storage() {
    const auto allocate = [](std::size_t bytes) {
        void* const memory = std::aligned_alloc(16, (bytes + 15) & ~std::size_t(15));
        assert(memory != nullptr);
        return memory;
    };
    const std::size_t state = coyopedal_fx_reverb_state_size();
    [[maybe_unused]] bool attached = coyopedal_fx_reverb_attach(allocate(state), state);
    assert(attached);
    const std::size_t predelay = coyopedal_fx_reverb_predelay_state_size();
    attached = coyopedal_fx_reverb_predelay_attach(allocate(predelay), predelay);
    assert(attached);
    for (std::size_t line = 0; line < coyopedal_fx_reverb_line_count(); ++line) {
        const std::size_t bytes = coyopedal_fx_reverb_line_state_size(line);
        attached = coyopedal_fx_reverb_line_attach(line, allocate(bytes), bytes);
        assert(attached);
    }
}
