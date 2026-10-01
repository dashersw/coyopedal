#include "ir_wav.hpp"
#include "cabinet_ir.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace {
std::uint32_t u32(const unsigned char* p) {
    return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 |
           std::uint32_t(p[3]) << 24;
}
unsigned u16(const unsigned char* p) {
    return p[0] | unsigned(p[1]) << 8;
}
} // namespace
bool pedalboard_ir_read_wav(FILE* file, float* taps, unsigned& count, char* error,
                            std::size_t capacity) {
    auto fail = [&](const char* reason) {
        if (error && capacity)
            std::snprintf(error, capacity, "%s", reason);
        return false;
    };
    if (!file || !taps)
        return fail("No IR file");
    unsigned char header[12];
    if (std::fread(header, 1, 12, file) != 12 || std::memcmp(header, "RIFF", 4) ||
        std::memcmp(header + 8, "WAVE", 4))
        return fail("IR is not a RIFF WAV");
    const std::uint64_t end = std::uint64_t(u32(header + 4)) + 8;
    unsigned format = 0, channels = 0, bits = 0, align = 0;
    std::uint32_t rate = 0, bytes = 0;
    long data_offset = 0;
    bool have_format = false;
    for (std::uint64_t pos = 12; pos + 8 <= end;) {
        unsigned char chunk[8];
        if (std::fread(chunk, 1, 8, file) != 8)
            return fail("Truncated IR header");
        const auto size = u32(chunk + 4);
        if (pos + 8 + size > end)
            return fail("Invalid IR chunk size");
        if (!std::memcmp(chunk, "fmt ", 4)) {
            unsigned char fmt[16];
            if (size < 16 || std::fread(fmt, 1, 16, file) != 16)
                return fail("Invalid IR format");
            format = u16(fmt);
            channels = u16(fmt + 2);
            rate = u32(fmt + 4);
            align = u16(fmt + 12);
            bits = u16(fmt + 14);
            have_format = true;
        } else if (!std::memcmp(chunk, "data", 4) && !data_offset) {
            data_offset = static_cast<long>(pos + 8);
            bytes = size;
        }
        pos += 8 + std::uint64_t(size) + (size & 1);
        if (pos > end || pos > 0x7fffffff || std::fseek(file, static_cast<long>(pos), SEEK_SET))
            return fail("Invalid IR chunk offset");
        if (have_format && data_offset)
            break;
    }
    if (!have_format || !data_offset || !bytes)
        return fail("IR has no format or samples");
    if (rate != 48000)
        return fail("IR must be 48 kHz");
    if (channels != 1 && channels != 2)
        return fail("IR must be mono or stereo");
    if (!((format == 1 && (bits == 16 || bits == 24 || bits == 32)) || (format == 3 && bits == 32)))
        return fail("IR needs PCM16/24/32 or float32");
    if (align != channels * (bits / 8) || bytes % align)
        return fail("Invalid IR sample layout");
    // Check the declared data extent even when a long IR is truncated to 1024 taps.
    if (std::fseek(file, 0, SEEK_END))
        return fail("IR seek failed");
    const long file_bytes = std::ftell(file);
    if (file_bytes < 0 || std::uint64_t(file_bytes) < end ||
        std::uint64_t(data_offset) + bytes > std::uint64_t(file_bytes))
        return fail("Truncated IR samples");
    if (std::fseek(file, data_offset, SEEK_SET))
        return fail("IR seek failed");
    const unsigned frames = std::min<unsigned>(bytes / align, coyopedal::pedal::CabinetIR::kTaps);
    for (unsigned i = 0; i < frames; ++i) {
        unsigned char frame[8];
        if (std::fread(frame, 1, align, file) != align)
            return fail("IR read failed");
        float sample = 0;
        for (unsigned ch = 0; ch < channels; ++ch) {
            const auto* p = frame + ch * (bits / 8);
            float value;
            if (format == 3) {
                const auto word = u32(p);
                std::memcpy(&value, &word, 4);
            } else {
                std::uint32_t word = bits == 16 ? u16(p)
                                     : bits == 24
                                         ? (std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 |
                                            std::uint32_t(p[2]) << 16)
                                         : u32(p);
                word <<= 32 - bits;
                std::int32_t signed_word;
                std::memcpy(&signed_word, &word, 4);
                value = static_cast<float>(signed_word) * (1.0F / 2147483648.0F);
            }
            if (!std::isfinite(value) || std::fabs(value) > 32.0F)
                return fail("IR contains invalid samples");
            sample += value / channels;
        }
        taps[i] = sample;
    }
    count = frames;
    return true;
}
