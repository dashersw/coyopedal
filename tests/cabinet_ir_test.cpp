#include "audio/cabinet_ir.hpp"
#include "audio/ir_wav.hpp"
#include <array>
#include <algorithm>
#include <string>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>
#include <thread>

using coyopedal::pedal::CabinetIR;

static void concurrent_pipeline() {
    // Match the device's two cores and three transport slots. Serial begin/
    // finish tests cannot detect a shared forward/inverse FFT workspace.
    CabinetIR cabinet;
    std::array<float, 1024> taps{};
    for (unsigned i = 0; i < taps.size(); ++i)
        taps[i] = 0.005F * std::sin(i * 0.19F);
    assert(cabinet.load(taps.data(), taps.size()));
    constexpr unsigned blocks = 256;
    std::vector<float> input(blocks * 64), expected(input.size());
    for (unsigned n = 0; n < input.size(); ++n) {
        input[n] = 0.1F * std::sin(n * 0.371F);
        double sum = 0;
        for (unsigned k = 0; k < taps.size() && k <= n; ++k)
            sum += double(taps[k]) * input[n - k];
        expected[n] = sum * 0.5;
    }
    struct Slot {
        std::array<float, 64> samples{}, imaginary{};
        unsigned cursor{};
    };
    std::array<Slot, 3> slots{};
    std::atomic<unsigned> produced{}, consumed{};
    std::thread stage_b([&] {
        for (unsigned b = 0; b < blocks; ++b) {
            while (b - consumed.load(std::memory_order_acquire) == slots.size())
                std::this_thread::yield();
            auto& slot = slots[b % slots.size()];
            std::copy_n(input.data() + b * 64, 64, slot.samples.data());
            slot.cursor = cabinet.begin(slot.samples.data(), slot.imaginary.data());
            produced.store(b + 1, std::memory_order_release);
        }
    });
    for (unsigned b = 0; b < blocks; ++b) {
        while (produced.load(std::memory_order_acquire) == b)
            std::this_thread::yield();
        auto& slot = slots[b % slots.size()];
        cabinet.finish(slot.samples.data(), slot.imaginary.data(), slot.cursor, 0.5F);
        for (unsigned i = 0; i < 64; ++i)
            assert(std::fabs(slot.samples[i] - expected[b * 64 + i]) < 2e-6F);
        consumed.store(b + 1, std::memory_order_release);
    }
    stage_b.join();
}

static void convolution() {
    CabinetIR cabinet;
    std::array<float, 1024> taps{};
    taps[0] = 0.75F;
    taps[63] = -0.125F;
    taps[64] = 0.25F;
    taps[1023] = 0.0625F;
    assert(cabinet.load(taps.data(), taps.size()));
    std::vector<float> input(2560);
    for (unsigned i = 0; i < input.size(); ++i)
        input[i] = 0.1F * std::sin(i * 0.371F);
    // All 16 partitions have nonzero coefficients in the direct-convolution comparison.
    for (unsigned i = 1; i < taps.size(); ++i)
        taps[i] += 0.001F * std::sin(i * 0.19F);
    assert(cabinet.load(taps.data(), taps.size()));
    for (unsigned base = 0; base < input.size(); base += 64) {
        std::array<float, 64> block{};
        std::copy_n(input.data() + base, 64, block.data());
        cabinet.process(block.data(), 0.5F);
        for (unsigned i = 0; i < 64; ++i) {
            double expected = 0;
            const unsigned n = base + i;
            for (unsigned k = 0; k < 1024 && k <= n; ++k)
                expected += double(taps[k]) * input[n - k];
            assert(std::fabs(block[i] - expected * 0.5) < 2e-6);
        }
    }
    // The transport can have three blocks in flight. New spectra must not
    // overwrite the oldest block's 1024-tap history across ring wraparound.
    cabinet.reset();
    for (unsigned base = 0; base < input.size(); base += 192) {
        std::array<std::array<float, 64>, 3> blocks{}, imaginary{};
        std::array<unsigned, 3> cursors{};
        const unsigned pending = std::min<unsigned>(3, (input.size() - base) / 64);
        for (unsigned b = 0; b < pending; ++b) {
            std::copy_n(input.data() + base + b * 64, 64, blocks[b].data());
            cursors[b] = cabinet.begin(blocks[b].data(), imaginary[b].data());
        }
        for (unsigned b = 0; b < pending; ++b) {
            cabinet.finish(blocks[b].data(), imaginary[b].data(), cursors[b], 0.5F);
            for (unsigned i = 0; i < 64; ++i) {
                double expected = 0;
                const unsigned n = base + b * 64 + i;
                for (unsigned k = 0; k < 1024 && k <= n; ++k)
                    expected += double(taps[k]) * input[n - k];
                assert(std::fabs(blocks[b][i] - expected * 0.5) < 2e-6);
            }
        }
    }
    cabinet.reset();
    std::array<float, 64> zero{};
    cabinet.process(zero.data(), 1);
    for (float value : zero)
        assert(value == 0);
    // The first output contains tap zero; a tail at sample 1023 stays at 1023.
    taps.fill(0);
    taps[0] = 1;
    taps[1023] = 0.5;
    assert(cabinet.load(taps.data(), 1024));
    for (unsigned base = 0; base < 1088; base += 64) {
        std::array<float, 64> block{};
        if (!base)
            block[0] = 1;
        cabinet.process(block.data(), 1);
        for (unsigned i = 0; i < 64; ++i) {
            const unsigned n = base + i;
            assert(std::fabs(block[i] - (n == 0 ? 1.0F : n == 1023 ? 0.5F : 0.0F)) < 2e-6);
        }
    }
    float invalid = std::numeric_limits<float>::quiet_NaN();
    assert(!cabinet.load(&invalid, 1));
    assert(!cabinet.load(taps.data(), 1025));
    assert(cabinet.loaded());
    cabinet.reset();
    zero[0] = 1;
    cabinet.process(zero.data(), 1);
    assert(std::fabs(zero[0] - 1) < 2e-6);
    cabinet.release();
    assert(!cabinet.loaded());
    zero.fill(0.123F);
    cabinet.process(zero.data(), 1);
    for (float x : zero)
        assert(x == 0.123F);
}

static void append(std::vector<unsigned char>& out, unsigned value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i)
        out.push_back(value >> (i * 8));
}
static std::vector<unsigned char> wav(unsigned bits, unsigned channels, bool floating,
                                      unsigned rate = 48000) {
    std::vector<unsigned char> result{'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E'};
    // An odd-sized unknown chunk tests RIFF padding and chunk walking.
    for (char c : std::string("JUNK"))
        result.push_back(c);
    append(result, 1, 4);
    result.push_back(99);
    result.push_back(0);
    for (char c : std::string("fmt "))
        result.push_back(c);
    append(result, 16, 4);
    append(result, floating ? 3 : 1, 2);
    append(result, channels, 2);
    append(result, rate, 4);
    append(result, rate * channels * (bits / 8), 4);
    append(result, channels * (bits / 8), 2);
    append(result, bits, 2);
    for (char c : std::string("data"))
        result.push_back(c);
    append(result, 1100 * channels * (bits / 8), 4);
    for (unsigned i = 0; i < 1100; ++i)
        for (unsigned ch = 0; ch < channels; ++ch) {
            const float value = ch ? -0.25F : 0.5F;
            unsigned word;
            if (floating)
                std::memcpy(&word, &value, 4);
            else
                word = ch ? (0xE0000000U >> (32 - bits)) : (0x40000000U >> (32 - bits));
            append(result, word, bits / 8);
        }
    const unsigned length = result.size() - 8;
    for (unsigned i = 0; i < 4; ++i)
        result[4 + i] = length >> (i * 8);
    return result;
}
static bool read(const std::vector<unsigned char>& bytes, float* taps, unsigned& count) {
    FILE* file = std::tmpfile();
    assert(file);
    assert(std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size());
    std::rewind(file);
    char error[96]{};
    const bool ok = pedalboard_ir_read_wav(file, taps, count, error, sizeof error);
    assert(ok || error[0]);
    std::fclose(file);
    return ok;
}
static void factory_cabinets() {
    for (const char* path :
         {"assets/cabinets/jester-v30-sm57.wav", "assets/cabinets/jester-dv77-sm57.wav",
          "assets/cabinets/jester-rockdriver-e606.wav"}) {
        FILE* source = std::fopen(path, "rb");
        assert(source);
        std::vector<unsigned char> bytes(3116);
        assert(std::fread(bytes.data(), 1, bytes.size(), source) == bytes.size());
        assert(std::fgetc(source) == EOF);
        std::fclose(source);
        // Firmware reads the embedded WAV through this same memory stream.
        FILE* file = fmemopen(bytes.data(), bytes.size(), "rb");
        assert(file);
        std::array<float, 1024> taps{};
        unsigned count = 0;
        char error[96]{};
        assert(pedalboard_ir_read_wav(file, taps.data(), count, error, sizeof error));
        std::fclose(file);
        assert(count == taps.size());
        double energy = 0;
        for (float value : taps) {
            assert(std::isfinite(value));
            energy += value * value;
        }
        assert(energy > 1);
        CabinetIR cabinet;
        assert(cabinet.load(taps.data(), count));
        // The split pipeline must reproduce each shipped IR, including its
        // final partition, with no additional block of latency or tail.
        for (unsigned base = 0; base < 1088; base += 64) {
            std::array<float, 64> block{}, imaginary{};
            if (!base)
                block[0] = 1;
            const unsigned cursor = cabinet.begin(block.data(), imaginary.data());
            cabinet.finish(block.data(), imaginary.data(), cursor, 1);
            for (unsigned i = 0; i < 64; ++i) {
                const unsigned n = base + i;
                assert(std::fabs(block[i] - (n < count ? taps[n] : 0)) < 2e-6F);
            }
        }
    }
}
int main() {
    factory_cabinets();
    convolution();
    concurrent_pipeline();
    std::array<float, 1024> taps{};
    unsigned count = 0;
    for (unsigned bits : {16U, 24U, 32U})
        for (unsigned channels : {1U, 2U}) {
            assert(read(wav(bits, channels, false), taps.data(), count));
            assert(count == 1024);
            for (float value : taps)
                assert(value == (channels == 1 ? 0.5F : 0.125F));
        }
    assert(read(wav(32, 2, true), taps.data(), count));
    for (float value : taps)
        assert(value == 0.125F);
    assert(!read(wav(16, 1, false, 44100), taps.data(), count));
    auto broken = wav(16, 1, false);
    broken.pop_back();
    assert(!read(broken, taps.data(), count));
    broken = wav(32, 1, true);
    const unsigned nan = 0x7fc00000;
    for (unsigned i = 0; i < 4; ++i)
        broken[54 + i] = nan >> (i * 8);
    assert(!read(broken, taps.data(), count));
    std::puts("cabinet: convolution, latency, tail, reset, replacement and WAV parsing passed");
}
