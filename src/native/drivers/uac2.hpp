#pragma once
#include <stdint.h>
#include <stddef.h>
#include <vector>
#include <algorithm>
#include <limits>

// USB Audio 2.0 descriptor discovery. This module has no device identities,
// OS dependencies or streaming-time allocation. All multi-byte fields are LE.
namespace uac2 {
inline uint16_t le16(const uint8_t* p) {
    return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}
inline uint32_t le32(const uint8_t* p) {
    return uint32_t(le16(p)) | (uint32_t(le16(p + 2)) << 16);
}
enum class Error { None, Truncated, Descriptor, Duplicate, Topology, NoPCM, Clock, Rate, Capacity };
inline const char* errorName(Error e) {
    switch (e) {
    case Error::None:
        return "ok";
    case Error::Truncated:
        return "truncated descriptors";
    case Error::Descriptor:
        return "invalid descriptor";
    case Error::Duplicate:
        return "duplicate descriptor identity";
    case Error::Topology:
        return "unresolved audio topology";
    case Error::NoPCM:
        return "no PCM alternate";
    case Error::Clock:
        return "clock control failed";
    case Error::Rate:
        return "requested sample rate unavailable";
    case Error::Capacity:
        return "USB packet capacity exceeded";
    }
    return "unknown";
}
enum class Encoding { SignedPCM, UnsignedPCM8, Float32 };
struct Format {
    uint8_t channels = 0, subslot = 0, bits = 0;
    Encoding encoding = Encoding::SignedPCM;
    unsigned frameBytes() const {
        return unsigned(channels) * subslot;
    }
};
struct Endpoint {
    uint8_t address = 0, interval = 0, sync = 0, usage = 0;
    uint32_t maxBytes = 0;
    unsigned ticks() const {
        return interval ? 1u << (interval - 1) : 0;
    }
};
struct Stream {
    uint8_t interface = 0, alternate = 0, control = 0, terminal = 0, clock = 0;
    Format format;
    Endpoint data, feedback;
    uint8_t expectedEndpoints = 0, seenEndpoints = 0;
    bool general = false, typed = false, pcm = false;
    bool input() const {
        return data.address & 0x80;
    }
};
struct Entity {
    uint8_t control = 0, id = 0, type = 0, controls = 0, attributes = 0;
    std::vector<uint8_t> sources;
};
struct Terminal {
    uint8_t control, id, clock, type, channels;
    uint16_t kind;
};
// A UAC2 Feature Unit carries the mute and volume controls for one path. The
// host is expected to write them: a class-compliant interface may come up muted
// or at minimum gain, which is heard as an interface that streams its own noise
// floor and nothing else.
struct Feature {
    uint8_t control = 0, id = 0, source = 0, channels = 0;
};
struct Function {
    uint8_t first, count, control;
    bool header = false;
};
struct Configuration {
    uint8_t value = 0;
    std::vector<Stream> streams;
    std::vector<Entity> clocks;
    std::vector<Terminal> terminals;
    std::vector<Feature> features;
    std::vector<Function> functions;
};
inline Error discover(const uint8_t* b, size_t size, Configuration& result) {
    result = {};
    if (!b || size < 9 || b[0] != 9 || b[1] != 2 || le16(b + 2) != size || !b[5])
        return Error::Truncated;
    Configuration c;
    c.value = b[5];
    struct Association {
        uint8_t first, count;
    };
    std::vector<Association> associations;
    // Pass 1 establishes function boundaries even when descriptor order differs.
    std::vector<uint16_t> alternates;
    for (size_t off = 9; off < size;) {
        if (size - off < 2 || b[off] < 2 || b[off] > size - off)
            return Error::Truncated;
        const auto* d = b + off;
        unsigned n = d[0];
        if (d[1] == 11) {
            if (n < 8 || !d[3] || unsigned(d[2]) + d[3] > 256)
                return Error::Descriptor;
            if (d[4] == 1 && d[6] == 0x20)
                associations.push_back({d[2], d[3]});
        }
        if (d[1] == 4) {
            if (n < 9)
                return Error::Descriptor;
            uint16_t key = uint16_t(d[2]) * 256 + d[3];
            if (std::find(alternates.begin(), alternates.end(), key) != alternates.end())
                return Error::Duplicate;
            alternates.push_back(key);
            if (d[5] == 1 && d[6] == 1 && d[7] == 0x20 && d[3] == 0)
                c.functions.push_back({d[2], 1, d[2], false});
        }
        off += n;
    }
    for (auto& f : c.functions)
        for (const auto& a : associations)
            if (f.control >= a.first && unsigned(f.control) < unsigned(a.first) + a.count) {
                f.first = a.first;
                f.count = a.count;
            }
    if (c.functions.empty())
        return Error::NoPCM;
    auto functionFor = [&](uint8_t iface) -> Function* {
        Function* found = nullptr;
        for (auto& f : c.functions)
            if (iface >= f.first && unsigned(iface) < unsigned(f.first) + f.count) {
                if (found)
                    return nullptr;
                found = &f;
            }
        if (!found && associations.empty() && c.functions.size() == 1)
            found = &c.functions[0];
        return found;
    };
    int stream = -1;
    int ac = -1;
    bool audio = false;
    for (size_t off = 9; off < size;) {
        const auto* d = b + off;
        unsigned n = d[0];
        if (d[1] == 4) {
            stream = ac = -1;
            audio = d[5] == 1 && d[7] == 0x20;
            if (audio && d[6] == 1 && d[3] == 0)
                ac = d[2];
            else if (audio && d[6] == 2 && d[3] != 0) {
                auto* f = functionFor(d[2]);
                if (!f)
                    return Error::Topology;
                Stream s;
                s.interface = d[2];
                s.alternate = d[3];
                s.control = f->control;
                s.expectedEndpoints = d[4];
                c.streams.push_back(s);
                stream = int(c.streams.size() - 1);
            }
        } else if (d[1] == 0x24 && audio) {
            if (n < 3)
                return Error::Descriptor;
            if (ac >= 0) {
                if (d[2] == 1) {
                    if (n < 9 || le16(d + 3) != 0x0200)
                        return Error::Descriptor;
                    for (auto& f : c.functions)
                        if (f.control == ac) {
                            if (f.header)
                                return Error::Duplicate;
                            f.header = true;
                        }
                } else if (d[2] == 2 || d[2] == 3) {
                    bool in = d[2] == 2;
                    if (n < (in ? 17u : 12u) || !d[3] || !d[in ? 7 : 8])
                        return Error::Descriptor;
                    c.terminals.push_back({uint8_t(ac), d[3], d[in ? 7 : 8], d[2],
                                           uint8_t(in ? d[8] : 0), le16(d + 4)});
                } else if (d[2] == 6) {
                    // FEATURE_UNIT: bUnitID, bSourceID, then one 4-byte
                    // bmaControls per channel plus one for master.
                    if (n < 10 || !d[3] || (n - 6u) % 4u != 0u) {
                        return Error::Descriptor;
                    }
                    const unsigned entries = (n - 6u) / 4u;
                    c.features.push_back(
                        {uint8_t(ac), d[3], d[4], uint8_t(entries > 0 ? entries - 1 : 0)});
                } else if (d[2] >= 10 && d[2] <= 12) {
                    if (n < (d[2] == 10 ? 8u : 7u) || !d[3])
                        return Error::Descriptor;
                    Entity e;
                    e.control = uint8_t(ac);
                    e.id = d[3];
                    e.type = d[2];
                    if (d[2] == 10) {
                        e.attributes = d[4];
                        e.controls = d[5];
                    }
                    if (d[2] == 11) {
                        if (!d[4] || n < unsigned(7 + d[4]))
                            return Error::Descriptor;
                        e.sources.assign(d + 5, d + 5 + d[4]);
                        e.controls = d[5 + d[4]];
                    }
                    if (d[2] == 12) {
                        e.sources.push_back(d[4]);
                        e.controls = d[5];
                    }
                    c.clocks.push_back(e);
                }
            } else if (stream >= 0) {
                auto& s = c.streams[stream];
                if (d[2] == 1) {
                    if (n < 16 || s.general)
                        return Error::Descriptor;
                    s.general = true;
                    s.terminal = d[3];
                    s.format.channels = d[10];
                    uint32_t formats = le32(d + 6);
                    if (d[5] == 1 && (formats & 7)) {
                        s.pcm = true;
                        s.format.encoding =
                            (formats & 1)
                                ? Encoding::SignedPCM
                                : ((formats & 4) ? Encoding::Float32 : Encoding::UnsignedPCM8);
                    }
                } else if (d[2] == 2) {
                    if (n < 6 || s.typed)
                        return Error::Descriptor;
                    s.typed = true;
                    s.format.subslot = d[4];
                    s.format.bits = d[5];
                    if (d[3] != 1)
                        s.pcm = false;
                }
            }
        } else if (d[1] == 5 && stream >= 0) {
            if (n < 7)
                return Error::Descriptor;
            auto& s = c.streams[stream];
            ++s.seenEndpoints;
            if ((d[3] & 3) == 1) {
                uint16_t w = le16(d + 4);
                unsigned transactions = 1 + ((w >> 11) & 3);
                if (!(d[2] & 15) || (d[2] & 0x70) || !d[6] || d[6] > 16 || transactions > 3 ||
                    (w & 0xe000))
                    return Error::Descriptor;
                Endpoint ep{d[2], d[6], uint8_t((d[3] >> 2) & 3), uint8_t((d[3] >> 4) & 3),
                            uint32_t((w & 2047) * transactions)};
                if (!ep.maxBytes)
                    return Error::Descriptor;
                if (ep.usage == 1) {
                    if (s.feedback.address || !(ep.address & 0x80))
                        return Error::Descriptor;
                    s.feedback = ep;
                } else if (ep.usage == 0 || ep.usage == 2) {
                    if (s.data.address)
                        return Error::Descriptor;
                    s.data = ep;
                } else
                    return Error::Descriptor;
            }
        }
        off += n;
    }
    for (const auto& f : c.functions)
        if (!f.header)
            return Error::Topology;
    for (size_t i = 0; i < c.clocks.size(); ++i) {
        const auto& e = c.clocks[i];
        for (size_t j = 0; j < i; ++j)
            if (e.control == c.clocks[j].control && e.id == c.clocks[j].id)
                return Error::Duplicate;
        for (const auto& t : c.terminals)
            if (e.control == t.control && e.id == t.id)
                return Error::Duplicate;
    }
    for (size_t i = 0; i < c.terminals.size(); ++i)
        for (size_t j = 0; j < i; ++j)
            if (c.terminals[i].control == c.terminals[j].control &&
                c.terminals[i].id == c.terminals[j].id)
                return Error::Duplicate;
    std::vector<Stream> usable;
    for (auto s : c.streams) {
        if (!s.pcm)
            continue;
        if (!s.general || !s.typed || !s.data.address || s.seenEndpoints != s.expectedEndpoints)
            return Error::Descriptor;
        const auto& f = s.format;
        if (!f.channels || !f.subslot || f.subslot > 4 || !f.bits || f.bits > f.subslot * 8)
            return Error::Descriptor;
        if ((f.encoding == Encoding::UnsignedPCM8 && (f.bits != 8 || f.subslot != 1)) ||
            (f.encoding == Encoding::Float32 && (f.bits != 32 || f.subslot != 4)))
            continue;
        bool linked = false;
        for (const auto& t : c.terminals)
            if (t.control == s.control && t.id == s.terminal) {
                if (t.kind != 0x0101 || (s.input() ? t.type != 3 : t.type != 2))
                    return Error::Topology;
                s.clock = t.clock;
                linked = true;
            }
        if (!linked)
            return Error::Topology;
        if (s.input() && s.feedback.address)
            return Error::Topology;
        if (s.data.maxBytes < f.frameBytes())
            return Error::Capacity;
        usable.push_back(s);
    }
    c.streams = std::move(usable);
    if (c.streams.empty())
        return Error::NoPCM;
    result = std::move(c);
    return Error::None;
}
struct Pair {
    Stream capture, playback;
};
inline std::vector<Pair> pairs(const Configuration& c, int control = -1) {
    std::vector<Pair> out;
    for (const auto& i : c.streams)
        if (i.input() && (control < 0 || i.control == control))
            for (const auto& o : c.streams)
                if (!o.input() && o.control == i.control && o.interface != i.interface)
                    out.push_back({i, o});
    // Prefer a shared clock, short service intervals, few channels and 4-byte
    // containers for the pedal's mono-in, stereo-out graph. This is
    // descriptor-based, not a device-specific fast path.
    auto score = [](const Pair& p) {
        return (p.capture.clock == p.playback.clock ? 0 : 100000) +
               int(p.capture.data.ticks() + p.playback.data.ticks()) * 1000 +
               int(p.capture.format.channels + std::max(2, int(p.playback.format.channels)) - 3) *
                   100 +
               (p.capture.format.subslot == 4 ? 0 : 10) + (p.playback.format.subslot == 4 ? 0 : 10);
    };
    std::stable_sort(out.begin(), out.end(),
                     [&](const Pair& a, const Pair& b) { return score(a) < score(b); });
    return out;
}
struct Range {
    uint32_t min, max, step;
};
inline Error ranges(const uint8_t* b, size_t n, std::vector<Range>& out) {
    out.clear();
    if (!b || n < 2)
        return Error::Truncated;
    unsigned count = le16(b);
    if (!count || n != 2 + size_t(count) * 12)
        return Error::Descriptor;
    for (unsigned i = 0; i < count; ++i) {
        const auto* p = b + 2 + i * 12;
        Range r{le32(p), le32(p + 4), le32(p + 8)};
        if (!r.min || r.min > r.max || (!r.step && r.min != r.max))
            return Error::Rate;
        out.push_back(r);
    }
    return Error::None;
}
inline bool contains(const std::vector<Range>& rs, uint32_t rate) {
    for (const auto& r : rs)
        if (rate >= r.min && rate <= r.max && (!r.step || (rate - r.min) % r.step == 0))
            return true;
    return false;
}
// Clock I/O is injected, allowing the exact discovery/control code to be tested
// without a device. Call signature: (in, request, selector, entity, bytes, size).
template <class IO>
Error resolveClock(const Configuration& c, uint8_t ac, uint8_t id, IO& io, uint8_t& root,
                   uint64_t& numerator, uint64_t& denominator, uint8_t& controls) {
    bool seen[256] = {};
    numerator = denominator = 1;
    for (unsigned depth = 0; depth < 256; ++depth) {
        if (!id || seen[id])
            return Error::Topology;
        seen[id] = true;
        const Entity* e = nullptr;
        for (const auto& x : c.clocks)
            if (x.control == ac && x.id == id)
                e = &x;
        if (!e)
            return Error::Topology;
        if (e->type == 10) {
            root = id;
            controls = e->controls;
            return Error::None;
        }
        if (e->type == 11) {
            uint8_t pin = 1;
            if ((e->controls & 1) && !io(true, 1, 1, id, &pin, 1))
                return Error::Clock;
            if (!pin || pin > e->sources.size())
                return Error::Clock;
            id = e->sources[pin - 1];
        } else {
            uint8_t n[2] = {}, d[2] = {};
            if (!io(true, 1, 1, id, n, 2) || !io(true, 1, 2, id, d, 2) || !le16(n) || !le16(d))
                return Error::Clock;
            if (numerator > UINT64_MAX / le16(n) || denominator > UINT64_MAX / le16(d))
                return Error::Clock;
            numerator *= le16(n);
            denominator *= le16(d);
            id = e->sources[0];
        }
    }
    return Error::Topology;
}
template <class IO> inline Error validClock(IO& io, uint8_t root, uint8_t controls) {
    if (!(controls & 4))
        return Error::None;
    uint8_t valid = 0;
    return io(true, 1, 2, root, &valid, 1) && valid == 1 ? Error::None : Error::Clock;
}
// This S3 transport runs the graph at 48 kHz. Refuse other rates instead of
// reinterpreting samples. Clock multipliers are resolved with checked 64-bit math.
template <class IO>
inline Error setRate48k(IO& io, uint8_t root, uint8_t controls, uint64_t numerator,
                        uint64_t denominator) {
    if (!numerator || !denominator || denominator > UINT64_MAX / 48000U)
        return Error::Rate;
    const uint64_t scaled = 48000U * denominator;
    if (scaled % numerator || scaled / numerator > UINT32_MAX)
        return Error::Rate;
    const uint32_t desired = uint32_t(scaled / numerator);
    uint8_t cur[4]{};
    if (!(controls & 1) || !io(true, 1, 1, root, cur, 4))
        return Error::Clock;
    if (le32(cur) != desired) {
        if ((controls & 3) != 3)
            return Error::Rate;
        uint8_t count[2]{};
        if (!io(true, 2, 1, root, count, 2))
            return Error::Clock;
        if (!le16(count) || le16(count) > 128)
            return Error::Rate;
        std::vector<uint8_t> raw(2 + size_t(le16(count)) * 12);
        if (!io(true, 2, 1, root, raw.data(), raw.size()))
            return Error::Clock;
        std::vector<Range> rs;
        const auto error = ranges(raw.data(), raw.size(), rs);
        if (error != Error::None)
            return error;
        if (!contains(rs, desired))
            return Error::Rate;
    }
    if ((controls & 3) == 3) {
        for (unsigned i = 0; i < 4; ++i)
            cur[i] = uint8_t(desired >> (i * 8));
        if (!io(false, 1, 1, root, cur, 4))
            return Error::Clock;
    }
    if (!io(true, 1, 1, root, cur, 4) || le32(cur) != desired)
        return Error::Clock;
    return validClock(io, root, controls);
}

// Limits of the S3's fixed 1 ms service loop, independent of device identity.
inline bool fullSpeed48k(const Pair& p, unsigned inputLimit = 504, unsigned outputLimit = 448) {
    for (const auto* s : {&p.capture, &p.playback}) {
        if (s->data.interval != 1 || s->data.maxBytes > (s->input() ? inputLimit : outputLimit) ||
            s->data.maxBytes < 48 * s->format.frameBytes())
            return false;
    }
    if (p.capture.data.maxBytes / p.capture.format.frameBytes() > 96)
        return false;
    const auto& fb = p.playback.feedback;
    return !fb.address ||
           (fb.interval >= 1 && fb.interval <= 4 && (fb.maxBytes == 3 || fb.maxBytes == 4));
}
} // namespace uac2
