#include "native/drivers/uac2.hpp"
#include "native/drivers/uac2_pcm.hpp"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>

// Real configuration 1 read from an XTONE Pro (152a:8840), firmware 1.1.0.
// This checks descriptor discovery only.
int main(int argc, char** argv) {
    assert(argc == 2 || argc == 3);
    const bool fullSpeed = argc == 3;
    const unsigned slotBytes = fullSpeed ? 3 : 4;
    std::ifstream file(argv[1], std::ios::binary);
    assert(file.good());
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file), {}};
    uac2::Configuration config;
    assert(uac2::discover(bytes.data(), bytes.size(), config) == uac2::Error::None);
    auto pairs = uac2::pairs(config, 0);
    assert(pairs.size() == 2);
    const auto& p = pairs.front();
    assert(p.capture.interface == 2 && p.capture.alternate == 1 && p.capture.data.address == 0x81);
    assert(p.playback.interface == 1 && p.playback.alternate == 1 &&
           p.playback.data.address == 0x01);
    for (const auto* stream : {&p.capture, &p.playback}) {
        assert(stream->control == 0 && stream->clock == 40);
        assert(stream->format.channels == 2 && stream->format.subslot == slotBytes &&
               stream->format.bits == 24);
        assert(stream->data.maxBytes == (fullSpeed ? 294 : 200) && stream->data.ticks() == 1);
    }
    // The selector's pin is a runtime response, simulated here; no hardware
    // clock request or rate negotiation is claimed by this offline test.
    auto io = [](bool in, uint8_t request, uint8_t selector, uint8_t entity, uint8_t* value,
                 size_t size) {
        assert(in && request == 1 && selector == 1 && entity == 40 && size == 1);
        *value = 1;
        return true;
    };
    uint8_t root = 0, controls = 0;
    uint64_t numerator = 0, denominator = 0;
    assert(uac2::resolveClock(config, 0, 40, io, root, numerator, denominator, controls) ==
           uac2::Error::None);
    assert(root == 41 && controls == 7 && numerator == 1 && denominator == 1);
    uac2::Codec capture, playback;
    assert(capture.configure(p.capture.format) && playback.configure(p.playback.format));
    const int32_t samples[] = {0x12345600, -0x23456700, -0x34567800, 0x45678900};
    uint8_t pcm[16]{};
    for (unsigned i = 0; i < 4; ++i)
        capture.encode(pcm + i * slotBytes, samples[i]);
    assert(capture.capture(pcm) == samples[0]);
    assert(capture.capture(pcm + capture.format.frameBytes()) == samples[2]);
    assert(capture.capture(pcm, 1) == samples[1]);
    uint64_t stereo = uint32_t(samples[0]) | (uint64_t(uint32_t(samples[1])) << 32);
    uint8_t output[8] = {};
    playback.playback(output, &stereo, 1);
    assert(playback.capture(output, 0) == samples[0] && playback.capture(output, 1) == samples[1]);
    assert(uac2::fullSpeed48k(p) == fullSpeed);
    if (fullSpeed) {
        assert(uac2::fullSpeed48k(p, 344, 384));
        assert(!uac2::fullSpeed48k(p, 288, 384));
        assert(!uac2::fullSpeed48k(p, 344, 200));
        assert(uac2::fullSpeed48k(pairs.back(), 344, 200));
    }
    puts("XTONE recorded topology, preferred PCM format, clock selector and guitar/stereo mapping "
         "passed");
}
