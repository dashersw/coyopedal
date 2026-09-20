#pragma once
#include "uac2.hpp"
#include <string.h>
#include <cmath>
namespace uac2 {
using Decode = int32_t (*)(const uint8_t*);
using Encode = void (*)(uint8_t*, int32_t);
inline int32_t read32(const uint8_t* p) {
    int32_t v;
    memcpy(&v, p, 4);
    return v;
}
inline int32_t read24(const uint8_t* p) {
    return int32_t((uint32_t(p[0]) << 8) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 24));
}
inline int32_t read16(const uint8_t* p) {
    return int32_t(uint32_t(le16(p)) << 16);
}
inline int32_t read8(const uint8_t* p) {
    return int32_t(uint32_t(*p) << 24);
}
inline int32_t readU8(const uint8_t* p) {
    return int32_t(uint32_t(*p ^ 0x80) << 24);
}
inline int32_t readFloat(const uint8_t* p) {
    float v;
    memcpy(&v, p, 4);
    if (!std::isfinite(v))
        return 0;
    return v >= 1 ? INT32_MAX : (v <= -1 ? INT32_MIN : int32_t(double(v) * 2147483648.));
}
inline void write32(uint8_t* p, int32_t v) {
    memcpy(p, &v, 4);
}
inline void write24(uint8_t* p, int32_t v) {
    uint32_t u = uint32_t(v);
    p[0] = u >> 8;
    p[1] = u >> 16;
    p[2] = u >> 24;
}
inline void write16(uint8_t* p, int32_t v) {
    uint32_t u = uint32_t(v);
    p[0] = u >> 16;
    p[1] = u >> 24;
}
inline void write8(uint8_t* p, int32_t v) {
    *p = uint32_t(v) >> 24;
}
inline void writeU8(uint8_t* p, int32_t v) {
    *p = (uint32_t(v) >> 24) ^ 0x80;
}
inline void writeFloat(uint8_t* p, int32_t v) {
    float f = float(double(v) / 2147483648.);
    memcpy(p, &f, 4);
}
struct Codec {
    Format format;
    Decode decode = nullptr;
    Encode encode = nullptr;
    bool nativeMono = false, nativeStereo = false;
    bool configure(Format f) {
        if (!f.channels || !f.subslot || f.subslot > 4 || !f.bits || f.bits > f.subslot * 8)
            return false;
        format = f;
        if (f.encoding == Encoding::Float32) {
            if (f.subslot != 4 || f.bits != 32)
                return false;
            decode = readFloat;
            encode = writeFloat;
        } else if (f.encoding == Encoding::UnsignedPCM8) {
            if (f.subslot != 1 || f.bits != 8)
                return false;
            decode = readU8;
            encode = writeU8;
        } else {
            Decode ds[] = {read8, read16, read24, read32};
            Encode es[] = {write8, write16, write24, write32};
            decode = ds[f.subslot - 1];
            encode = es[f.subslot - 1];
        }
        nativeMono = f.encoding == Encoding::SignedPCM && f.subslot == 4 && f.channels == 1;
        nativeStereo = f.encoding == Encoding::SignedPCM && f.subslot == 4 && f.channels == 2;
        return true;
    }
    // Signed PCM in three- or four-byte slots, whatever the channel count: the
    // formats the transport completions decode and encode in their own loops
    // instead of through the per-sample function pointers above.
    bool packedPcm() const {
        return format.encoding == Encoding::SignedPCM &&
               (format.subslot == 3 || format.subslot == 4);
    }
    int32_t capture(const uint8_t* frame, unsigned channel = 0) const {
        return channel < format.channels ? decode(frame + channel * format.subslot) : 0;
    }
    void playback(uint8_t* dest, const uint64_t* frames, unsigned count) const {
        if (nativeStereo) {
            if (format.bits == 32)
                memcpy(dest, frames, size_t(count) * 8);
            else {
                uint64_t word = uint32_t(0xffffffffu << (32 - format.bits));
                const uint64_t mask = word | (word << 32);
                for (unsigned n = 0; n < count; ++n) {
                    uint64_t v = frames[n] & mask;
                    memcpy(dest + size_t(n) * 8, &v, 8);
                }
            }
            return;
        }
        for (unsigned n = 0; n < count; ++n) {
            const auto l = int32_t(uint32_t(frames[n])), r = int32_t(uint32_t(frames[n] >> 32));
            for (unsigned c = 0; c < format.channels; ++c) {
                int32_t value = c == 0 ? l : (c == 1 ? r : 0);
                if (format.channels == 1)
                    value = int32_t((int64_t(l) + r) / 2);
                if (format.encoding == Encoding::SignedPCM && format.bits < 32)
                    value = int32_t(uint32_t(value) & (0xffffffffu << (32 - format.bits)));
                encode(dest + (size_t(n) * format.channels + c) * format.subslot, value);
            }
        }
    }
};
} // namespace uac2
