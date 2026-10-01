#include "../src/native/drivers/speaker_buffer.hpp"
#include <array>
#include <cassert>
#include <cmath>
int main() {
    SpeakerBuffer ring;
    std::array<std::int16_t, SpeakerBuffer::capacity> storage{};
    ring.samples = storage.data();
    std::array<std::uint64_t, SpeakerBuffer::capacity> input{};
    // Equal and opposite channels must cancel without signed overflow.
    input.fill((std::uint64_t{0x80000000} << 32) | 0x7fff0000);
    assert(ring.push(input.data(), input.size()) == SpeakerBuffer::capacity);
    assert(ring.push(input.data(), 1) == 0);
    std::array<std::int16_t, 320> output{};
    assert(ring.render(output.data(), output.size()));
    for (auto sample : output)
        assert(sample == 0);
    ring.clear();
    assert(ring.available() == 0);
    assert(!ring.render(output.data(), output.size()));
    // Continuous DC survives both wraparound and +/- 1000 ppm clock drift.
    for (unsigned count : {128U, 256U, 320U})
        for (double drift : {0.999, 1.001}) {
            const double step = count * drift;
            ring.clear();
            input.fill((std::uint64_t{0x10000000} << 32) | 0x10000000);
            ring.push(input.data(), 384);
            double pending = 0;
            for (int block = 0; block < 30000; ++block) {
                assert(ring.render(output.data(), count));
                for (unsigned i = 0; i < count; ++i)
                    assert(output[i] == 4096);
                pending += step;
                const auto frames = static_cast<std::size_t>(pending);
                pending -= frames;
                assert(ring.push(input.data(), frames) == frames);
            }
            assert(ring.available() > 200 && ring.available() < 600);
        }
}
