#include "audio/processor.hpp"
#include <array>
#include <cassert>
#include <cmath>
#include <fstream>
#include <iterator>
#include <vector>

int main(int argc, char** argv) {
    assert(argc == 2);
    std::ifstream file(argv[1], std::ios::binary);
    std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(file)), {});
    coyopedal::pedal::Processor processor;
    char error[128]{};
    assert(processor.load_namb(data.data(), data.size(), error, sizeof error));
    std::vector<float> expected;
    for (unsigned run = 0; run < 2; ++run) {
        processor.reset();
        coyopedal::pedal::Processor::BlockScratch scratch{};
        for (unsigned block = 0; block < 256; ++block) {
            const float bass = block < 128 ? 0.0F : 3.0F;
            processor.set_controls(1.1F, .7F, bass, 0.0F, 0.0F);
            std::array<float, 64> audio{};
            for (unsigned i = 0; i < 64; ++i)
                audio[i] = .1F * std::sin(float(block * 64 + i) * .137F);
            assert(processor.begin_block(audio.data(), audio.size(), scratch));
            processor.process_layers(scratch, 0, 8);
            // A later UI edit belongs to the next block, including tone and gain.
            if (run == 1)
                processor.set_controls(2.0F, .1F, -9.0F, 6.0F, 3.0F);
            processor.process_layers(scratch, 8, processor.kStagedLayerCount);
            processor.finish_block(scratch, audio.data());
            for (unsigned i = 0; i < 64; ++i) {
                if (run == 0)
                    expected.push_back(audio[i]);
                else
                    assert(audio[i] == expected[block * 64 + i]);
            }
        }
    }
}
