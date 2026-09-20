#include "nam_a2_full_s3_native.hpp"
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>
using Model = nam_bfp::A2FullS3Native;
int main(int argc, char** argv) {
    assert(argc == 2);
    std::ifstream file(argv[1], std::ios::binary);
    std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(file)), {});
    assert(data.size() == Model::kFileSize);
    data.resize(Model::kPreparedFileSize);
    auto model = std::make_unique<Model>();
    char error[128]{};
    assert(model->load_namb(data.data(), Model::kFileSize, error, sizeof error));
    assert(model->export_precision(data.data() + Model::kFileSize, Model::kPreparedTrailerSize));
    assert(Model::validate_prepared(data.data(), data.size()));
    std::array<std::uint8_t, Model::kPreparedTrailerSize> sweep_only{};
    assert(Model::prepare_sweep(data.data(), Model::kFileSize, sweep_only.data(), error,
                                sizeof error));
    assert(!std::memcmp(sweep_only.data(), data.data() + Model::kFileSize, sweep_only.size()));
    std::array<float, 4096> original{}, restored{};
    for (unsigned i = 0; i < original.size(); ++i)
        original[i] = restored[i] =
            .25f * std::sin(float(i) * .13f) + .17f * std::cos(float(i) * .053f);
    for (unsigned i = 0; i < original.size(); i += 64)
        model->process(original.data() + i, 64);
    assert(model->load_namb(data.data(), data.size(), error, sizeof error));
    std::array<std::uint8_t, Model::kPreparedTrailerSize> roundtrip{};
    assert(model->export_precision(roundtrip.data(), roundtrip.size()));
    assert(!std::memcmp(roundtrip.data(), data.data() + Model::kFileSize, roundtrip.size()));
    for (unsigned i = 0; i < restored.size(); i += 64)
        model->process(restored.data() + i, 64);
    assert(!std::memcmp(original.data(), restored.data(), sizeof original));
    for (auto offset : {16U, 100U, unsigned(Model::kFileSize), unsigned(Model::kFileSize + 4),
                        unsigned(Model::kFileSize + 30)}) {
        data[offset] ^= 1;
        assert(!model->load_namb(data.data(), data.size(), error, sizeof error));
        data[offset] ^= 1;
    }
    assert(!model->load_namb(data.data(), data.size() - 1, error, sizeof error));
    // Maintenance must free every native arena and permit the same object to
    // load again. Repeated unload is safe, and reloaded audio stays bit exact.
    for (unsigned cycle = 0; cycle < 3; ++cycle) {
        model->release();
        model->release();
        assert(!model->loaded());
        assert(model->load_namb(data.data(), data.size(), error, sizeof error));
        for (unsigned i = 0; i < restored.size(); ++i)
            restored[i] = .25f * std::sin(float(i) * .13f) + .17f * std::cos(float(i) * .053f);
        for (unsigned i = 0; i < restored.size(); i += 64)
            model->process(restored.data() + i, 64);
        assert(!std::memcmp(original.data(), restored.data(), sizeof original));
    }
    std::puts("PASS: sweep/prepared audio bit exact; export stable; model/mask/parameter "
              "corruption and truncation rejected");
}
