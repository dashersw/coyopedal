#include "audio/effects.h"
#include <cassert>
#include <cstring>
#include "reverb_storage.hpp"
int main() {
    attach_reverb_storage();
    coyopedal_fx_init();
    const char* names[] = {"Hard Gate", "Studio VCA", "Klon", "Spring", "Chorus", "Digital"};
    for (unsigned i = 0; i < 6; i++) {
        const auto b = static_cast<coyopedal_fx_block_t>(i);
        assert(std::strcmp(coyopedal_fx_name(b), names[i]) == 0);
        coyopedal_fx_set_enabled(b, true);
        assert(coyopedal_fx_enabled(b));
    }
    assert(coyopedal_fx_name(COYOPEDAL_FX_BLOCK_COUNT)[0] == '\0');
}
