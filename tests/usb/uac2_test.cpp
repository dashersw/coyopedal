#include "native/drivers/uac2.hpp"
#include "native/drivers/uac2_pcm.hpp"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <cstring>
using namespace uac2;
std::vector<uint8_t> load(const char* path) {
    std::ifstream f(path, std::ios::binary);
    assert(f.good());
    return {(std::istreambuf_iterator<char>(f)), {}};
}
int main(int argc, char** argv) {
    assert(argc == 2);
    auto live = load(argv[1]);
    Configuration c;
    assert(discover(live.data(), live.size(), c) == Error::None);
    auto ps = pairs(c);
    assert(ps.size() == 1);
    auto p = ps[0];
    assert(p.capture.interface == 3 && p.playback.interface == 4 && p.capture.clock == 7 &&
           p.playback.clock == 7);
    assert(p.capture.data.address == 0x84 && p.playback.data.address == 3 &&
           p.playback.feedback.address == 0x83);
    assert(p.capture.format.channels == 1 && p.playback.format.channels == 2 &&
           p.capture.format.bits == 24);
    // Renumber every interface, terminal, clock and endpoint without changing
    // topology. No part of generic discovery may depend on HD X's identities.
    auto moved = live;
    for (size_t off = 0; off < moved.size(); off += moved[off]) {
        auto* d = moved.data() + off;
        if (d[1] == 11)
            d[2] += 10;
        if (d[1] == 4)
            d[2] += 10;
        if (d[1] == 5)
            d[2] = uint8_t((d[2] & 0x80) | ((d[2] + 1) & 15));
        if (d[1] == 0x24 && off >= 92) {
            if (d[2] == 10)
                d[3] += 20;
            if (d[2] == 2 && d[0] == 17) {
                d[3] += 20;
                d[7] += 20;
            }
            if (d[2] == 3 && d[0] == 12) {
                d[3] += 20;
                d[7] += 20;
                d[8] += 20;
            }
            if (d[2] == 6) {
                d[3] += 20;
                d[4] += 20;
            }
            if (d[2] == 1 && d[0] == 16)
                d[3] += 20;
        }
    }
    assert(discover(moved.data(), moved.size(), c) == Error::None);
    ps = pairs(c);
    assert(ps[0].capture.interface == 13 && ps[0].capture.clock == 27 &&
           ps[0].capture.data.address == 0x85);
    for (size_t n = 0; n < live.size(); ++n)
        assert(discover(live.data(), n, c) != Error::None);
    for (size_t off = 0; off < live.size(); off += live[off]) {
        auto b = live;
        b[off] = 0;
        assert(discover(b.data(), b.size(), c) != Error::None);
    }
    auto corrupt = live;
    corrupt[224 + 3] = 99;
    assert(discover(corrupt.data(), corrupt.size(), c) == Error::Topology);
    // Same topology with packed16, packed24, multichannel and float alternates.
    for (unsigned slot : {2u, 3u, 4u})
        for (unsigned channels : {1u, 2u, 8u}) {
            auto b = live;
            b[240 + 4] = slot;
            b[240 + 5] = slot * 8;
            b[224 + 10] = channels;
            assert(discover(b.data(), b.size(), c) == Error::None);
            assert(pairs(c)[0].capture.format.subslot == slot);
        }
    Codec codec;
    const int32_t values[] = {INT32_MIN, -123456789, 0, 123456789, INT32_MAX};
    for (unsigned slot = 1; slot <= 4; ++slot) {
        assert(codec.configure({1, uint8_t(slot), uint8_t(slot * 8), Encoding::SignedPCM}));
        for (int32_t v : values) {
            uint8_t b[4] = {};
            codec.encode(b, v);
            uint32_t mask = slot == 4 ? 0xffffffffu : (0xffffffffu << (32 - slot * 8));
            assert(uint32_t(codec.decode(b)) == (uint32_t(v) & mask));
        }
    }
    assert(codec.configure({1, 1, 8, Encoding::UnsignedPCM8}));
    uint8_t b[32] = {};
    codec.encode(b, 0);
    assert(*b == 128 && codec.decode(b) == 0);
    assert(codec.configure({1, 4, 32, Encoding::Float32}));
    float nan = std::numeric_limits<float>::quiet_NaN();
    memcpy(b, &nan, 4);
    assert(codec.decode(b) == 0);
    assert(codec.configure({4, 3, 24, Encoding::SignedPCM}));
    uint64_t frame = (uint64_t(uint32_t(-1073741824)) << 32) | 1073741824;
    codec.playback(b, &frame, 1);
    assert(read24(b) == 1073741824 && read24(b + 3) == -1073741824 && read24(b + 6) == 0 &&
           read24(b + 9) == 0);
    assert(discover(live.data(), live.size(), c) == Error::None);
    c.clocks.push_back({2, 8, 11, 1, 0, {7}});
    c.clocks.push_back({2, 9, 12, 5, 0, {8}});
    auto io = [](bool, uint8_t, uint8_t selector, uint8_t entity, uint8_t* bytes, size_t count) {
        memset(bytes, 0, count);
        bytes[0] = entity == 8 ? 1 : (selector == 1 ? 2 : 1);
        return true;
    };
    uint8_t root = 0, controls = 0;
    uint64_t numerator = 0, denominator = 0;
    assert(resolveClock(c, 2, 9, io, root, numerator, denominator, controls) == Error::None &&
           root == 7 && numerator == 2 && denominator == 1);
    c.clocks[1].sources = {9};
    assert(resolveClock(c, 2, 9, io, root, numerator, denominator, controls) == Error::Topology);
    uint32_t frequency = 96000;
    unsigned sets = 0, rangeReads = 0;
    bool badReadback = false;
    auto rateIO = [&](bool in, uint8_t request, uint8_t selector, uint8_t entity, uint8_t* bytes,
                      size_t count) {
        assert(selector == 1 && entity == 7);
        if (request == 2) {
            ++rangeReads;
            assert(in);
            std::fill_n(bytes, count, 0);
            bytes[0] = 1;
            if (count == 14) {
                uint32_t f = 48000;
                memcpy(bytes + 2, &f, 4);
                memcpy(bytes + 6, &f, 4);
            } else
                assert(count == 2);
        } else {
            assert(request == 1 && count == 4);
            if (in) {
                uint32_t f = badReadback ? frequency + 1 : frequency;
                memcpy(bytes, &f, 4);
            } else {
                frequency = le32(bytes);
                ++sets;
            }
        }
        return true;
    };
    assert(setRate48k(rateIO, 7, 3, 1, 1) == Error::None && frequency == 48000 && sets == 1 &&
           rangeReads == 2);
    sets = rangeReads = 0;
    assert(setRate48k(rateIO, 7, 3, 1, 1) == Error::None && sets == 1 && rangeReads == 0);
    frequency = 96000;
    sets = 0;
    assert(setRate48k(rateIO, 7, 1, 1, 1) == Error::Rate && sets == 0);
    frequency = 48000;
    badReadback = true;
    assert(setRate48k(rateIO, 7, 3, 1, 1) == Error::Clock);
    bool valid = true;
    unsigned validityReads = 0;
    auto validityIO = [&](bool in, uint8_t request, uint8_t selector, uint8_t entity,
                          uint8_t* bytes, size_t count) {
        assert(in && request == 1 && selector == 2 && entity == 7 && count == 1);
        *bytes = valid;
        validityReads++;
        return true;
    };
    assert(validClock(validityIO, 7, 3) == Error::None && validityReads == 0);
    assert(validClock(validityIO, 7, 7) == Error::None && validityReads == 1);
    valid = false;
    assert(validClock(validityIO, 7, 7) == Error::Clock);
    assert(setRate48k(rateIO, 7, 3, 0, 1) == Error::Rate);
    assert(setRate48k(rateIO, 7, 3, 1, UINT64_MAX) == Error::Rate);
    p.capture.data.maxBytes = 156;
    p.playback.data.maxBytes = 104;
    assert(!fullSpeed48k(p)); // Undersized full-speed descriptors must never stream.
    p.capture.data.maxBytes = 192;
    p.playback.data.maxBytes = 384;
    assert(fullSpeed48k(p));
    p.playback.data.interval = 2;
    assert(!fullSpeed48k(p));
    p.playback.data.interval = 1;
    p.capture.data.maxBytes = 191;
    assert(!fullSpeed48k(p));
    puts("Generic UAC2: live HD X topology, renumbering, truncations, PCM conversion, channel "
         "mapping, 48 kHz negotiation and clock traversal passed");
}
