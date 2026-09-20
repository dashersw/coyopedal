// The A2 model shape, weight order and inference equations implemented here are
// derived from NeuralAmpModelerCore, MIT, Copyright (c) 2023 Steven Atkinson.
// See THIRD_PARTY_NOTICES.md.

// The NAM A2-Full engine for the ESP32-S3: model parsing and calibration, live
// retuning, the two-stage block pipeline, and the dispatch into the vector
// kernels in the .S files next to this one.
#include "nam_a2_full_s3_native.hpp"
#include "s3_tuning_probe.hpp"
#include "s3_tuning_calibration.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <limits>
#include <memory>
#include <new>

#if defined(ESP_PLATFORM)
#include "esp_attr.h"
#include "runtime_allocator.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_heap_caps_init.h"
#include "heap_memory_layout.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_crc.h"
#include "esp_memory_utils.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#endif

#if defined(ESP_PLATFORM)
extern "C" void s3_a2full_quantize8_narrow(const std::int32_t* input, std::int16_t* output,
                                           std::int32_t right_shift, std::int32_t frames);
extern "C" void s3_a2full_quantize8_a22_s7_s8(const std::int32_t* input, std::int16_t* high,
                                              std::int8_t* low, std::int32_t right_shift,
                                              std::int32_t frames);
#else
namespace {

// Host models of the PIE QACC arithmetic, used by the host twin of the fused
// kernels in run_layer.
inline std::int64_t host_s3_qacc40_wrap(const std::int64_t value) noexcept {
    // Every signed QACC MAC in PIE saturates its lane to signed 40-bit.
    constexpr std::int64_t minimum = -(INT64_C(1) << 39U);
    constexpr std::int64_t maximum = (INT64_C(1) << 39U) - 1U;
    return std::clamp(value, minimum, maximum);
}

inline std::int64_t host_s3_qacc40_round_extract(std::int64_t value, const int shift) noexcept {
    value = host_s3_qacc40_wrap(value);
    if (shift > 0) {
        // QACC_SHIFT_TO_I32_VECTORS adds the positive half-LSB in QACC and
        // then performs SRCMB's arithmetic shift.
        value = host_s3_qacc40_wrap(value + (INT64_C(1) << (shift - 1)));
        value >>= shift;
    } else if (shift < 0) {
        value = host_s3_qacc40_wrap(value << -shift);
    }
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(value));
}

inline std::int64_t host_s3_qacc40_trunc_extract(std::int64_t value, const int shift) noexcept {
    value = host_s3_qacc40_wrap(value);
    if (shift > 0) {
        value >>= shift;
    } else if (shift < 0) {
        value = host_s3_qacc40_wrap(value << -shift);
    }
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(value));
}

inline std::int32_t host_s3_sat_add32(const std::int32_t left, const std::int32_t right) noexcept {
    const std::int64_t sum = static_cast<std::int64_t>(left) + right;
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(sum, INT32_MIN, INT32_MAX));
}

} // namespace
#endif

namespace nam_bfp {
namespace {

constexpr std::array<std::size_t, 23> kKernelSizes{
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 15, 15, 6, 6, 6, 6, 6, 6, 6,
};
constexpr std::array<std::size_t, 23> kDilations{
    1, 3, 7, 17, 41, 101, 239, 1, 3, 7, 17, 41, 101, 239, 1, 13, 1, 3, 7, 17, 41, 101, 239,
};
constexpr std::uint32_t kS3NativeResidualNarrowMask = 0x003FF000U;
constexpr std::uint32_t kS3NativeExactResidualMask = 0;

constexpr std::size_t kHeaderSize = 32;
constexpr uint16_t kFormatVersion = 1;
constexpr uint16_t kArchitectureA2Full = 2;
constexpr uint32_t kSampleRate = 48000;
constexpr std::size_t kChannels = A2FullS3Native::kChannels;
constexpr std::size_t kLayerCount = A2FullS3Native::kLayerCount;
constexpr std::size_t kHeadKernel = A2FullS3Native::kHeadKernel;
constexpr float kLeakySlope = 0.01F;

uint16_t read_u16(const uint8_t* const data) noexcept {
    return static_cast<uint16_t>(data[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(data[1]) << 8U);
}

uint32_t read_u32(const uint8_t* const data) noexcept {
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8U) |
           (static_cast<uint32_t>(data[2]) << 16U) | (static_cast<uint32_t>(data[3]) << 24U);
}

float read_float(const uint8_t* const data) noexcept {
    return std::bit_cast<float>(read_u32(data));
}

uint32_t crc32(const uint8_t* const data, const std::size_t size) noexcept {
#if defined(ESP_PLATFORM)
    return esp_rom_crc32_le(0U, data, static_cast<std::uint32_t>(size));
#else
    static constexpr auto table = [] {
        std::array<std::uint32_t, 256> values{};
        for (std::uint32_t i = 0; i < values.size(); ++i) {
            std::uint32_t crc = i;
            for (unsigned bit = 0; bit < 8; ++bit)
                crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
            values[i] = crc;
        }
        return values;
    }();
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t i = 0; i < size; ++i)
        crc = table[(crc ^ data[i]) & 0xFFU] ^ (crc >> 8U);
    return ~crc;
#endif
}

void copy_error(const char* const message, char* const destination,
                const std::size_t capacity) noexcept {
    if (destination == nullptr || capacity == 0U) {
        return;
    }
    const std::size_t count = std::min(std::strlen(message), capacity - 1U);
    std::memcpy(destination, message, count);
    destination[count] = '\0';
}

inline float leaky(const float value) noexcept {
    // Preserve fmax's NaN handling; finite values need no library call.
    if (std::isnan(value))
        return std::fmax(value, value * kLeakySlope);
    return value >= 0.0F ? value : value * kLeakySlope;
}

// Calibration runs in float once at model load. Different FPUs can disagree
// by a few ULPs after 23 recurrent layers; preserving those ULPs in the later
// int64 "exact" constants makes an unstable high-gain recurrence diverge.
// Round every positive peak upward to a 10-bit mantissa grid. The maximum
// scale loss is below 0.1%, while host and device land on the same envelope.
inline float stable_calibration_peak(const float value) noexcept {
    if (!(value > 0.0F) || !std::isfinite(value)) {
        return value;
    }
    constexpr std::uint32_t kDiscardedMantissaBits = 13U;
    constexpr std::uint32_t kDiscardedMask = (1U << kDiscardedMantissaBits) - 1U;
    std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    if ((bits & kDiscardedMask) != 0U) {
        bits = (bits & ~kDiscardedMask) + (1U << kDiscardedMantissaBits);
    }
    return std::bit_cast<float>(bits);
}

#if defined(ESP_PLATFORM)
// Only the serialized import tuner uses this worker. Its PSRAM stack persists
// through maintenance imports and disappears on the audio-mode reboot.
struct TuningWorker {
    TaskHandle_t task = nullptr;
    SemaphoreHandle_t wake = nullptr, done = nullptr;
    void (*run)(void*) = nullptr;
    void* argument = nullptr;
};
TuningWorker tuning_worker;
void tuning_worker_task(void*) {
    for (;;) {
        xSemaphoreTake(tuning_worker.wake, portMAX_DELAY);
        tuning_worker.run(tuning_worker.argument);
        xSemaphoreGive(tuning_worker.done);
    }
}
bool ensure_tuning_worker() {
    if (tuning_worker.task)
        return true;
    constexpr std::size_t stack_bytes = 8192U;
    auto* stack = static_cast<StackType_t*>(
        heap_caps_aligned_alloc(16U, stack_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto* storage = static_cast<StaticTask_t*>(
        heap_caps_calloc(1U, sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    auto wake = xSemaphoreCreateBinary(), done = xSemaphoreCreateBinary();
    if (!stack || !storage || !wake || !done) {
        heap_caps_free(stack);
        heap_caps_free(storage);
        if (wake)
            vSemaphoreDelete(wake);
        if (done)
            vSemaphoreDelete(done);
        return false;
    }
    tuning_worker.wake = wake;
    tuning_worker.done = done;
    tuning_worker.task = xTaskCreateStaticPinnedToCore(tuning_worker_task, "nam_tune_b",
                                                       stack_bytes, nullptr, 22, stack, storage, 1);
    if (!tuning_worker.task) {
        vSemaphoreDelete(wake);
        vSemaphoreDelete(done);
        heap_caps_free(stack);
        heap_caps_free(storage);
        tuning_worker = {};
        return false;
    }
    return true;
}
void submit_tuning_work(void (*run)(void*), void* argument) {
    tuning_worker.run = run;
    tuning_worker.argument = argument;
    xSemaphoreGive(tuning_worker.wake);
}
void wait_tuning_work() {
    xSemaphoreTake(tuning_worker.done, portMAX_DELAY);
}
#endif

// A float copy of the model, used once at load to find each layer's peaks. The
// scales cannot be derived from the weights alone: what matters is how large
// the stream and the convolution sums actually get, which depends on the model.
struct FloatModel {
    struct Layer {
        std::array<float, 15 * kChannels * kChannels> convolution{};
        std::array<float, kChannels> bias{};
        std::array<float, kChannels> mixin{};
        std::array<float, kChannels * kChannels> residual{};
        std::array<float, kChannels> residual_bias{};
        float* history = nullptr;
        std::size_t capacity = 0;
        std::size_t position = 0;

        ~Layer() {
            delete[] history;
        }
    };
    std::array<Layer, kLayerCount> layers{};
    std::array<float, kChannels> rechannel{};
    std::array<float, kHeadKernel * kChannels> head{};
    float head_bias = 0.0F;
    float head_scale = 0.0F;
    std::array<float, kHeadKernel * kChannels> head_history{};
    std::size_t head_position = 0;
    std::array<float, kLayerCount> max_stream{};
    std::array<float, kLayerCount> max_conv{};
    // The residual path is quantised too, so it needs its own two peaks: how
    // large an activation gets, and how large the matrix makes their sum.
    std::array<float, kLayerCount> max_activation{};
    std::array<std::array<float, kChannels>, kLayerCount> max_activation_channel{};
    std::array<float, kLayerCount> max_residual{};
    std::array<float, kChannels> max_head_sum{};
    struct Block {
        FloatModel* owner = nullptr;
        const float* samples = nullptr;
        float* output = nullptr;
        std::size_t frames = 0;
        std::array<std::array<float, kChannels>, 64U> streams{}, heads{};
    };
    // Both slots reside with FloatModel in PSRAM, never on a task stack.
    std::array<Block, 2> blocks{};
    bool parallel_tuning = false;
    std::size_t tuning_block_frames = 64U;
    void begin(Block&, const float*, float*, std::size_t) noexcept;
    void run_layers(Block&, std::size_t, std::size_t) noexcept;
    void finish(Block&) noexcept;

    void run(const float* samples, std::size_t count) noexcept {
        run(samples, nullptr, count);
    }
    void reset_state() noexcept {
        for (Layer& layer : layers) {
            if (layer.history != nullptr) {
                std::fill_n(layer.history, layer.capacity * kChannels, 0.0F);
            }
            layer.position = 0U;
        }
        head_history = {};
        head_position = 0U;
    }
    // With an output pointer this is a plain float A2-Full, which is how the
    // structure here gets checked independently of the quantisation.
    void run(const float* samples, float* output, std::size_t count) noexcept;
};

void FloatModel::begin(Block& block, const float* samples, float* output,
                       std::size_t frames) noexcept {
    block.owner = this;
    block.samples = samples;
    block.output = output;
    block.frames = frames;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        block.heads[frame].fill(0.0F);
        for (std::size_t channel = 0; channel < kChannels; ++channel)
            block.streams[frame][channel] = rechannel[channel] * samples[frame];
    }
}
void FloatModel::run_layers(Block& block, std::size_t first, std::size_t last) noexcept {
    for (std::size_t index = first; index < last; ++index) {
        for (std::size_t frame = 0; frame < block.frames; ++frame) {
            const float condition = block.samples[frame];
            auto& stream = block.streams[frame];
            auto& head_sum = block.heads[frame];
            Layer& layer = layers[index];
            const std::size_t kernel = kKernelSizes[index];
            const std::size_t dilation = kDilations[index];
            float* const history = layer.history;
            float* const current = history + layer.position * kChannels;
            for (std::size_t channel = 0; channel < kChannels; ++channel) {
                current[channel] = stream[channel];
                max_stream[index] = std::max(max_stream[index], std::fabs(stream[channel]));
            }

            std::array<float, kChannels> activated{};
            for (std::size_t output = 0; output < kChannels; ++output) {
                activated[output] = layer.bias[output] + layer.mixin[output] * condition;
            }
            for (std::size_t tap = 0; tap < kernel; ++tap) {
                const std::size_t delay = (kernel - 1U - tap) * dilation;
                // position and delay are both below capacity, so one
                // subtraction wraps exactly without an integer division.
                const std::size_t at = layer.position >= delay
                                           ? layer.position - delay
                                           : layer.position + layer.capacity - delay;
                const float* const source = history + at * kChannels;
                const float* const weights = layer.convolution.data() + tap * kChannels * kChannels;
                for (std::size_t input = 0; input < kChannels; ++input) {
                    for (std::size_t output = 0; output < kChannels; ++output) {
                        activated[output] += weights[input * kChannels + output] * source[input];
                    }
                }
            }
            for (std::size_t output = 0; output < kChannels; ++output) {
                max_conv[index] = std::max(max_conv[index], std::fabs(activated[output]));
                activated[output] = leaky(activated[output]);
                max_activation[index] =
                    std::max(max_activation[index], std::fabs(activated[output]));
                max_activation_channel[index][output] =
                    std::max(max_activation_channel[index][output], std::fabs(activated[output]));
            }

            ++layer.position;
            if (layer.position == layer.capacity) {
                layer.position = 0;
            }

            for (std::size_t output = 0; output < kChannels; ++output) {
                head_sum[output] += activated[output];
                float residual = 0.0F;
                for (std::size_t input = 0; input < kChannels; ++input) {
                    residual += layer.residual[input * kChannels + output] * activated[input];
                }
                max_residual[index] = std::max(max_residual[index], std::fabs(residual));
                stream[output] += residual + layer.residual_bias[output];
            }
        }
    }
}
void FloatModel::finish(Block& block) noexcept {
    for (std::size_t frame = 0; frame < block.frames; ++frame) {
        auto& head_sum = block.heads[frame];
        float* const head_current = head_history.data() + head_position * kChannels;
        for (std::size_t channel = 0; channel < kChannels; ++channel) {
            max_head_sum[channel] = std::max(max_head_sum[channel], std::fabs(head_sum[channel]));
        }
        std::copy_n(head_sum.data(), kChannels, head_current);
        head_position = (head_position + 1U) % kHeadKernel;

        if (block.output != nullptr) {
            float result = head_bias;
            for (std::size_t tap = 0; tap < kHeadKernel; ++tap) {
                const std::size_t delay = kHeadKernel - 1U - tap;
                // head_position already points at the next slot, so the frame
                // just written is at position - 1.
                const std::size_t at = (head_position + kHeadKernel - 1U - delay) % kHeadKernel;
                const float* const source = head_history.data() + at * kChannels;
                const float* const weights = head.data() + tap * kChannels;
                for (std::size_t channel = 0; channel < kChannels; ++channel) {
                    result += weights[channel] * source[channel];
                }
            }
            block.output[frame] = result * head_scale;
        }
    }
}
void FloatModel::run(const float* samples, float* output, std::size_t count) noexcept {
    constexpr std::size_t split = 13U; // Equal convolution tap counts on both cores.
    bool pending = false;
    for (std::size_t base = 0, number = 0; base < count; base += tuning_block_frames, ++number) {
        auto& block = blocks[number & 1U];
        begin(block, samples + base, output ? output + base : nullptr,
              std::min(tuning_block_frames, count - base));
#if defined(ESP_PLATFORM)
        if (parallel_tuning) {
            run_layers(block, 0U, split);
            if (pending)
                wait_tuning_work();
            submit_tuning_work(
                [](void* p) {
                    auto& b = *static_cast<Block*>(p);
                    b.owner->run_layers(b, split, kLayerCount);
                    b.owner->finish(b);
                },
                &block);
            pending = true;
        } else
#endif
        {
            run_layers(block, 0U, kLayerCount);
            finish(block);
        }
#if defined(ESP_PLATFORM)
        if ((number & 15U) == 15U)
            vTaskDelay(1);
#endif
    }
#if defined(ESP_PLATFORM)
    if (pending)
        wait_tuning_work();
#else
    (void)pending;
    (void)split;
#endif
}

// A quarter-second full-band chirp. Generate it entirely from integers so the
// host and the device calibrate from the same float bitstream: libm's
// powf/sinf are allowed to differ by an ULP, and one changed peak can otherwise
// select an adjacent fixed-point constant in the recurrent engine.
void fill_calibration(float* const samples, const std::size_t count) noexcept {
    constexpr std::uint64_t kPhaseTurn = UINT64_C(1) << 32U;
    constexpr std::uint32_t kStartIncrement =
        static_cast<std::uint32_t>(60U * kPhaseTurn / kSampleRate);
    constexpr std::uint32_t kEndIncrement =
        static_cast<std::uint32_t>(8000U * kPhaseTurn / kSampleRate);
    constexpr float kTriangleScale = 0.95F / 32767.0F;
    std::uint32_t phase = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const std::uint32_t increment =
            kStartIncrement +
            static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(kEndIncrement - kStartIncrement) * index) / count);
        phase += increment;
        const std::uint32_t position = phase >> 16U;
        const std::int32_t triangle =
            static_cast<std::int32_t>(((position < 32768U) ? position : 65535U - position) * 2U) -
            32767;
        samples[index] = static_cast<float>(triangle) * kTriangleScale;
    }
}

} // namespace

namespace {
// The header checks and the weight parse, shared by the engine and by the test
// hook below. Returns a heap FloatModel the caller owns, or null with the
// reason written out.
FloatModel* parse_float_model(const uint8_t* const data, const std::size_t size,
                              char* const error_message, const std::size_t error_message_capacity,
                              const bool allocate_history = true) noexcept {
    constexpr std::size_t kWeightCount = A2FullS3Native::kWeightCount;
    if (data == nullptr || size != A2FullS3Native::kFileSize) {
        copy_error("invalid .namb file size", error_message, error_message_capacity);
        return nullptr;
    }
    if (std::memcmp(data, "NAMB", 4) != 0 || read_u16(data + 4U) != kFormatVersion) {
        copy_error("invalid .namb header", error_message, error_message_capacity);
        return nullptr;
    }
    if (read_u16(data + 6U) != kArchitectureA2Full) {
        copy_error("model is not A2-Full", error_message, error_message_capacity);
        return nullptr;
    }
    if (read_u32(data + 8U) != kSampleRate || read_u32(data + 12U) != kWeightCount) {
        copy_error("unsupported .namb parameters", error_message, error_message_capacity);
        return nullptr;
    }
    const uint8_t* const payload = data + kHeaderSize;
    if (crc32(payload, kWeightCount * sizeof(float)) != read_u32(data + 16U)) {
        copy_error(".namb checksum mismatch", error_message, error_message_capacity);
        return nullptr;
    }

    for (std::size_t i = 0; i < kWeightCount; ++i) {
        if (!std::isfinite(read_float(payload + i * 4U))) {
            copy_error("non-finite model weight", error_message, error_message_capacity);
            return nullptr;
        }
    }
    FloatModel* const model = new (std::nothrow) FloatModel;
    if (model == nullptr) {
        copy_error("calibration allocation failed", error_message, error_message_capacity);
        return nullptr;
    }

    std::size_t weight_index = 0;
    auto take = [&]() noexcept { return read_float(payload + (weight_index++) * sizeof(float)); };

    // The .namb weight order.
    for (float& value : model->rechannel) {
        value = take();
    }
    for (std::size_t index = 0; index < kLayerCount; ++index) {
        FloatModel::Layer& layer = model->layers[index];
        const std::size_t kernel = kKernelSizes[index];
        layer.capacity = (kernel - 1U) * kDilations[index] + 1U;
        if (allocate_history) {
            layer.history = new (std::nothrow) float[layer.capacity * kChannels]{};
            if (layer.history == nullptr) {
                delete model;
                copy_error("calibration history allocation failed", error_message,
                           error_message_capacity);
                return nullptr;
            }
        }
        for (std::size_t output = 0; output < kChannels; ++output) {
            for (std::size_t input = 0; input < kChannels; ++input) {
                for (std::size_t tap = 0; tap < kernel; ++tap) {
                    layer.convolution[tap * kChannels * kChannels + input * kChannels + output] =
                        take();
                }
            }
        }
        for (float& value : layer.bias) {
            value = take();
        }
        for (float& value : layer.mixin) {
            value = take();
        }
        for (std::size_t output = 0; output < kChannels; ++output) {
            for (std::size_t input = 0; input < kChannels; ++input) {
                layer.residual[input * kChannels + output] = take();
            }
        }
        for (float& value : layer.residual_bias) {
            value = take();
        }
    }
    for (std::size_t input = 0; input < kChannels; ++input) {
        for (std::size_t tap = 0; tap < kHeadKernel; ++tap) {
            model->head[tap * kChannels + input] = take();
        }
    }
    model->head_bias = take();
    model->head_scale = take();
    return model;
}
} // namespace

namespace {

constexpr std::int32_t kS3NativeActivationMaximum = (INT32_C(1) << (22 - 1)) - 1;
// Only the final layer uses a direct A16 activation grid. Every other layer
// splits its A22 activation into a high and a low limb.
constexpr std::uint32_t kS3NativeA16LayerMask = 0x400000U;

inline bool s3_native_low_layer(const std::size_t index) noexcept {
    return (A2FullS3Native::kLowLayerMask & (1U << index)) != 0U;
}

inline std::int32_t s3_native_activation_maximum(const std::size_t index) noexcept {
    return (kS3NativeA16LayerMask & (1U << index)) != 0U ? 32767 : kS3NativeActivationMaximum;
}

inline bool s3_native_a16_layer(const std::size_t index) noexcept {
    return (kS3NativeA16LayerMask & (1U << index)) != 0U;
}

// The bank arena.
//
// ESP32-S3 data SRAM is interleaved in 32 KiB banks, and two cores reading the
// same bank arbitrate on every access. Each pipeline stage therefore gets its
// hottest blocks -- its coefficient table and, for stage A, its largest
// history -- on a bank of their own, and a bank's tail only ever holds
// histories of the stage that owns it.
//
// Asking the heap for 32 KiB-aligned blocks fails once USB, the display and the
// effects have fragmented it, so the banks are a fixed region instead:
// kS3BankCount banks at kS3BankArenaBase, reserved out of the heap with
// SOC_RESERVE_MEMORY_REGION before heap_caps_init runs. The address is above
// the linker's static ceiling, where no static object can reach, so the
// reservation costs no .bss alignment padding.
//
// The first kS3BankKeptBanks banks hold the aligned blocks and always stay the
// arena's. A maintenance boot, which never loads a graph, lends the rest to the
// heap. After a graph is placed, every unused bank tail goes back to the heap.
#if defined(ESP_PLATFORM)
constexpr std::size_t kS3BankBytes = 32U * 1024U;
constexpr std::size_t kS3BankCount = 5U;
constexpr std::uintptr_t kS3BankArenaBase = 0x3FCC0000U;
constexpr std::size_t kS3BankArenaBytes = kS3BankBytes * kS3BankCount;

// dram0_0_seg, the linker's ceiling for static data, ends at 0x3FCDB700; the
// arena sits above it, below APP_USABLE_DRAM_END (0x3FCE9710). Nothing reads
// the region before the first model load and every block is zeroed as it is
// handed out, so it needs no startup clearing. If .bss ever grows past
// kS3BankArenaBase, the two reservations overlap and heap_caps_init aborts at
// boot.
std::uint8_t* const g_s3_bank_arena = reinterpret_cast<std::uint8_t*>(kS3BankArenaBase);
std::size_t g_s3_bank_used[kS3BankCount];
// How much of each bank is still the arena's. A tail given back to the heap
// after the graph is placed drops out of the arena for good, so a later reload
// cannot hand out memory the display is now using.
std::size_t g_s3_bank_limit[kS3BankCount];
bool g_s3_bank_limits_ready;
// The banks that are never lent. The three blocks that must start on a 32 KiB
// boundary -- the two stages' hot coefficient tables and the largest Core 0
// history -- get their own bank each, and those banks stay the arena's in every
// mode. Were they lent, a maintenance boot would scatter Wi-Fi, HTTP, BLE and
// display allocations through them, and returning to the pedalboard in place
// would find no free 32 KiB boundary for the coefficient tables.
constexpr std::size_t kS3BankKeptBanks = 3U;
static_assert(kS3BankKeptBanks <= kS3BankCount, "cannot keep more banks than exist");
// Whether the banks past the kept ones have been handed to the maintenance heap.
bool g_s3_bank_lent;

// A bank this boot may still hand out.
constexpr bool s3_bank_available(const std::size_t bank, const bool lent) noexcept {
    return !lent || bank < kS3BankKeptBanks;
}
// Which pipeline stage -- which core -- streams a bank. The 32 KiB separation
// only helps while one core owns the bank, so a bank carries the stage that
// claimed it and only ever takes tails from that stage.
enum : std::uint8_t { kS3StageNone = 0U, kS3StageA = 1U, kS3StageB = 2U };
std::uint8_t g_s3_bank_stage[kS3BankCount];
// A stage pinned independently of any graph placement. The effects slice is
// carved once and outlives every graph load, so its bank belongs to Core 1 for
// the whole boot -- a reset must not release it to Core 0.
std::uint8_t g_s3_bank_pin[kS3BankCount];

// The first bank no block has taken yet that this stage may own.
std::size_t s3_bank_free_bank(const std::uint8_t stage) noexcept {
    for (std::size_t bank = 0; bank < kS3BankCount; ++bank) {
        if (s3_bank_available(bank, g_s3_bank_lent) && g_s3_bank_used[bank] == 0U &&
            (g_s3_bank_stage[bank] == kS3StageNone || g_s3_bank_stage[bank] == stage)) {
            return bank;
        }
    }
    return kS3BankCount;
}

constexpr std::size_t s3_bank_round(const std::size_t bytes) noexcept {
    return (bytes + 15U) & ~std::size_t{15U};
}

void s3_bank_limits_init() noexcept {
    if (g_s3_bank_limits_ready) {
        return;
    }
    for (auto& limit : g_s3_bank_limit) {
        limit = kS3BankBytes;
    }
    g_s3_bank_limits_ready = true;
}

bool s3_bank_arena_owns(const void* const block) noexcept {
    const auto address = reinterpret_cast<std::uintptr_t>(block);
    if (address < kS3BankArenaBase || address >= kS3BankArenaBase + kS3BankArenaBytes) {
        return false;
    }
    s3_bank_limits_init();
    const std::size_t offset = address - kS3BankArenaBase;
    const std::size_t bank = offset / kS3BankBytes;
    // A lent bank holds ordinary heap addresses and must be freed like any
    // other, or a graph loaded in this boot leaks every ring inside it.
    if (!s3_bank_available(bank, g_s3_bank_lent)) {
        return false;
    }
    return offset % kS3BankBytes < g_s3_bank_limit[bank];
}

// A whole bank, for a block that must not share one with another stage.
void* s3_bank_arena_take_bank(const std::size_t bytes, const std::uint8_t stage) noexcept {
    if (bytes == 0U || bytes > kS3BankBytes) {
        return nullptr;
    }
    s3_bank_limits_init();
    const std::size_t bank = s3_bank_free_bank(stage);
    if (bank == kS3BankCount || s3_bank_round(bytes) > g_s3_bank_limit[bank]) {
        return nullptr;
    }
    g_s3_bank_used[bank] = s3_bank_round(bytes);
    g_s3_bank_stage[bank] = stage;
    std::uint8_t* const block = g_s3_bank_arena + bank * kS3BankBytes;
    std::memset(block, 0, bytes);
    return block;
}

// The tail of a bank whose aligned block is already placed. Best fit, so a
// large history is not stranded by a small one taking the only tail that could
// have held it. A bank nobody claimed for separation is fair game once the
// aligned blocks have all been handed out, which they are: they are requested
// before any history.
void* s3_bank_arena_take_tail(const std::size_t bytes, const std::uint8_t stage) noexcept {
    if (bytes == 0U) {
        return nullptr;
    }
    s3_bank_limits_init();
    const std::size_t want = s3_bank_round(bytes);
    std::size_t best = kS3BankCount;
    std::size_t best_free = kS3BankBytes + 1U;
    for (std::size_t bank = 0; bank < kS3BankCount; ++bank) {
        if (!s3_bank_available(bank, g_s3_bank_lent)) {
            continue;
        }
        // Only this stage's banks. A tail in the other core's bank is worse
        // than PSRAM would be at the same size: it is fast for whoever reads
        // it and a stall for the core whose coefficient stream it interrupts.
        if (g_s3_bank_stage[bank] != stage) {
            continue;
        }
        const std::size_t free_bytes = g_s3_bank_limit[bank] - g_s3_bank_used[bank];
        if (free_bytes >= want && free_bytes < best_free) {
            best = bank;
            best_free = free_bytes;
        }
    }
    if (best == kS3BankCount) {
        best = s3_bank_free_bank(stage);
        if (best == kS3BankCount || want > g_s3_bank_limit[best]) {
            return nullptr;
        }
        g_s3_bank_stage[best] = stage;
    }
    std::uint8_t* const block = g_s3_bank_arena + best * kS3BankBytes + g_s3_bank_used[best];
    g_s3_bank_used[best] += want;
    std::memset(block, 0, bytes);
    return block;
}

void s3_bank_arena_reset() noexcept {
    for (auto& used : g_s3_bank_used) {
        used = 0U;
    }
    for (std::size_t bank = 0; bank < kS3BankCount; ++bank) {
        g_s3_bank_stage[bank] = g_s3_bank_pin[bank];
    }
}

// Keep the heap out of the arena. This is the documented way for a component to
// claim a fixed range before heap_caps_init runs, and the only way to hold
// memory above the linker's static ceiling.
SOC_RESERVE_MEMORY_REGION(kS3BankArenaBase, kS3BankArenaBase + kS3BankArenaBytes, nam_bank_arena)
#else
bool s3_bank_arena_owns(const void*) noexcept {
    return false;
}
enum : std::uint8_t { kS3StageNone = 0U, kS3StageA = 1U, kS3StageB = 2U };
void* s3_bank_arena_take_tail(std::size_t, std::uint8_t) noexcept {
    return nullptr;
}
void s3_bank_arena_reset() noexcept {}
#endif

// MALLOC_CAP_INTERNAL alone is not "fast memory" on this chip.
//
// esp32s3/memory_layout.c registers RTC fast memory as a heap region whose caps
// include MALLOC_CAP_INTERNAL, in the allocator's low-priority column, so an
// internal request that DIRAM and DRAM cannot fit falls through to it silently.
// That region runs on the APB clock, a third of the CPU's speed.
//
// RTC RAM is the one internal region without MALLOC_CAP_DMA, and every real
// DRAM region has it, so asking for DMA costs nothing here and puts RTC fast
// memory out of reach. A ring that does not fit fails and is replanned instead
// of running a convolution out of slow memory.
template <typename T> T* s3_native_allocate(const std::size_t count, const bool internal) noexcept {
#if defined(ESP_PLATFORM)
    const std::uint32_t caps =
        internal ? (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) : MALLOC_CAP_SPIRAM;
    return static_cast<T*>(heap_caps_aligned_calloc(16, count, sizeof(T), caps));
#else
    (void)internal;
    return new (std::nothrow) T[count]{};
#endif
}

template <typename T>
T* s3_native_allocate_bank_aligned(const std::size_t count, const std::uint8_t stage,
                                   const bool external = false) noexcept {
    if (external)
        return s3_native_allocate<T>(count, false);
#if defined(ESP_PLATFORM)
    // ESP32-S3 data SRAM is interleaved in 32 KiB banks. Giving each pipeline
    // stage its own aligned coefficient base prevents the two LX7 cores from
    // streaming their hottest convolution tables through the same bank.
    //
    // The bank comes from the arena, where the alignment is free and the
    // placement cannot fail. Only a maintenance boot, which has lent the
    // arena back to the heap and never loads a graph, falls through.
    if (void* const bank = s3_bank_arena_take_bank(count * sizeof(T), stage); bank != nullptr) {
        return static_cast<T*>(bank);
    }
    constexpr std::size_t kSramBankBytes = 32U * 1024U;
    return static_cast<T*>(heap_caps_aligned_calloc(kSramBankBytes, count, sizeof(T),
                                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
#else
    (void)stage;
    return new (std::nothrow) T[count]{};
#endif
}

template <typename T> void s3_native_free(T*& pointer) noexcept {
#if defined(ESP_PLATFORM)
    // Arena blocks are fixed storage and outlive the partial free that
    // precedes a joint replan, which is why the pointer is left in place: the
    // replan skips every history it still holds. release() drops them.
    if (s3_bank_arena_owns(pointer)) {
        return;
    }
    heap_caps_free(pointer);
#else
    delete[] pointer;
#endif
    pointer = nullptr;
}

int ceil_log2_ratio(const float numerator, const float denominator) noexcept {
    return static_cast<int>(
        std::ceil(std::log2(std::max(numerator, 1.0e-30F) / std::max(denominator, 1.0e-30F))));
}

std::int32_t quantize_i32(const float value) noexcept {
    // The source is already binary32 and all runtime grids are calibrated
    // inside int32. Avoid promoting every sample/channel to software double;
    // LX7 can round binary32 to a 32-bit integer directly.
    return static_cast<std::int32_t>(std::lrintf(value));
}

} // namespace

#if defined(ESP_PLATFORM)
// A maintenance boot never loads a graph and does need internal SRAM for Wi-Fi,
// HTTP and OTA, so it gives the arena to the heap for the life of the boot. It
// is never taken back: the two modes do not coexist, and a reboot is what
// returns the board to audio.
extern "C" void coyopedal_nam_bank_arena_lend_to_heap(void) {
    if (g_s3_bank_lent) {
        return;
    }
    constexpr std::size_t kLentBytes = (kS3BankCount - kS3BankKeptBanks) * kS3BankBytes;
    if (kLentBytes == 0U) {
        return;
    }
    const auto start = static_cast<intptr_t>(kS3BankArenaBase + kS3BankKeptBanks * kS3BankBytes);
    const esp_err_t added =
        heap_caps_add_region(start, start + static_cast<intptr_t>(kLentBytes) - 1);
    if (added != ESP_OK) {
        ESP_LOGW("nam", "NAM bank arena stays reserved: %u bytes at 0x%08x rejected (%s)",
                 static_cast<unsigned>(kLentBytes), static_cast<unsigned>(start),
                 esp_err_to_name(added));
        return;
    }
    g_s3_bank_lent = true;
    ESP_LOGI("nam",
             "NAM bank arena lent %u of %u bytes to the maintenance heap at 0x%08x; "
             "%u bytes of aligned banks stay reserved",
             static_cast<unsigned>(kLentBytes), static_cast<unsigned>(kS3BankArenaBytes),
             static_cast<unsigned>(start), static_cast<unsigned>(kS3BankKeptBanks * kS3BankBytes));
}

// Carve a slice off the top of bank 1 for the effects, before the graph is
// placed. A heap that has just placed the graph, the USB host and the display
// cannot hand out even four contiguous kilobytes for the reverb's hot state;
// the arena can, because its addresses are fixed. The graph pays for the slice
// by spilling the same number of bytes to the heap, where its small histories
// fit in fragments the reverb could not use.
extern "C" void* coyopedal_nam_bank_arena_take_effects(const std::size_t bytes) {
    static std::uint8_t* carved = nullptr;
    static std::size_t carved_bytes = 0U;
    if (bytes == 0U) {
        return nullptr;
    }
    // Idempotent: the slice outlives a graph unload, so a reload gets the same
    // addresses back instead of shrinking the bank again.
    if (carved != nullptr) {
        return bytes <= carved_bytes ? carved : nullptr;
    }
    s3_bank_limits_init();
    // Banks are claimed in order: bank 0 by the largest Core 0 history, bank 1
    // by Core 1's coefficient table and bank 2 by Core 0's. The slice pins its
    // bank to Core 1, so it must be bank 1; pinning bank 2 would leave Core 0's
    // table without a bank.
    constexpr std::size_t bank = 1U;
    static_assert(bank < kS3BankKeptBanks, "the effects slice must not sit in a lent bank");
    const std::size_t want = s3_bank_round(bytes);
    // Only before the graph places anything in that bank, and never so much
    // that a 32 KiB-aligned block could no longer start there.
    if (g_s3_bank_used[bank] != 0U || want >= g_s3_bank_limit[bank]) {
        return nullptr;
    }
    g_s3_bank_limit[bank] -= want;
    // The reverb runs in stage_b_task on Core 1, so this bank is Core 1's for
    // the rest of the boot: no Core 0 history may be packed in front of it.
    g_s3_bank_pin[bank] = kS3StageB;
    g_s3_bank_stage[bank] = kS3StageB;
    carved = g_s3_bank_arena + bank * kS3BankBytes + g_s3_bank_limit[bank];
    carved_bytes = want;
    std::memset(carved, 0, want);
    ESP_LOGI("nam", "effects slice carved from the NAM bank arena: %u bytes at %p",
             static_cast<unsigned>(want), static_cast<void*>(carved));
    return carved;
}

// So the firmware's release paths can tell an arena pointer from a heap one.
extern "C" bool coyopedal_nam_bank_arena_owns_ptr(const void* const block) {
    const auto address = reinterpret_cast<std::uintptr_t>(block);
    if (address < kS3BankArenaBase || address >= kS3BankArenaBase + kS3BankArenaBytes) {
        return false;
    }
    // A lent bank is ordinary heap and must be freed as such.
    return s3_bank_available((address - kS3BankArenaBase) / kS3BankBytes, g_s3_bank_lent);
}

// Hand every byte the placed graph did not take back to the heap. The LCD
// flush buffers, the effect delay lines and the tuner pump all allocate
// internal SRAM after the graph is placed, out of whatever it left behind.
extern "C" void coyopedal_nam_bank_arena_release_unused(void) {
    s3_bank_limits_init();
    std::size_t returned = 0U;
    for (std::size_t bank = 0; bank < kS3BankCount; ++bank) {
        if (!s3_bank_available(bank, g_s3_bank_lent)) {
            continue;
        }
        const std::size_t tail = g_s3_bank_limit[bank] - g_s3_bank_used[bank];
        // Below a TLSF block header plus a usable payload the region costs more
        // than it gives, and heap_caps_add_region would refuse it anyway.
        if (tail < 1024U) {
            continue;
        }
        const auto start =
            static_cast<intptr_t>(kS3BankArenaBase + bank * kS3BankBytes + g_s3_bank_used[bank]);
        if (heap_caps_add_region(start, start + static_cast<intptr_t>(tail) - 1) != ESP_OK) {
            continue;
        }
        g_s3_bank_limit[bank] = g_s3_bank_used[bank];
        returned += tail;
    }
    ESP_LOGI("nam", "NAM bank arena returned %u unused bytes; internal free=%u largest=%u",
             static_cast<unsigned>(returned),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
}
#endif

A2FullS3Native::~A2FullS3Native() {
    release();
}

void A2FullS3Native::release() noexcept {
    loaded_ = false;
    s3_native_free(ring_high_);
    for (auto*& ring : ring_high_internal_) {
        s3_native_free(ring);
    }
    s3_native_free(ring_low_l0_s8_);
    s3_native_free(ring_low_);
    s3_native_free(convolution_high_);
    s3_native_free(convolution_high_stage_b_);
    convolution_low_l0_s8_ = nullptr;
    convolution_low_ = nullptr;
    convolution_low_stage_b_ = nullptr;
    residual_high_ = nullptr;
    residual_high_stage_b_ = nullptr;
    residual_low_ = nullptr;
    residual_low_stage_b_ = nullptr;
    // s3_native_free deliberately leaves arena pointers in place for the
    // replan, so a full teardown drops them by hand and rewinds the cursors.
    // Placement is deterministic, so the next load lands identically.
    for (auto*& ring : ring_high_internal_) {
        if (s3_bank_arena_owns(ring)) {
            ring = nullptr;
        }
    }
    if (s3_bank_arena_owns(convolution_high_)) {
        convolution_high_ = nullptr;
    }
    if (s3_bank_arena_owns(convolution_high_stage_b_)) {
        convolution_high_stage_b_ = nullptr;
    }
    // The arena's cursors are global and belong to the live graph. A tuning
    // candidate never takes arena memory (its blocks all go to PSRAM), and it
    // is released while the live graph still occupies the arena. Rewinding
    // the cursors here would mark the live rings and tables free, and the
    // candidate's next placement would hand them to the heap.
    if (!external_workspace_) {
        s3_bank_arena_reset();
    }
    ring_high_count_ = ring_low_count_ = 0;
    convolution_high_count_ = convolution_low_count_ = 0;
    residual_high_count_ = residual_low_count_ = 0;
}

std::int16_t* A2FullS3Native::convolution_high_for_layer(const std::size_t index) const noexcept {
    if (index < kStageSplitLayer) {
        return convolution_high_ + layers_[index].convolution_high_offset;
    }
    return convolution_high_stage_b_ +
           (layers_[index].convolution_high_offset - convolution_high_stage_b_offset_);
}

std::int16_t* A2FullS3Native::convolution_low_for_layer(const std::size_t index) const noexcept {
    if (index == 0U) {
        return convolution_low_l0_s8_;
    }
    if (index < kStageSplitLayer) {
        return convolution_low_ + layers_[index].convolution_low_offset;
    }
    return convolution_low_stage_b_ +
           (layers_[index].convolution_low_offset - convolution_low_stage_b_offset_);
}

std::int16_t* A2FullS3Native::residual_high_for_layer(const std::size_t index) const noexcept {
    if (index < kStageSplitLayer) {
        return residual_high_ + layers_[index].residual_high_offset;
    }
    return residual_high_stage_b_ +
           (layers_[index].residual_high_offset - residual_high_stage_b_offset_);
}

std::int16_t* A2FullS3Native::residual_low_for_layer(const std::size_t index) const noexcept {
    if (index < kStageSplitLayer) {
        return residual_low_ + layers_[index].residual_low_offset;
    }
    return residual_low_stage_b_ +
           (layers_[index].residual_low_offset - residual_low_stage_b_offset_);
}

namespace {
void precision_u32(std::uint8_t* out, std::uint32_t value) noexcept {
    for (unsigned i = 0; i < 4; ++i)
        out[i] = static_cast<std::uint8_t>(value >> (i * 8));
}
void precision_float(std::uint8_t* out, float value) noexcept {
    std::uint32_t bits;
    std::memcpy(&bits, &value, 4);
    precision_u32(out, bits);
}
std::uint32_t precision_crc(const std::uint8_t* record) noexcept {
    // The CRC field is outside the covered bytes: prefix + record, stored last.
    return crc32(record, A2FullS3Native::kPreparedTrailerSize - 4U);
}
} // namespace

namespace {
float prepared_stream_margin(const std::uint8_t* data) noexcept {
    if (read_u32(data + 28U) == 0x4D533353U) {
        const float value = read_float(data + 24U);
        if (std::isfinite(value) && value >= 1.0F && value <= 16.0F)
            return value;
    }
    return 1.6F;
}
void make_sweep_precision(
    FloatModel& model, float stream_margin, float head_margin, float layer_margin,
    std::array<std::uint8_t, A2FullS3Native::kDiagnosticPrecisionSize>& record,
    bool full_lanes = false) noexcept {
    // Full-lane grids need reserve beyond the measured calibration envelope.
    // Keep transient headroom even if the short validation prefers a smaller
    // range; S16 activation/history extraction wraps outside its lane range.
    if (full_lanes)
        layer_margin = std::max(layer_margin, 1.3125F);
    float stream_peak = 0, head_peak = 0;
    for (std::size_t i = 0; i < kLayerCount; ++i) {
        model.max_stream[i] = stable_calibration_peak(model.max_stream[i]);
        model.max_activation[i] = stable_calibration_peak(model.max_activation[i]);
        stream_peak = std::max(stream_peak, model.max_stream[i]);
    }
    for (float peak : model.max_head_sum)
        head_peak = std::max(head_peak, stable_calibration_peak(peak));
    const float stream = std::max(stream_peak * stream_margin / 1073741823.0F, 1.0e-20F);
    const float head = std::max(head_peak * head_margin / 1073741823.0F, 1.0e-20F);
    record.fill(0);
    std::memcpy(record.data(), full_lanes ? "S3P2" : "S3P1", 4);
    precision_float(record.data() + 4, stream);
    precision_float(record.data() + 8, head);
    for (std::size_t i = 0; i < kLayerCount; ++i) {
        const auto& source = model.layers[i];
        const int ring_shift =
            ceil_log2_ratio(std::max(model.max_stream[i] * layer_margin /
                                         (full_lanes && i != 0U ? 32767.0F * 128.0F : 2097151.0F),
                                     1.0e-20F),
                            stream);
        const float ring = std::ldexp(stream, ring_shift);
        const int head_shift = ceil_log2_ratio(
            std::max(model.max_activation[i] * layer_margin /
                         (full_lanes && i != 0U && i != 22U
                              ? 32767.0F * 128.0F
                              : static_cast<float>(s3_native_activation_maximum(i))),
                     1.0e-20F),
            head);
        const float activation = std::ldexp(head, head_shift);
        float conv_peak = 0, res_peak = 0;
        for (std::size_t at = 0; at < kKernelSizes[i] * 64; ++at)
            conv_peak = std::max(conv_peak, std::fabs(source.convolution[at] * ring));
        for (float weight : source.residual)
            res_peak = std::max(res_peak, std::fabs(weight * activation));
        const int conv_shift = static_cast<int>(
            std::floor(std::log2(activation / std::max(conv_peak / 32767.0F, 1.0e-20F))));
        const int res_shift = static_cast<int>(
            std::floor(std::log2(stream / std::max(res_peak / 32767.0F, 1.0e-20F))));
        auto* row = record.data() + 12 + i * 48;
        precision_u32(row, ring_shift);
        precision_u32(row + 4, head_shift);
        precision_u32(row + 8, conv_shift);
        precision_u32(row + 12, res_shift);
        for (unsigned ch = 0; ch < 8; ++ch)
            precision_u32(row + 16 + ch * 4, quantize_i32(source.bias[ch] / activation));
    }
    if (full_lanes) {
        // The output history narrows the accumulated head sum to S16. Its
        // transient reserve is independent of internal activation precision:
        // doubling this grid and decrementing head shifts preserves every
        // activation grid, convolution weight, and residual weight exactly.
        precision_float(record.data() + 8, head * 2.0F);
        for (std::size_t i = 0; i < kLayerCount; ++i) {
            auto* field = record.data() + 12 + i * 48 + 4;
            precision_u32(field, static_cast<std::int32_t>(read_u32(field)) - 1);
        }
    }
}
} // namespace

bool A2FullS3Native::prepare_sweep(const std::uint8_t* data, std::size_t size,
                                   std::uint8_t* trailer, char* error,
                                   std::size_t capacity) noexcept {
    if (!trailer)
        return false;
#if defined(ESP_PLATFORM)
    heap_caps_malloc_extmem_enable(0U);
    struct RestoreMalloc {
        ~RestoreMalloc() {
            heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
        }
    } restore_malloc;
#endif
    // Only the float calibration model is needed during import. In particular,
    // never allocate the native engine's retained internal-SRAM audio rings.
    std::unique_ptr<FloatModel> model(parse_float_model(data, size, error, capacity, true));
    if (!model)
        return false;
    constexpr std::size_t count = 12000;
    std::unique_ptr<float[]> sweep(new (std::nothrow) float[count]);
    if (!sweep) {
        copy_error("sweep allocation failed", error, capacity);
        return false;
    }
    fill_calibration(sweep.get(), count);
#if defined(ESP_PLATFORM)
    for (std::size_t at = 0; at < count; at += 64) {
        model->run(sweep.get() + at, std::min<std::size_t>(64, count - at));
        vTaskDelay(1);
    }
#else
    model->run(sweep.get(), count);
#endif
    std::array<std::uint8_t, kDiagnosticPrecisionSize> record{};
    make_sweep_precision(*model, prepared_stream_margin(data), 1.6F, 1.6F, record);
    return wrap_precision(data, record.data(), kS3NativeExactResidualMask, trailer);
}

#include "sweep_tuning.inc"

bool A2FullS3Native::validate_prepared(const std::uint8_t* data, std::size_t size) noexcept {
    if (!data || size != kPreparedFileSize)
        return false;
    if (std::memcmp(data, "NAMB", 4) || read_u16(data + 4) != 1 || read_u16(data + 6) != 2 ||
        read_u32(data + 8) != 48000 || read_u32(data + 12) != kWeightCount ||
        read_u32(data + 20) != 32 || read_u32(data + 16) != crc32(data + 32, kFileSize - 32))
        return false;
    for (std::size_t i = 0; i < kWeightCount; ++i)
        if (!std::isfinite(read_float(data + 32 + i * 4)))
            return false;
    const auto* p = data + kFileSize;
    const bool tuned = std::memcmp(p, "S3P4", 4) == 0;
    const auto* record = p + 12;
    if (std::memcmp(record, "S3P1", 4) && std::memcmp(record, "S3P2", 4))
        return false;
    for (unsigned at : {4U, 8U}) {
        const float grid = read_float(record + at);
        if (!std::isfinite(grid) || grid < 1.0e-20F || grid > 1.0F)
            return false;
    }
    for (std::size_t i = 0; i < kLayerCount; ++i) {
        const auto* row = record + 12 + i * 48;
        const auto head = static_cast<std::int32_t>(read_u32(row + 4));
        const auto residual = read_u32(row + 12);
        if (tuned && ((residual & ~0x00011f3fU) != 0U ||
                      (!(residual & 0x00010000U) && (residual & 0x00001f00U)) ||
                      ((residual & 0x00010000U) && (i < 12U || i > 21U))))
            return false;
        if (read_u32(row) > 24 || head < -24 || head > 24 || read_u32(row + 8) > 30 ||
            (tuned ? residual & 63U : residual) > (i + 1 == kLayerCount ? 63U : 30U))
            return false;
    }
    return (std::memcmp(p, "S3P3", 4) == 0 || tuned) && read_u32(p + 4) <= 0x3FF000U &&
           (read_u32(p + 4) & ~0x3FF000U) == 0 && read_u32(p + 8) == crc32(data, kFileSize) &&
           read_u32(p + kPreparedTrailerSize - 4) == precision_crc(p);
}

bool A2FullS3Native::wrap_precision(const std::uint8_t* model, const std::uint8_t* parameters,
                                    std::uint32_t mask, std::uint8_t* out) noexcept {
    if (!model || !parameters || !out || (mask & ~0x3FF000U))
        return false;
    std::memcpy(out, "S3P3", 4);
    precision_u32(out + 4, mask);
    precision_u32(out + 8, crc32(model, kFileSize));
    std::memcpy(out + 12, parameters, kDiagnosticPrecisionSize);
    precision_u32(out + kPreparedTrailerSize - 4, precision_crc(out));
    return true;
}

bool A2FullS3Native::export_precision(std::uint8_t* out, std::size_t size) const noexcept {
    if (!loaded_ || !out || size != kPreparedTrailerSize)
        return false;
    std::memcpy(out, tuned_narrowing_ ? "S3P4" : "S3P3", 4);
    precision_u32(out + 4, exact_residual_mask_);
    precision_u32(out + 8, model_crc_);
    auto* p = out + 12;
    std::memcpy(p, full_activation_lanes_ ? "S3P2" : "S3P1", 4);
    precision_float(p + 4, stream_scale_);
    precision_float(p + 8, head_grid_);
    for (std::size_t i = 0; i < kLayerCount; ++i) {
        auto* row = p + 12 + i * 48;
        const auto& layer = layers_[i];
        precision_u32(row, layer.ring_shift);
        precision_u32(row + 4, layer.head_shift);
        precision_u32(row + 8, layer.convolution_shift);
        precision_u32(
            row + 12,
            static_cast<std::uint32_t>(layer.residual_shift) |
                (tuned_narrowing_ && i >= 12U && i <= 21U
                     ? 0x00010000U | (static_cast<std::uint32_t>(layer.residual_narrow_shift) << 8U)
                     : 0U));
        for (unsigned j = 0; j < 8; ++j)
            precision_u32(row + 16 + j * 4, layer.activation_bias[j]);
    }
    precision_u32(out + kPreparedTrailerSize - 4, precision_crc(out));
    return true;
}

bool A2FullS3Native::exact_residual_layer(std::size_t index) const noexcept {
    return (exact_residual_mask_ & (1U << index)) != 0U;
}

bool A2FullS3Native::load_namb(const std::uint8_t* const data, const std::size_t size,
                               char* const error_message,
                               const std::size_t error_message_capacity) noexcept {
    const std::uint8_t* precision = nullptr;
    bool full_activation_lanes = false;
    const bool tuned_narrowing =
        data && size == kPreparedFileSize && std::memcmp(data + kFileSize, "S3P4", 4) == 0;
    std::uint32_t prepared_mask = kS3NativeExactResidualMask;
    if (size == kPreparedFileSize) {
        if (!validate_prepared(data, size)) {
            copy_error("invalid S3 prepared model (version, parameters or CRC)", error_message,
                       error_message_capacity);
            return false;
        }
        prepared_mask = read_u32(data + kFileSize + 4U);
        precision = data + kFileSize + 12U;
    }
    if (precision != nullptr) {
        full_activation_lanes = std::memcmp(precision, "S3P2", 4) == 0;
        bool valid = std::memcmp(precision, "S3P1", 4) == 0 || full_activation_lanes;
        for (std::size_t offset : {4U, 8U}) {
            const float grid = read_float(precision + offset);
            valid &= std::isfinite(grid) && grid >= 1.0e-20F && grid <= 1.0F;
        }
        for (std::size_t i = 0; i < kLayerCount; ++i) {
            const auto* row = precision + 12U + i * 48U;
            const auto ring = static_cast<std::int32_t>(read_u32(row));
            const auto head = static_cast<std::int32_t>(read_u32(row + 4U));
            valid &= ring >= 0 && ring <= 24 && head >= -24 && head <= 24;
            // The final layer has no residual output. Its zero weights can
            // produce a larger, unused sweep shift (44 in the bundled model).
            valid &= read_u32(row + 8U) <= 30U;
            valid &= (tuned_narrowing ? read_u32(row + 12U) & 63U : read_u32(row + 12U)) <=
                     (i + 1 == kLayerCount ? 63U : 30U);
        }
        if (!valid) {
            copy_error("invalid precision parameters", error_message, error_message_capacity);
            return false;
        }
    }
    // Every A2-Full capture has this same topology. Keep the fixed-shape
    // coefficient and history storage across model changes and replace its
    // contents while the audio pipeline is drained. Returning it to the heap
    // after USB, effects and UI have started fragments the large SRAM rings.
    loaded_ = false;
    ring_high_count_ = ring_low_count_ = 0;
    convolution_high_count_ = convolution_low_count_ = 0;
    residual_high_count_ = residual_low_count_ = 0;
#if defined(ESP_PLATFORM)
    // Parsing and calibration are cold, temporary work. Prefer PSRAM for their
    // ordinary C++ allocations so the retained realtime SRAM is untouched.
    heap_caps_malloc_extmem_enable(0U);
    struct RestoreMallocPreference {
        ~RestoreMallocPreference() {
            heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
        }
    } restore_malloc_preference;
#endif
    FloatModel* const model =
        parse_float_model(data, precision == nullptr ? size : kFileSize, error_message,
                          error_message_capacity, precision == nullptr);
    if (model == nullptr) {
        return false;
    }

    constexpr std::size_t kSweep = 12000;
    float* const sweep = precision == nullptr ? new (std::nothrow) float[kSweep] : nullptr;
    if (precision == nullptr && sweep == nullptr) {
        delete model;
        copy_error("S3-native calibration allocation failed", error_message,
                   error_message_capacity);
        return false;
    }
    if (precision == nullptr) {
        fill_calibration(sweep, kSweep);
#if defined(ESP_PLATFORM)
        constexpr std::size_t kCalibrationChunk = 64U;
        for (std::size_t at = 0; at < kSweep; at += kCalibrationChunk) {
            model->run(sweep + at, std::min(kCalibrationChunk, kSweep - at));
            vTaskDelay(1);
        }
#else
        model->run(sweep, kSweep);
#endif
    }
    delete[] sweep;

    constexpr float kMargin = 1.6F;
    float kStreamMargin = kMargin;
    constexpr float kHeadMargin = kMargin;
    constexpr float kLayerMargin = kMargin;
    // Header bytes 24..31 were reserved by NAMB v1. A converter may attach a
    // model-calibrated S3 stream scale without changing the model payload or
    // its CRC. Files without it keep the 1.6 default.
    constexpr std::uint32_t kS3StreamMarginMarker = 0x4D533353U; // "S3SM"
    if (read_u32(data + 28U) == kS3StreamMarginMarker) {
        const float encoded = read_float(data + 24U);
        if (std::isfinite(encoded) && encoded >= 1.0F && encoded <= 16.0F) {
            kStreamMargin = encoded;
        }
    }
    constexpr float kStreamMaximum = 1073741823.0F;
    float stream_peak = 0.0F;
    float head_peak = 0.0F;
    for (std::size_t index = 0; index < kLayerCount; ++index) {
        model->max_stream[index] = stable_calibration_peak(model->max_stream[index]);
        model->max_activation[index] = stable_calibration_peak(model->max_activation[index]);
        stream_peak = std::max(stream_peak, model->max_stream[index]);
    }
    for (const float peak : model->max_head_sum) {
        head_peak = std::max(head_peak, stable_calibration_peak(peak));
    }
    stream_scale_ = std::max(stream_peak * kStreamMargin / kStreamMaximum, 1.0e-20F);
    head_grid_ = std::max(head_peak * kHeadMargin / kStreamMaximum, 1.0e-20F);
    if (precision != nullptr) {
        stream_scale_ = read_float(precision + 4U);
        head_grid_ = read_float(precision + 8U);
#if defined(ESP_PLATFORM)
        ESP_LOGI("nam", "S3P1 grids stream=%.9g head=%.9g", static_cast<double>(stream_scale_),
                 static_cast<double>(head_grid_));
#endif
    }

    float rechannel_coefficient_peak = 0.0F;
    for (const float coefficient : model->rechannel) {
        rechannel_coefficient_peak = std::max(rechannel_coefficient_peak,
                                              std::fabs(coefficient / (stream_scale_ * 32768.0F)));
    }
    rechannel_shift_ = std::clamp(static_cast<int>(std::floor(std::log2(
                                      32767.0F / std::max(rechannel_coefficient_peak, 1.0e-20F)))),
                                  0, 15);
    const float rechannel_multiplier =
        std::ldexp(1.0F, rechannel_shift_) / (stream_scale_ * 32768.0F);
    for (std::size_t channel = 0; channel < kChannels; ++channel) {
        rechannel_q15_[channel] = static_cast<std::int16_t>(std::clamp<long>(
            std::lrintf(model->rechannel[channel] * rechannel_multiplier), INT16_MIN, INT16_MAX));
    }
    ring_high_count_ = 0;
    ring_low_count_ = 0;
    convolution_high_count_ = 0;
    convolution_low_count_ = 0;
    residual_high_count_ = 0;
    residual_low_count_ = 0;
    for (std::size_t index = 0; index < kLayerCount; ++index) {
        Layer& layer = layers_[index];
        layer.kernel = kKernelSizes[index];
        layer.dilation = kDilations[index];
        const std::size_t required_capacity =
            (layer.kernel - 1U) * layer.dilation + kMaxStagedFrames;
        layer.capacity = required_capacity;
        layer.mirror = kRingMirror;
        layer.span = layer.capacity + layer.mirror;
        layer.position = 0;
        layer.ring_high_offset = ring_high_count_;
        ring_high_count_ += layer.span * kChannels;
        if (s3_native_low_layer(index)) {
            layer.ring_low_offset = ring_low_count_;
            if (index != 0U) {
                ring_low_count_ += layer.span * kChannels;
            }
        }
        layer.convolution_high_offset = convolution_high_count_;
        convolution_high_count_ += layer.kernel * kChannels * kChannels;
        if (index > 0U && s3_native_low_layer(index)) {
            layer.convolution_low_offset = convolution_low_count_;
            convolution_low_count_ += layer.kernel * kChannels * kChannels;
        }
        layer.residual_high_offset = residual_high_count_;
        residual_high_count_ += kChannels * kChannels;
        if (index > 0U && s3_native_low_layer(index)) {
            layer.residual_low_offset = residual_low_count_;
            residual_low_count_ += kChannels * kChannels;
        }
    }
    convolution_high_stage_b_offset_ = layers_[kStageSplitLayer].convolution_high_offset;
    convolution_low_stage_b_offset_ = layers_[kStageSplitLayer].convolution_low_offset;
    residual_high_stage_b_offset_ = layers_[kStageSplitLayer].residual_high_offset;
    residual_low_stage_b_offset_ = layers_[kStageSplitLayer].residual_low_offset;

    if (ring_high_ == nullptr) {
        ring_high_ = s3_native_allocate<std::int16_t>(ring_high_count_, false);
    }
    // Most runtime rings are allocated after the temporary float parser is
    // released. The S3 layout reserves its largest Core 0 history first
    // to keep it separate from the later Core 1 coefficient bank.
    // The fused LX7 MAC/load pipeline reads four vectors ahead. Keep explicit
    // zero slack after the final layer so that priming never crosses the
    // coefficient allocation.
    constexpr std::size_t kWeightPipelineSlack = 4U * kChannels;
    constexpr auto align_weight_count = [](const std::size_t count) {
        return (count + 7U) & ~std::size_t{7U};
    };
    const std::size_t l0_low_count = layers_[0].kernel * kChannels * kChannels;
    const std::size_t stage_a_low_count = convolution_low_stage_b_offset_;
    const std::size_t stage_b_low_count = convolution_low_count_ - convolution_low_stage_b_offset_;
    const std::size_t stage_a_residual_high_count = residual_high_stage_b_offset_;
    const std::size_t stage_b_residual_high_count =
        residual_high_count_ - residual_high_stage_b_offset_;
    const std::size_t stage_a_residual_low_count = residual_low_stage_b_offset_;
    const std::size_t stage_b_residual_low_count =
        residual_low_count_ - residual_low_stage_b_offset_;

    const std::size_t stage_a_l0_low_at =
        align_weight_count(convolution_high_stage_b_offset_ + kWeightPipelineSlack);
    const std::size_t stage_a_low_at = align_weight_count(stage_a_l0_low_at + l0_low_count);
    const std::size_t stage_a_residual_high_at =
        align_weight_count(stage_a_low_at + stage_a_low_count);
    const std::size_t stage_a_residual_low_at =
        align_weight_count(stage_a_residual_high_at + stage_a_residual_high_count);
    const std::size_t stage_a_hot_count = stage_a_residual_low_at + stage_a_residual_low_count;

    const std::size_t stage_b_high_count =
        convolution_high_count_ - convolution_high_stage_b_offset_;
    const std::size_t stage_b_low_at =
        align_weight_count(stage_b_high_count + kWeightPipelineSlack);
    const std::size_t stage_b_residual_high_at =
        align_weight_count(stage_b_low_at + stage_b_low_count);
    const std::size_t stage_b_residual_low_at =
        align_weight_count(stage_b_residual_high_at + stage_b_residual_high_count);
    const std::size_t stage_b_hot_count = stage_b_residual_low_at + stage_b_residual_low_count;
    // The bank-aligned history is owned by the model and must survive a joint
    // replan of the other histories during this load.
    [[maybe_unused]] std::size_t protected_history = kLayerCount;

#if defined(ESP_PLATFORM)
    // Place the largest Core 0 history before the coefficient banks, so its
    // vector reads do not share Core 1's hot coefficient bank.
    std::size_t largest_stage_a_ring = 0U;
    for (std::size_t index = 1U; index < kStageSplitLayer; ++index) {
        if (layers_[index].span > layers_[largest_stage_a_ring].span) {
            largest_stage_a_ring = index;
        }
    }
    if (!external_workspace_)
        protected_history = largest_stage_a_ring;
    if (!external_workspace_ && ring_high_internal_[largest_stage_a_ring] == nullptr) {
        ring_high_internal_[largest_stage_a_ring] = s3_native_allocate_bank_aligned<std::int16_t>(
            layers_[largest_stage_a_ring].span * kChannels, kS3StageA);
        if (ring_high_internal_[largest_stage_a_ring] == nullptr) {
            delete model;
            release();
            copy_error("Core 0 history bank allocation failed", error_message,
                       error_message_capacity);
            return false;
        }
    }
#endif
    // Place the larger coefficient bank first. With the protected history
    // already reserved, placing the smaller table first can strand the only
    // remaining 21 KiB history region after USB allocations. Both tables keep
    // their own 32 KiB boundaries; their physical address order is irrelevant.
    if (stage_b_hot_count > stage_a_hot_count && convolution_high_stage_b_ == nullptr) {
        convolution_high_stage_b_ = s3_native_allocate_bank_aligned<std::int16_t>(
            stage_b_hot_count, kS3StageB, external_workspace_);
    }
    if (convolution_high_ == nullptr) {
        convolution_high_ = s3_native_allocate_bank_aligned<std::int16_t>(
            stage_a_hot_count, kS3StageA, external_workspace_);
    }
    if (convolution_high_stage_b_ == nullptr) {
        convolution_high_stage_b_ = s3_native_allocate_bank_aligned<std::int16_t>(
            stage_b_hot_count, kS3StageB, external_workspace_);
    }
    if (convolution_high_ != nullptr) {
        convolution_low_l0_s8_ = convolution_high_ + stage_a_l0_low_at;
        convolution_low_ = convolution_high_ + stage_a_low_at;
        residual_high_ = convolution_high_ + stage_a_residual_high_at;
        residual_low_ = convolution_high_ + stage_a_residual_low_at;
    }
    if (convolution_high_stage_b_ != nullptr) {
        convolution_low_stage_b_ = convolution_high_stage_b_ + stage_b_low_at;
        residual_high_stage_b_ = convolution_high_stage_b_ + stage_b_residual_high_at;
        residual_low_stage_b_ = convolution_high_stage_b_ + stage_b_residual_low_at;
    }
    if (ring_high_ == nullptr || convolution_high_ == nullptr ||
        convolution_high_stage_b_ == nullptr || convolution_low_l0_s8_ == nullptr ||
        convolution_low_ == nullptr || convolution_low_stage_b_ == nullptr ||
        residual_high_ == nullptr || residual_high_stage_b_ == nullptr ||
        residual_low_ == nullptr || residual_low_stage_b_ == nullptr) {
#if defined(ESP_PLATFORM)
        ESP_LOGE("nam",
                 "coefficient allocation: ring=%p/%u core0=%p/%u core1=%p/%u; "
                 "internal free=%u largest=%u PSRAM free=%u largest=%u",
                 static_cast<void*>(ring_high_),
                 static_cast<unsigned>(ring_high_count_ * sizeof(*ring_high_)),
                 static_cast<void*>(convolution_high_),
                 static_cast<unsigned>(stage_a_hot_count * sizeof(*convolution_high_)),
                 static_cast<void*>(convolution_high_stage_b_),
                 static_cast<unsigned>(stage_b_hot_count * sizeof(*convolution_high_stage_b_)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
#endif
        delete model;
        release();
        copy_error("S3-native coefficient allocation failed", error_message,
                   error_message_capacity);
        return false;
    }

    head_bias_ = model->head_bias;
    head_scale_ = model->head_scale;
    float head_weight_peak = 0.0F;
    for (const float weight : model->head) {
        head_weight_peak = std::max(head_weight_peak, std::fabs(weight));
    }
    head_weight_shift_ = std::clamp(
        static_cast<int>(std::floor(std::log2(4095.0F / std::max(head_weight_peak, 1.0e-20F)))), 0,
        15);
    const float head_weight_multiplier = std::ldexp(1.0F, head_weight_shift_);
    for (std::size_t at = 0; at < model->head.size(); ++at) {
        head_q15_[at] = static_cast<std::int16_t>(std::clamp<long>(
            std::lrintf(model->head[at] * head_weight_multiplier), INT16_MIN, INT16_MAX));
    }
    for (std::size_t index = 0; index < kLayerCount; ++index) {
        Layer& layer = layers_[index];
        const FloatModel::Layer& source = model->layers[index];
        const float ring_minimum =
            std::max(model->max_stream[index] * kLayerMargin / 2097151.0F, 1.0e-20F);
        layer.ring_shift = ceil_log2_ratio(ring_minimum, stream_scale_);
        if (precision != nullptr) {
            layer.ring_shift = static_cast<int>(read_u32(precision + 12U + index * 48U));
        }
        const float ring_scale = std::ldexp(stream_scale_, layer.ring_shift);
        const float activation_maximum = static_cast<float>(s3_native_activation_maximum(index));
        const float activation_minimum =
            std::max(model->max_activation[index] * kLayerMargin / activation_maximum, 1.0e-20F);
        layer.head_shift = ceil_log2_ratio(activation_minimum, head_grid_);
        if (precision != nullptr) {
            layer.head_shift = static_cast<std::int32_t>(read_u32(precision + 16U + index * 48U));
        }
        const float activation_scale = std::ldexp(head_grid_, layer.head_shift);
        float convolution_peak = 0.0F;
        for (std::size_t at = 0; at < layer.kernel * kChannels * kChannels; ++at) {
            convolution_peak =
                std::max(convolution_peak, std::fabs(source.convolution[at] * ring_scale));
        }
        const float convolution_minimum = std::max(convolution_peak / 32767.0F, 1.0e-20F);
        layer.convolution_shift =
            static_cast<int>(std::floor(std::log2(activation_scale / convolution_minimum)));
        if (precision != nullptr) {
            layer.convolution_shift = static_cast<int>(read_u32(precision + 20U + index * 48U));
        }
        const float convolution_scale = std::ldexp(activation_scale, -layer.convolution_shift);
        constexpr int kLowWeightShift = 9;
        layer.convolution_low_shift = layer.convolution_shift - kLowWeightShift;

        auto* const high_weights = convolution_high_for_layer(index);
        for (std::size_t at = 0; at < layer.kernel * kChannels * kChannels; ++at) {
            high_weights[at] = static_cast<std::int16_t>(std::clamp<long>(
                std::lrintf(source.convolution[at] * ring_scale / convolution_scale), INT16_MIN,
                INT16_MAX));
        }
        if (index == 0U) {
            for (std::size_t tap = 0; tap < layer.kernel; ++tap) {
                for (std::size_t pair = 0; pair < 4U; ++pair) {
                    for (std::size_t out = 0; out < kChannels; ++out) {
                        for (std::size_t half = 0; half < 2U; ++half) {
                            const std::size_t input = pair * 2U + half;
                            const std::size_t source_at = tap * 64U + input * 8U + out;
                            const std::size_t packed_at = source_at;
                            const std::int32_t weight = high_weights[source_at];
                            const std::int32_t rounded =
                                weight >= 0 ? (weight + 256) >> 9 : -(((-weight) + 256) >> 9);
                            const std::int32_t quantized =
                                std::clamp(rounded, static_cast<std::int32_t>(INT8_MIN),
                                           static_cast<std::int32_t>(INT8_MAX));
#if defined(ESP_PLATFORM)
                            convolution_low_l0_s8_[packed_at] =
                                static_cast<std::int16_t>(quantized * 4);
#else
                            convolution_low_l0_s8_[packed_at] =
                                static_cast<std::int16_t>(quantized);
#endif
                        }
                    }
                }
            }
        }
        if (index > 0U && s3_native_low_layer(index)) {
            auto* const low_weights = convolution_low_for_layer(index);
            const float low_scale = std::ldexp(convolution_scale, kLowWeightShift);
            for (std::size_t tap = 0; tap < layer.kernel; ++tap) {
                for (std::size_t pair = 0; pair < 4; ++pair) {
                    for (std::size_t out = 0; out < kChannels; ++out) {
                        for (std::size_t half = 0; half < 2; ++half) {
                            const std::size_t input = pair * 2U + half;
                            const std::size_t source_at = tap * 64U + input * 8U + out;
                            const std::size_t packed_at = source_at;
                            const long quantized = std::clamp<long>(
                                std::lrintf(source.convolution[source_at] * ring_scale / low_scale),
                                INT8_MIN, INT8_MAX);
#if defined(ESP_PLATFORM)
                            low_weights[packed_at] = static_cast<std::int16_t>(quantized * 4L);
#else
                            low_weights[packed_at] = static_cast<std::int16_t>(quantized);
#endif
                        }
                    }
                }
            }
        }

        float residual_peak = 0.0F;
        for (const float weight : source.residual) {
            residual_peak = std::max(residual_peak, std::fabs(weight * activation_scale));
        }
        const float residual_minimum = std::max(residual_peak / 32767.0F, 1.0e-20F);
        layer.residual_shift =
            static_cast<int>(std::floor(std::log2(stream_scale_ / residual_minimum)));
        if (precision != nullptr) {
            layer.residual_shift = static_cast<int>(read_u32(precision + 24U + index * 48U) &
                                                    (tuned_narrowing ? 63U : UINT32_MAX));
#if defined(ESP_PLATFORM)
            ESP_LOGI("nam", "S3P1 layer=%u ring=%d head=%d conv=%d res=%d",
                     static_cast<unsigned>(index), layer.ring_shift, layer.head_shift,
                     layer.convolution_shift, layer.residual_shift);
#endif
        }
        const float residual_scale = std::ldexp(stream_scale_, -layer.residual_shift);
        layer.residual_low_shift = layer.residual_shift - kLowWeightShift;
        auto* const high_residual = residual_high_for_layer(index);
        for (std::size_t at = 0; at < 64U; ++at) {
            high_residual[at] = static_cast<std::int16_t>(std::clamp<long>(
                std::lrintf(source.residual[at] * activation_scale / residual_scale), INT16_MIN,
                INT16_MAX));
        }
        // The fast late-layer residual path extracts a signed-S16 QACC lane
        // and widens it back into the int32 stream. Derive the minimum safe
        // extraction shift from the quantized weights and the exact range of
        // the A22 high limb. This is a proof over every possible activation
        // vector; it needs neither an audio probe nor a per-model table.
        if (!s3_native_a16_layer(index)) {
            const std::int64_t kActivationHighMinimum =
                full_activation_lanes && index != 0U ? -32768 : -16384;
            const std::int64_t kActivationHighMaximum =
                full_activation_lanes && index != 0U ? 32767 : 16383;
            std::int64_t largest_positive = 0;
            std::int64_t smallest_negative = 0;
            for (std::size_t out = 0; out < kChannels; ++out) {
                std::int64_t positive = 0;
                std::int64_t negative = 0;
                for (std::size_t input = 0; input < kChannels; ++input) {
                    const std::int64_t weight = high_residual[input * kChannels + out];
                    if (weight >= 0) {
                        positive += weight * kActivationHighMaximum;
                        negative += weight * kActivationHighMinimum;
                    } else {
                        positive += weight * kActivationHighMinimum;
                        negative += weight * kActivationHighMaximum;
                    }
                }
                largest_positive = std::max(largest_positive, positive);
                smallest_negative = std::min(smallest_negative, negative);
            }
            const int base_shift = layer.residual_shift - 7;
            int total_shift = std::max(base_shift, 0);
            while (total_shift < 31 && (largest_positive > (INT64_C(32767) << total_shift) ||
                                        -smallest_negative > (INT64_C(32768) << total_shift))) {
                ++total_shift;
            }
            layer.residual_narrow_shift = total_shift - base_shift;
            if (tuned_narrowing && precision != nullptr) {
                const auto packed = read_u32(precision + 24U + index * 48U);
                if (packed & 0x00010000U)
                    layer.residual_narrow_shift = static_cast<int>((packed >> 8U) & 31U);
            }
        } else {
            layer.residual_narrow_shift = 0;
        }
        if (index > 0U && s3_native_low_layer(index)) {
            auto* const low_residual = residual_low_for_layer(index);
            const float low_scale = std::ldexp(residual_scale, kLowWeightShift);
            for (std::size_t pair = 0; pair < 4; ++pair) {
                for (std::size_t out = 0; out < kChannels; ++out) {
                    for (std::size_t half = 0; half < 2; ++half) {
                        const std::size_t input = pair * 2U + half;
                        const long quantized =
                            std::clamp<long>(std::lrintf(source.residual[input * 8U + out] *
                                                         activation_scale / low_scale),
                                             INT8_MIN, INT8_MAX);
#if defined(ESP_PLATFORM)
                        // The fused LX7 kernel accumulates high and low residual
                        // limbs together. Their calibrated shifts differ by exactly
                        // two bits, so scale the low table once instead of paying for
                        // a second QACC extraction on every audio frame.
                        low_residual[input * kChannels + out] =
                            static_cast<std::int16_t>(quantized * 4L);
#else
                        low_residual[input * kChannels + out] =
                            static_cast<std::int16_t>(quantized);
#endif
                    }
                }
            }
        }

        std::array<std::int32_t, kChannels> mixin_q16{};
        std::int32_t maximum_mixin = 0;
        for (std::size_t out = 0; out < kChannels; ++out) {
            layer.activation_bias[out] = quantize_i32(source.bias[out] / activation_scale);
            if (precision != nullptr) {
                layer.activation_bias[out] =
                    static_cast<std::int32_t>(read_u32(precision + 28U + index * 48U + out * 4U));
            }
            mixin_q16[out] =
                quantize_i32(source.mixin[out] / (activation_scale * 32768.0F) * 65536.0F);
            maximum_mixin = std::max(maximum_mixin, std::abs(mixin_q16[out]));
            layer.residual_bias[out] = quantize_i32(source.residual_bias[out] / stream_scale_);
        }
        layer.activation_mixin_shift =
            std::max(0, ceil_log2_ratio(static_cast<float>(maximum_mixin), 32767.0F));
        const float mixin_divisor = std::ldexp(1.0F, layer.activation_mixin_shift);
        for (std::size_t out = 0; out < kChannels; ++out) {
            layer.activation_mixin[out] = static_cast<std::int16_t>(
                std::clamp<long>(std::lrintf(static_cast<float>(mixin_q16[out]) / mixin_divisor),
                                 INT16_MIN, INT16_MAX));
        }
        const int cached_affine_shift = layer.convolution_shift - 7;
        layer.cached_affine_safe =
            cached_affine_shift >= 0 && cached_affine_shift <= 14 &&
            std::all_of(layer.activation_bias.begin(), layer.activation_bias.end(),
                        [](const std::int32_t value) noexcept {
                            return value >= INT16_MIN && value <= INT16_MAX;
                        });

        std::uint32_t quantized_mixin_peak = 0U;
        for (const std::int16_t mixin : layer.activation_mixin) {
            const std::int32_t value = mixin;
            const std::uint32_t magnitude = static_cast<std::uint32_t>(value < 0 ? -value : value);
            quantized_mixin_peak = std::max(quantized_mixin_peak, magnitude);
        }
        const int mixin_sar = 16 - layer.activation_mixin_shift;
        if (quantized_mixin_peak == 0U) {
            layer.narrow_mixin_input_limit = 32768U;
        } else if (mixin_sar >= 0) {
            const std::uint64_t largest_safe_product = UINT64_C(32767)
                                                       << static_cast<unsigned>(mixin_sar);
            layer.narrow_mixin_input_limit = static_cast<std::uint32_t>(
                std::min<std::uint64_t>(32768U, largest_safe_product / quantized_mixin_peak));
        } else {
            layer.narrow_mixin_input_limit = 0U;
        }
    }

    // Place the rings greedily first. If a warm heap cannot fit them, release
    // the partial batch and plan it jointly.
    if (ring_low_ == nullptr)
        ring_low_ = s3_native_allocate<std::uint8_t>(ring_low_count_, !external_workspace_);
    if (ring_low_l0_s8_ == nullptr)
        ring_low_l0_s8_ =
            s3_native_allocate<std::int8_t>(layers_[0].span * kChannels, !external_workspace_);
    constexpr std::array<std::size_t, 23> kInternalRingAllocationOrder{
        6, 13, 22, 5, 12, 21, 11, 20, 4, 15, 3, 10, 19, 2, 9, 18, 1, 8, 17, 14, 0, 7, 16,
    };
    // A history outside kInternalHighRingMask is read straight from the PSRAM
    // shadow ring_high_, so it must not be allocated internally and its absence
    // is not a failure.
    const auto internal_high_ring = [](const std::size_t index) noexcept {
#if defined(ESP_PLATFORM)
        return (kInternalHighRingMask & (1U << index)) != 0U;
#else
        (void)index;
        return true;
#endif
    };
    bool internal_high_failed = false;
    for (const std::size_t index : kInternalRingAllocationOrder) {
        if (!internal_high_ring(index))
            continue;
        if (ring_high_internal_[index] == nullptr) {
            // Fill the tails the bank-aligned blocks left behind before asking
            // the heap. Every byte placed here is a byte the joint planner no
            // longer has to find a bank-shaped hole for.
            const std::size_t ring_count = layers_[index].span * kChannels;
            const std::uint8_t ring_stage = index < kStageSplitLayer ? kS3StageA : kS3StageB;
            void* const tail =
                external_workspace_
                    ? nullptr
                    : s3_bank_arena_take_tail(ring_count * sizeof(std::int16_t), ring_stage);
            ring_high_internal_[index] =
                tail != nullptr
                    ? static_cast<std::int16_t*>(tail)
                    : s3_native_allocate<std::int16_t>(ring_count, !external_workspace_);
        }
        internal_high_failed |= ring_high_internal_[index] == nullptr;
    }
#if defined(ESP_PLATFORM)
    if (!external_workspace_ && (!ring_low_ || !ring_low_l0_s8_ || internal_high_failed)) {
        s3_native_free(ring_low_);
        s3_native_free(ring_low_l0_s8_);
        for (std::size_t i = 0; i < kLayerCount; ++i) {
            if (i != protected_history)
                s3_native_free(ring_high_internal_[i]);
        }
        constexpr std::size_t capacity = kLayerCount + 2;
        std::size_t sizes[capacity]{ring_low_count_, layers_[0].span * kChannels};
        std::size_t indices[kLayerCount]{};
        std::size_t count = 2;
        void* blocks[capacity]{};
        for (std::size_t i = 0; i < kLayerCount; ++i) {
            // protected_history and every history the arena placed are still
            // allocated -- the frees above leave them alone -- so they are not
            // part of the plan.
            if (i == protected_history || ring_high_internal_[i] != nullptr ||
                !internal_high_ring(i)) {
                continue;
            }
            indices[count - 2] = i;
            sizes[count++] = layers_[i].span * kChannels * sizeof(std::int16_t);
        }
        internal_high_failed = !coyopedal_nam_internal_batch(count, sizes, blocks);
        ring_low_ = static_cast<std::uint8_t*>(blocks[0]);
        ring_low_l0_s8_ = static_cast<std::int8_t*>(blocks[1]);
        for (std::size_t i = 2; i < count; ++i)
            ring_high_internal_[indices[i - 2]] = static_cast<std::int16_t*>(blocks[i]);
    }
#endif
    if (ring_low_ == nullptr || ring_low_l0_s8_ == nullptr || internal_high_failed) {
#if defined(ESP_PLATFORM)
        ESP_LOGE("nam",
                 "S3 runtime alloc failed rl8=%p/%u "
                 "free=%u largest=%u whA=%p whB=%p",
                 static_cast<void*>(ring_low_),
                 static_cast<unsigned>(ring_low_count_ * sizeof(*ring_low_)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                 static_cast<void*>(convolution_high_),
                 static_cast<void*>(convolution_high_stage_b_));
        for (std::size_t index = 0; index < kLayerCount; ++index) {
            if (internal_high_ring(index) && ring_high_internal_[index] == nullptr) {
                ESP_LOGE(
                    "nam", "unallocated history[%u]: %u bytes", static_cast<unsigned>(index),
                    static_cast<unsigned>(layers_[index].span * kChannels * sizeof(std::int16_t)));
            }
        }
#endif
        delete model;
        release();
        copy_error("S3-native runtime allocation failed", error_message, error_message_capacity);
        return false;
    }
#if defined(ESP_PLATFORM)
    ESP_LOGI("nam",
             "S3MEM whA=%p/%u whB=%p/%u wlA=%p wlB=%p "
             "rhA=%p rhB=%p rlA=%p rlB=%p",
             static_cast<void*>(convolution_high_),
             static_cast<unsigned>(convolution_high_stage_b_offset_ * sizeof(*convolution_high_)),
             static_cast<void*>(convolution_high_stage_b_),
             static_cast<unsigned>((convolution_high_count_ - convolution_high_stage_b_offset_) *
                                   sizeof(*convolution_high_stage_b_)),
             static_cast<void*>(convolution_low_), static_cast<void*>(convolution_low_stage_b_),
             static_cast<void*>(residual_high_), static_cast<void*>(residual_high_stage_b_),
             static_cast<void*>(residual_low_), static_cast<void*>(residual_low_stage_b_));
    for (std::size_t index = 0; index < kLayerCount; ++index) {
        ESP_LOGI("nam", "S3MEM ring[%02u]=%p/%u weight=%p", static_cast<unsigned>(index),
                 static_cast<void*>(ring_high_internal_[index]),
                 static_cast<unsigned>(layers_[index].span * kChannels *
                                       sizeof(*ring_high_internal_[index])),
                 static_cast<void*>(convolution_high_for_layer(index)));
    }
    // The graph is placed; the display, the effect lines and the tuner pump are
    // next in line for internal SRAM and they get the rest. Only the live
    // graph's placement decides what the arena no longer needs.
    if (!external_workspace_) {
        coyopedal_nam_bank_arena_release_unused();
    }
#endif

    loaded_ = true;
    tuned_narrowing_ = tuned_narrowing;
    full_activation_lanes_ = full_activation_lanes;
    model_crc_ = crc32(data, kFileSize);
    exact_residual_mask_ = prepared_mask;
    delete model;
    reset();
    copy_error("", error_message, error_message_capacity);
    return true;
}

void A2FullS3Native::reset() noexcept {
    if (ring_high_ != nullptr) {
        std::fill_n(ring_high_, ring_high_count_, std::int16_t{});
    }
    for (std::size_t index = 0; index < kLayerCount; ++index) {
        if (ring_high_internal_[index] != nullptr) {
            std::fill_n(ring_high_internal_[index], layers_[index].span * kChannels,
                        std::int16_t{});
        }
    }
    if (ring_low_ != nullptr) {
        std::fill_n(ring_low_, ring_low_count_, std::uint8_t{});
    }
    if (ring_low_l0_s8_ != nullptr) {
        std::fill_n(ring_low_l0_s8_, layers_[0].span * kChannels, std::int8_t{});
    }
    for (Layer& layer : layers_) {
        layer.position = 0;
    }
    head_history_q15_ = {};
    head_position_ = 0;
    overflows_ = 0;
    if (!loaded_ || defer_tuning_prewarm_) {
        return;
    }
    constexpr std::size_t kPrewarmSamples = 8192U;
    std::array<float, kMaxStagedFrames> silence{};
    std::size_t remaining = kPrewarmSamples;
#if defined(ESP_PLATFORM)
    std::size_t blocks = 0U;
#endif
    while (remaining > 0U) {
        const std::size_t frames = std::min(remaining, silence.size());
        silence.fill(0.0F);
        process(silence.data(), frames);
        remaining -= frames;
#if defined(ESP_PLATFORM)
        if ((++blocks & 15U) == 0U) {
            vTaskDelay(1);
        }
#endif
    }
    overflows_ = 0;
}

// This finite convolutional network reaches a constant state on zero input.
// Seed each layer's complete ring from the preceding layer's constant output,
// using the same native kernels, then restore the normal warm-up positions.
// This is used only by the import tuner; normal model reset is unchanged.
void A2FullS3Native::reset_tuning_steady(BlockScratch& scratch) noexcept {
    constexpr std::size_t warm_frames = 8192U;
    constexpr std::size_t settle_frames = [] {
        std::size_t result = kMaxStagedFrames + kHeadKernel;
        for (std::size_t i = 0; i < kLayerCount; ++i)
            result += (kKernelSizes[i] - 1U) * kDilations[i];
        return result;
    }();
    static_assert(settle_frames < warm_frames);
    const bool deferred = defer_tuning_prewarm_;
    defer_tuning_prewarm_ = true;
    reset();
    defer_tuning_prewarm_ = deferred;
    if (!loaded_)
        return;
    std::array<float, kMaxStagedFrames> silence{};
    begin_block(silence.data(), silence.size(), scratch);
    for (std::size_t index = 0; index < kLayerCount; ++index) {
        const auto& layer = layers_[index];
#if defined(ESP_PLATFORM)
        std::int16_t* high = (kInternalHighRingMask & (1U << index)) != 0U
                                 ? ring_high_internal_[index]
                                 : ring_high_ + layer.ring_high_offset;
#else
        std::int16_t* high = ring_high_ + layer.ring_high_offset;
#endif
        for (std::size_t row = 1; row < layer.span; ++row)
            std::memcpy(high + row * kChannels, high, kChannels * sizeof(*high));
        if (s3_native_low_layer(index)) {
            auto* low = index == 0U ? reinterpret_cast<std::uint8_t*>(ring_low_l0_s8_)
                                    : ring_low_ + layer.ring_low_offset;
            for (std::size_t row = 1; row < layer.span; ++row)
                std::memcpy(low + row * kChannels, low, kChannels);
        }
        process_layers(scratch, index, index + 1U);
    }
    finish_block(scratch, silence.data());
    for (auto& layer : layers_)
        layer.position = warm_frames % layer.capacity;
    head_position_ = warm_frames % kHeadKernel;
    overflows_ = 0;
}

// The three per-block entry points below are small (60 to 500 bytes) but run
// on both stages every block. From flash they share the 16 KiB instruction
// cache with everything else, and the UI booting from PSRAM evicts them every
// block. IRAM is the placement that does not depend on what else the part is
// doing.
#if defined(ESP_PLATFORM)
#define COYOPEDAL_S3_BLOCK_HOT __attribute__((section(".iram1.a2_full_hot")))
#else
#define COYOPEDAL_S3_BLOCK_HOT
#endif

COYOPEDAL_S3_BLOCK_HOT
void A2FullS3Native::begin_block(const float* const samples, const std::size_t frames,
                                 BlockScratch& scratch) noexcept {
    scratch.frames = std::min(frames, kMaxStagedFrames);
#if defined(ESP_PLATFORM)
    extern std::uint32_t s3_a2full_native_input_q15(
        const float* input, std::int16_t* output,
        std::int32_t frames) noexcept asm("s3_a2full_native_input_q15");
    extern void s3_a2full_native_rechannel8(
        const std::int16_t* input, const std::int16_t* weights, std::int32_t shift,
        std::int32_t* stream, std::int32_t* head_sum,
        std::int32_t frames) noexcept asm("s3_a2full_native_rechannel8");
    scratch.input_peak = s3_a2full_native_input_q15(samples, scratch.input_q,
                                                    static_cast<std::int32_t>(scratch.frames));
    s3_a2full_native_rechannel8(scratch.input_q, rechannel_q15_.data(), rechannel_shift_,
                                scratch.stream[0], scratch.head_sum[0],
                                static_cast<std::int32_t>(scratch.frames));
#else
    scratch.input_peak = 0U;
    for (std::size_t frame = 0; frame < scratch.frames; ++frame) {
        const float condition = samples[frame];
        scratch.input_q[frame] = static_cast<std::int16_t>(
            std::clamp<long>(std::lrintf(condition * 32768.0F), INT16_MIN, INT16_MAX));
        const std::int32_t input_q = scratch.input_q[frame];
        const std::uint32_t input_magnitude =
            static_cast<std::uint32_t>(input_q < 0 ? -input_q : input_q);
        scratch.input_peak = std::max(scratch.input_peak, input_magnitude);
        for (std::size_t channel = 0; channel < kChannels; ++channel) {
            const std::int32_t product =
                static_cast<std::int32_t>(scratch.input_q[frame]) * rechannel_q15_[channel];
            if (rechannel_shift_ == 0) {
                scratch.stream[frame][channel] = product;
            } else {
                const std::int32_t half = 1 << (rechannel_shift_ - 1);
                scratch.stream[frame][channel] = product >= 0
                                                     ? (product + half) >> rechannel_shift_
                                                     : -(((-product) + half) >> rechannel_shift_);
            }
            scratch.head_sum[frame][channel] = 0;
        }
    }
#endif
}

namespace {
#if !defined(ESP_PLATFORM)
inline std::int32_t s3_native_prelu_rational_i32(const std::int32_t value) noexcept {
    if (value >= 0) {
        return value;
    }
    // The fused LX7 kernels approximate the 0.01 slope as 41/4096.
    const std::int32_t approximation = (value >> 7) + (value >> 9) + (value >> 12);
    // ACTIVATE_VECTOR_RATIONAL selects VMAX(x, approximation). Usually the
    // approximation is closer to zero, but for tiny negative x the three
    // arithmetic shifts can sum below x. Preserve that hardware corner case.
    return std::max(value, approximation);
}
#else
struct alignas(16) S3NativeHeadQuantizeConstants {
    std::int32_t half[4];
    std::int32_t minimum[4];
    std::int32_t maximum[4];
    std::int16_t dot_round_a[8];
    std::int16_t dot_round_b[8];
};

DRAM_ATTR const S3NativeHeadQuantizeConstants kS3NativeHeadQuantizeConstants{
    {16384, 16384, 16384, 16384},
    {INT16_MIN, INT16_MIN, INT16_MIN, INT16_MIN},
    {INT16_MAX, INT16_MAX, INT16_MAX, INT16_MAX},
    {1024, 1024, 1024, 1024, 1024, 1024, 1024, 1024},
    {8, 8, 8, 8, 8, 8, 8, 8},
};
static_assert(sizeof(S3NativeHeadQuantizeConstants) == 80);

struct S3NativeHeadBlockArgs {
    const std::int32_t* input;
    std::int16_t* history;
    const std::int16_t* weights;
    const S3NativeHeadQuantizeConstants* constants;
    std::int32_t* lane_sums;
    std::int32_t frames;
    std::int32_t position;
};
static_assert(sizeof(S3NativeHeadBlockArgs) == 28);

extern "C" std::int32_t s3_a2full_native_head_block(const S3NativeHeadBlockArgs* args) noexcept;

// One fused layer call over a block. This mirrors the argument table
// in s3_native_fused_layer8.S exactly; the size assertion keeps a field edit
// from silently moving every pointer the assembly reads. next_low_enabled is
// not read by the kernels, and residual_narrow_shift only by the narrow ones.
struct S3NativeFusedLayer8Args {
    const std::int16_t* high_run0;
    std::int32_t count0;
    const std::int16_t* high_weights;
    std::int32_t high_stride;
    std::int32_t high_shift;
    const std::int16_t* high_run1;
    std::int32_t count1;
    const void* low_run0;
    const void* low_weights;
    std::int32_t low_stride;
    std::int32_t low_shift;
    const void* low_run1;
    const std::int16_t* input_q;
    const std::int16_t* mixin;
    const std::int32_t* activation_bias;
    std::int32_t mixin_sar;
    std::int32_t* head_sum;
    std::int32_t head_shift;
    const std::int16_t* residual_high;
    const std::int16_t* residual_low;
    const std::int32_t* residual_bias;
    std::int32_t residual_high_shift;
    std::int32_t residual_low_shift;
    std::int32_t* stream;
    std::int16_t* next_ring_high;
    void* next_ring_low;
    std::int32_t next_ring_shift;
    std::int32_t next_low_enabled;
    std::int32_t frames;
    std::int32_t residual_narrow_shift;
};
static_assert(sizeof(S3NativeFusedLayer8Args) == 120);

using S3NativeFusedLayer8Fn = void (*)(const S3NativeFusedLayer8Args*) noexcept;
extern "C" void s3_a2full_fused_layer8_l0_s8(const S3NativeFusedLayer8Args* args) noexcept;
extern "C" void s3_a2full_fused_layer8_low_s8(const S3NativeFusedLayer8Args* args) noexcept;
extern "C" void
s3_a2full_fused_layer8_low_s8_conv_s16_guarded(const S3NativeFusedLayer8Args* args) noexcept;
extern "C" void
s3_a2full_fused_layer8_low_s8_next_high(const S3NativeFusedLayer8Args* args) noexcept;
extern "C" void
s3_a2full_fused_layer8_low_s8_wide_mixin_trunc(const S3NativeFusedLayer8Args* args) noexcept;
extern "C" void s3_a2full_fused_layer8_low_s8_conv_s16_guarded_wide_mixin_trunc(
    const S3NativeFusedLayer8Args* args) noexcept;
extern "C" void s3_a2full_fused_layer8_low_s8_next_high_wide_mixin_trunc(
    const S3NativeFusedLayer8Args* args) noexcept;
extern "C" void s3_a2full_fused_layer8_high_narrow(const S3NativeFusedLayer8Args* args) noexcept;
extern "C" void
s3_a2full_fused_layer8_high_narrow_uncached(const S3NativeFusedLayer8Args* args) noexcept;
extern "C" void
s3_a2full_fused_layer8_high_narrow_wide_mixin_trunc(const S3NativeFusedLayer8Args* args) noexcept;
extern "C" void s3_a2full_fused_layer8_high_narrow_wide_mixin_trunc_uncached(
    const S3NativeFusedLayer8Args* args) noexcept;
extern "C" void
s3_a2full_fused_layer8_high_exact_uncached(const S3NativeFusedLayer8Args* args) noexcept;
extern "C" void
s3_a2full_fused_layer8_high_exact_wide_mixin_trunc(const S3NativeFusedLayer8Args* args) noexcept;
extern "C" void
s3_a2full_fused_layer8_final_conv_s16_guarded(const S3NativeFusedLayer8Args* args) noexcept;
extern "C" void
s3_a2full_fused_layer8_final_cached_conv_s16_guarded(const S3NativeFusedLayer8Args* args) noexcept;
extern "C" void s3_a2full_fused_layer8_final_wide_mixin_trunc_conv_s16_guarded(
    const S3NativeFusedLayer8Args* args) noexcept;
extern "C" void s3_a2full_fused_layer8_final_cached_wide_mixin_trunc_conv_s16_guarded(
    const S3NativeFusedLayer8Args* args) noexcept;

#endif
} // namespace

#if defined(ESP_PLATFORM)
// run_layer is the dispatch around the IRAM kernels and runs on both cores
// every block, so it lives in IRAM where the 16 KiB shared instruction
// cache cannot lose it to the USB interrupt, the effects or the UI.
__attribute__((section(".iram1.a2_full_hot"), aligned(4)))
#endif
void A2FullS3Native::run_layer(BlockScratch& scratch, const std::size_t index,
                               const std::size_t first_frame, const std::size_t frame_count,
                               const bool ring_ready, const bool direct_next) noexcept {
    Layer& layer = layers_[index];
    const bool a16_activation = s3_native_a16_layer(index);
    const bool low_enabled = s3_native_low_layer(index);
    std::int16_t* const high_ring =
#if defined(ESP_PLATFORM)
        (kInternalHighRingMask & (1U << index)) != 0U ? ring_high_internal_[index]
                                                      : ring_high_ + layer.ring_high_offset;
#else
        ring_high_ + layer.ring_high_offset;
#endif
    std::uint8_t* const low_ring_storage = index == 0U
                                               ? reinterpret_cast<std::uint8_t*>(ring_low_l0_s8_)
                                               : ring_low_ + layer.ring_low_offset;
    std::int8_t* const low_ring_s8 = reinterpret_cast<std::int8_t*>(low_ring_storage);
    const std::size_t frames = frame_count;

#if !defined(ESP_PLATFORM)
    // Instruction-level host twin of the fused LX7 kernels. It reproduces their
    // combined-QACC arithmetic: truncating convolution extraction, positively
    // biased QACC residual rounding, the calibrated late-layer S16 narrowing,
    // and A22 ring splits.
    constexpr std::int32_t kRingMaximum = 2097151;
    const std::int32_t activation_maximum = s3_native_activation_maximum(index);
    const auto* const high_weights = convolution_high_for_layer(index);
    const auto* const low_weights = low_enabled ? convolution_low_for_layer(index) : nullptr;
    const auto* const high_residual = residual_high_for_layer(index);
    const auto* const low_residual =
        index != 0U && low_enabled ? residual_low_for_layer(index) : nullptr;
    const bool wide_mixin = scratch.input_peak > layer.narrow_mixin_input_limit;

    for (std::size_t frame = 0; frame < frames; ++frame) {
        const std::size_t position = layer.position;
        if (!ring_ready) {
            for (std::size_t channel = 0; channel < kChannels; ++channel) {
                if (low_enabled) {
                    const std::int32_t shifted =
                        scratch.stream[first_frame + frame][channel] >> layer.ring_shift;
                    const std::int32_t q =
                        index == 0U ? std::clamp(shifted, -kRingMaximum, kRingMaximum) : shifted;
                    const std::int16_t high = static_cast<std::int16_t>(q >> 7);
                    high_ring[position * kChannels + channel] = high;
                    low_ring_s8[position * kChannels + channel] =
                        static_cast<std::int8_t>(q - static_cast<std::int32_t>(high) * 128);
                } else {
                    high_ring[position * kChannels + channel] = static_cast<std::int16_t>(
                        scratch.stream[first_frame + frame][channel] >> (layer.ring_shift + 7));
                }
            }
        }

        // At most 120 signed-S16 products plus 120 S8*S16*4 terms.
        // Even the absolute-sum bound is below signed QACC40 capacity, so
        // intermediate saturation is impossible in these zero-initialized dots.
        std::int64_t convolution_lanes[kChannels]{};
        for (std::size_t tap = 0; tap < layer.kernel; ++tap) {
            const std::size_t delay = (layer.kernel - 1U - tap) * layer.dilation;
            const std::size_t at = (position + layer.capacity - delay) % layer.capacity;
            for (std::size_t input = 0; input < kChannels; ++input) {
                const std::int64_t high = high_ring[at * kChannels + input];
                const auto* weights = high_weights + tap * 64U + input * kChannels;
                for (std::size_t out = 0; out < kChannels; ++out)
                    convolution_lanes[out] += high * weights[out];
            }
        }
        if (low_enabled) {
            for (std::size_t tap = 0; tap < layer.kernel; ++tap) {
                const std::size_t delay = (layer.kernel - 1U - tap) * layer.dilation;
                const std::size_t at = (position + layer.capacity - delay) % layer.capacity;
                for (std::size_t input = 0; input < kChannels; ++input) {
                    const std::int64_t low = low_ring_s8[at * kChannels + input];
                    const auto* weights = low_weights + tap * 64U + input * kChannels;
                    for (std::size_t out = 0; out < kChannels; ++out)
                        convolution_lanes[out] += low * weights[out] * 4;
                }
            }
        }
        std::int16_t activation_high[kChannels]{};
        std::int8_t activation_low[kChannels]{};
        for (std::size_t out = 0; out < kChannels; ++out) {
            std::int32_t affine = static_cast<std::int32_t>(
                host_s3_qacc40_trunc_extract(convolution_lanes[out], layer.convolution_shift - 7));
            affine = host_s3_sat_add32(affine, layer.activation_bias[out]);
            const std::int32_t product =
                static_cast<std::int32_t>(scratch.input_q[first_frame + frame]) *
                layer.activation_mixin[out];
            const int mixin_shift = 16 - layer.activation_mixin_shift;
            std::int32_t mixin = mixin_shift >= 0
                                     ? product >> mixin_shift
                                     : static_cast<std::int32_t>(static_cast<std::uint32_t>(product)
                                                                 << -mixin_shift);
            if (!wide_mixin) {
                // EE.VMUL.S16 keeps the low 16 bits of each shifted product;
                // it does not saturate the result to signed 16-bit range.
                mixin = static_cast<std::int16_t>(mixin);
            }
            affine = host_s3_sat_add32(affine, mixin);

            std::int32_t activated;
            if (index == 0U) {
                // Only layer 0's exact epilogue executes an explicit A22 clamp.
                const std::int32_t leaky = affine >= 0 ? affine : -(((-affine) + 50) / 100);
                activated = std::clamp(leaky, -activation_maximum, activation_maximum);
            } else {
                // The rational vector epilogue used by layers 1..22 relies on
                // its qualified range and keeps the full signed int32 result.
                activated = s3_native_prelu_rational_i32(affine);
            }
            activation_high[out] = a16_activation ? static_cast<std::int16_t>(activated)
                                                  : static_cast<std::int16_t>(activated >> 7);
            activation_low[out] = a16_activation ? 0 : static_cast<std::int8_t>(activated & 127);
            const std::int32_t head_delta = static_cast<std::int32_t>(
                static_cast<std::uint32_t>(activated) << layer.head_shift);
            scratch.head_sum[first_frame + frame][out] =
                host_s3_sat_add32(scratch.head_sum[first_frame + frame][out], head_delta);
        }

        if (index + 1U != kLayerCount) {
            std::int64_t residual_lanes[kChannels]{};
            for (std::size_t input = 0; input < kChannels; ++input) {
                const std::int64_t high = activation_high[input];
                const auto* weights = high_residual + input * kChannels;
                for (std::size_t out = 0; out < kChannels; ++out)
                    residual_lanes[out] += high * weights[out];
            }
            if (low_enabled) {
                const std::int16_t* const low_table = index == 0U ? high_residual : low_residual;
                const int low_weight_scale = index == 0U ? 1 : 4;
                for (std::size_t input = 0; input < kChannels; ++input) {
                    const std::int64_t low = static_cast<std::uint8_t>(activation_low[input]);
                    const auto* weights = low_table + input * kChannels;
                    for (std::size_t out = 0; out < kChannels; ++out)
                        residual_lanes[out] += low * weights[out] * low_weight_scale;
                }
            }
            const bool narrow_residual =
                (kS3NativeResidualNarrowMask & (1U << index)) != 0U && !exact_residual_layer(index);
            for (std::size_t out = 0; out < kChannels; ++out) {
                std::int32_t delta = 0;
                if (narrow_residual) {
                    const int extra = layer.residual_narrow_shift;
                    const int total_shift = layer.residual_shift - 7 + extra;
                    const std::int64_t raw = host_s3_qacc40_wrap(residual_lanes[out]);
                    const std::int64_t shifted = host_s3_qacc40_round_extract(raw, total_shift);
                    const std::int32_t lane = static_cast<std::int32_t>(
                        std::clamp<std::int64_t>(shifted, INT16_MIN, INT16_MAX));
                    delta = static_cast<std::int32_t>(static_cast<std::uint32_t>(lane) << extra);
                } else if (index == 0U) {
                    std::int64_t high_accumulator = 0;
                    std::int64_t low_accumulator = 0;
                    for (std::size_t input = 0; input < kChannels; ++input) {
                        const std::int16_t weight = high_residual[input * kChannels + out];
                        high_accumulator = host_s3_qacc40_wrap(
                            high_accumulator +
                            static_cast<std::int64_t>(activation_high[input]) * weight);
                        low_accumulator = host_s3_qacc40_wrap(
                            low_accumulator + static_cast<std::int64_t>(static_cast<std::uint8_t>(
                                                  activation_low[input])) *
                                                  weight);
                    }
                    delta =
                        host_s3_sat_add32(static_cast<std::int32_t>(host_s3_qacc40_round_extract(
                                              high_accumulator, layer.residual_shift - 7)),
                                          static_cast<std::int32_t>(host_s3_qacc40_round_extract(
                                              low_accumulator, layer.residual_shift)));
                } else {
                    delta = static_cast<std::int32_t>(host_s3_qacc40_round_extract(
                        residual_lanes[out], layer.residual_shift - 7));
                }
                delta = host_s3_sat_add32(delta, layer.residual_bias[out]);
                scratch.stream[first_frame + frame][out] =
                    host_s3_sat_add32(scratch.stream[first_frame + frame][out], delta);
            }
        }

        if (direct_next && index + 1U < kLayerCount) {
            Layer& next = layers_[index + 1U];
            const std::size_t next_position = (next.position + frame) % next.capacity;
            std::int16_t* const next_high = ring_high_ + next.ring_high_offset;
            const bool next_low_enabled = s3_native_low_layer(index + 1U);
            auto* const next_low = reinterpret_cast<std::int8_t*>(ring_low_ + next.ring_low_offset);
            auto write_next = [&](const std::size_t at) noexcept {
                for (std::size_t channel = 0; channel < kChannels; ++channel) {
                    const std::int32_t value = scratch.stream[first_frame + frame][channel];
                    if (next_low_enabled) {
                        const std::int32_t shifted = value >> next.ring_shift;
                        const std::int16_t high = static_cast<std::int16_t>(shifted >> 7);
                        next_high[at * kChannels + channel] = high;
                        next_low[at * kChannels + channel] = static_cast<std::int8_t>(
                            shifted - static_cast<std::int32_t>(high) * 128);
                    } else {
                        next_high[at * kChannels + channel] =
                            static_cast<std::int16_t>(value >> (next.ring_shift + 7));
                    }
                }
            };
            write_next(next_position);
            if (next_position < next.mirror) {
                write_next(next_position + next.capacity);
            }
        }

        layer.position = (position + 1U) % layer.capacity;
    }
#else
    const std::size_t frame_end = first_frame + frames;
    for (std::size_t tile = first_frame; tile < frame_end; tile += kBlockFrames) {
        const std::size_t base = layer.position;
        const std::size_t count = std::min(kBlockFrames, frame_end - tile);
        if (!ring_ready) {
            const auto quantize_segment = [&](const std::size_t source_frame,
                                              const std::size_t position,
                                              const std::size_t segment_frames) {
                if (low_enabled) {
                    s3_a2full_quantize8_a22_s7_s8(
                        scratch.stream[tile + source_frame], high_ring + position * kChannels,
                        low_ring_s8 + position * kChannels, layer.ring_shift,
                        static_cast<std::int32_t>(segment_frames));
                } else {
                    // These layers never consume an activation low limb, so
                    // shifting by the ring grid plus the seven-bit limb
                    // boundary emits the high limb directly. The calibrated
                    // range has no A22 clips.
                    s3_a2full_quantize8_narrow(
                        scratch.stream[tile + source_frame], high_ring + position * kChannels,
                        layer.ring_shift + 7, static_cast<std::int32_t>(segment_frames));
                }
                const std::size_t mirror_frames =
                    position < layer.mirror ? std::min(segment_frames, layer.mirror - position)
                                            : 0U;
                if (mirror_frames != 0U) {
                    std::memcpy(high_ring + (layer.capacity + position) * kChannels,
                                high_ring + position * kChannels,
                                mirror_frames * kChannels * sizeof(std::int16_t));
                    if (low_enabled) {
                        std::memcpy(low_ring_storage + (layer.capacity + position) * kChannels,
                                    low_ring_storage + position * kChannels,
                                    mirror_frames * kChannels);
                    }
                }
            };
            const std::size_t first = std::min(count, layer.capacity - base);
            quantize_segment(0U, base, first);
            if (first < count) {
                quantize_segment(first, 0U, count - first);
            }
        }

        std::size_t start = base + layer.capacity - (layer.kernel - 1U) * layer.dilation;
        while (start >= layer.capacity) {
            start -= layer.capacity;
        }
        const std::size_t before_wrap =
            layer.mirror == layer.capacity
                ? layer.kernel
                : std::min(layer.kernel,
                           (layer.capacity - start + layer.dilation - 1U) / layer.dilation);
        const std::size_t after_wrap = layer.kernel - before_wrap;
        const std::size_t wrapped_start = start + before_wrap * layer.dilation - layer.capacity;
        const auto* const high_weights = convolution_high_for_layer(index);
        const std::int16_t* const fused_high_run0 = high_ring + start * kChannels;
        const std::int16_t* const fused_high_run1 =
            after_wrap != 0U ? high_ring + wrapped_start * kChannels : high_ring;

        const void* fused_low_run0 = nullptr;
        const void* fused_low_run1 = nullptr;
        const void* fused_low_weights = nullptr;
        std::int32_t fused_low_stride = 0;
        std::int32_t fused_low_shift = 0;
        if (low_enabled) {
            if (index == 0U) {
                fused_low_run0 = ring_low_l0_s8_ + start * kChannels;
                fused_low_run1 = after_wrap != 0U ? ring_low_l0_s8_ + wrapped_start * kChannels
                                                  : ring_low_l0_s8_;
                fused_low_weights = convolution_low_l0_s8_;
            } else {
                fused_low_run0 = low_ring_s8 + start * kChannels;
                fused_low_run1 =
                    after_wrap != 0U ? low_ring_s8 + wrapped_start * kChannels : low_ring_s8;
                fused_low_weights = convolution_low_for_layer(index);
            }
            fused_low_stride = static_cast<std::int32_t>(layer.dilation * kChannels);
            fused_low_shift = layer.convolution_low_shift;
        }

        Layer* const next_layer =
            direct_next && index + 1U < kLayerCount ? &layers_[index + 1U] : nullptr;
        std::int16_t* next_high_base = nullptr;
        std::uint8_t* next_low_base = nullptr;
        std::int32_t next_low_enabled = 0;
        std::size_t next_position = 0U;
        if (next_layer != nullptr) {
            next_position = next_layer->position + (tile - first_frame);
            while (next_position >= next_layer->capacity) {
                next_position -= next_layer->capacity;
            }
            std::int16_t* const next_high_ring =
                (kInternalHighRingMask & (1U << (index + 1U))) != 0U
                    ? ring_high_internal_[index + 1U]
                    : ring_high_ + next_layer->ring_high_offset;
            next_high_base = next_high_ring + next_position * kChannels;
            if (s3_native_low_layer(index + 1U)) {
                next_low_base = ring_low_ + next_layer->ring_low_offset + next_position * kChannels;
                next_low_enabled = 1;
            }
        }

        const bool exact_residual = exact_residual_layer(index);
        const bool narrow_residual =
            (kS3NativeResidualNarrowMask & (1U << index)) != 0U && !exact_residual;
        const S3NativeFusedLayer8Args fused_args{
            fused_high_run0,
            static_cast<std::int32_t>(before_wrap),
            high_weights,
            static_cast<std::int32_t>(layer.dilation * kChannels * sizeof(std::int16_t)),
            layer.convolution_shift - 7,
            fused_high_run1,
            static_cast<std::int32_t>(after_wrap),
            fused_low_run0,
            fused_low_weights,
            fused_low_stride,
            fused_low_shift,
            fused_low_run1,
            scratch.input_q + tile,
            layer.activation_mixin.data(),
            layer.activation_bias.data(),
            16 - layer.activation_mixin_shift,
            scratch.head_sum[tile],
            layer.head_shift,
            residual_high_for_layer(index),
            low_enabled && index != 0U ? residual_low_for_layer(index) : nullptr,
            layer.residual_bias.data(),
            layer.residual_shift - (a16_activation ? 0 : 7),
            index == 0U ? layer.residual_shift : layer.residual_low_shift,
            scratch.stream[tile],
            next_high_base,
            next_low_base,
            next_layer != nullptr ? next_layer->ring_shift : 0,
            next_low_enabled,
            static_cast<std::int32_t>(count),
            narrow_residual ? layer.residual_narrow_shift : 0,
        };
        const bool wide_mixin = scratch.input_peak > layer.narrow_mixin_input_limit;
        S3NativeFusedLayer8Fn fused_function = nullptr;
        if (index == 0U) {
            fused_function = s3_a2full_fused_layer8_l0_s8;
        } else if (low_enabled) {
            if (index + 1U < kLayerCount && !s3_native_low_layer(index + 1U)) {
                fused_function = wide_mixin
                                     ? s3_a2full_fused_layer8_low_s8_next_high_wide_mixin_trunc
                                     : s3_a2full_fused_layer8_low_s8_next_high;
            } else if (index == 5U) {
                fused_function =
                    wide_mixin ? s3_a2full_fused_layer8_low_s8_conv_s16_guarded_wide_mixin_trunc
                               : s3_a2full_fused_layer8_low_s8_conv_s16_guarded;
            } else {
                fused_function = wide_mixin ? s3_a2full_fused_layer8_low_s8_wide_mixin_trunc
                                            : s3_a2full_fused_layer8_low_s8;
            }
        } else if (exact_residual) {
            // Preserve exact residual extraction when the input mixin needs
            // S32. Falling back to the narrow kernel would use the exact
            // path's zero narrowing shift and can overflow its S16 lanes.
            fused_function = wide_mixin ? s3_a2full_fused_layer8_high_exact_wide_mixin_trunc
                                        : s3_a2full_fused_layer8_high_exact_uncached;
        } else if (narrow_residual) {
            if (layer.cached_affine_safe) {
                fused_function = wide_mixin ? s3_a2full_fused_layer8_high_narrow_wide_mixin_trunc
                                            : s3_a2full_fused_layer8_high_narrow;
            } else {
                fused_function = wide_mixin
                                     ? s3_a2full_fused_layer8_high_narrow_wide_mixin_trunc_uncached
                                     : s3_a2full_fused_layer8_high_narrow_uncached;
            }
        }
        if (index + 1U == kLayerCount) {
            if (layer.cached_affine_safe) {
                fused_function =
                    wide_mixin
                        ? s3_a2full_fused_layer8_final_cached_wide_mixin_trunc_conv_s16_guarded
                        : s3_a2full_fused_layer8_final_cached_conv_s16_guarded;
            } else {
                fused_function =
                    wide_mixin ? s3_a2full_fused_layer8_final_wide_mixin_trunc_conv_s16_guarded
                               : s3_a2full_fused_layer8_final_conv_s16_guarded;
            }
        }
        fused_function(&fused_args);

        if (next_layer != nullptr) {
            std::int16_t* const next_high_ring = next_high_base - next_position * kChannels;
            const std::size_t end = next_position + count;
            if (end > next_layer->capacity) {
                const std::size_t wrapped = end - next_layer->capacity;
                const std::size_t tail = count - wrapped;
                // The fused call wrote the wrapped prefix into the mirror.
                // Move it into the primary ring before the next tile.
                std::memcpy(next_high_ring, next_high_ring + next_layer->capacity * kChannels,
                            wrapped * kChannels * sizeof(std::int16_t));
                if (next_layer->mirror == next_layer->capacity) {
                    std::memcpy(next_high_ring + (next_layer->capacity + next_position) * kChannels,
                                next_high_ring + next_position * kChannels,
                                tail * kChannels * sizeof(std::int16_t));
                }
                if (next_low_base != nullptr) {
                    std::uint8_t* const next_low_ring = next_low_base - next_position * kChannels;
                    std::memcpy(next_low_ring, next_low_ring + next_layer->capacity * kChannels,
                                wrapped * kChannels);
                    if (next_layer->mirror == next_layer->capacity) {
                        std::memcpy(next_low_ring +
                                        (next_layer->capacity + next_position) * kChannels,
                                    next_low_ring + next_position * kChannels, tail * kChannels);
                    }
                }
            } else if (next_position < next_layer->mirror) {
                const std::size_t mirrored = std::min(count, next_layer->mirror - next_position);
                std::memcpy(next_high_ring + (next_layer->capacity + next_position) * kChannels,
                            next_high_ring + next_position * kChannels,
                            mirrored * kChannels * sizeof(std::int16_t));
                if (next_low_base != nullptr) {
                    std::uint8_t* const next_low_ring = next_low_base - next_position * kChannels;
                    std::memcpy(next_low_ring + (next_layer->capacity + next_position) * kChannels,
                                next_low_ring + next_position * kChannels, mirrored * kChannels);
                }
            }
        }

        layer.position = base + count;
        while (layer.position >= layer.capacity) {
            layer.position -= layer.capacity;
        }
    }
#endif
}

COYOPEDAL_S3_BLOCK_HOT
void A2FullS3Native::process_layers(BlockScratch& scratch, const std::size_t first,
                                    const std::size_t last) noexcept {
    const std::size_t end = std::min(last, kLayerCount);
    // Each layer writes the next layer's ring directly. A preceding pipeline
    // segment therefore leaves this layer's ring quantized, and the second
    // segment does not rebuild it.
    bool ring_ready = first != 0U;
    for (std::size_t index = first; index < end; ++index) {
        const bool direct_next = index + 1U < kLayerCount;
        run_layer(scratch, index, 0U, scratch.frames, ring_ready, direct_next);
        ring_ready = direct_next;
    }
}

COYOPEDAL_S3_BLOCK_HOT
void A2FullS3Native::finish_block(BlockScratch& scratch, float* const output) noexcept {
    constexpr int kHeadHistoryShift = 15;
    // The head dot product accumulates in eight lanes, each narrowed to S16
    // after this shift.
    constexpr int kHeadLaneShift = 14;
#if defined(ESP_PLATFORM)
    const float head_dot_scale =
        std::ldexp(head_grid_, kHeadHistoryShift - head_weight_shift_ + kHeadLaneShift);
    // The final layer no longer needs scratch.stream. Reuse its first row span
    // for the 64 lane sums instead of reserving more SRAM.
    std::int32_t* const lane_sums = &scratch.stream[0][0];
    const S3NativeHeadBlockArgs args{
        &scratch.head_sum[0][0],
        &head_history_q15_[0][0],
        head_q15_.data(),
        &kS3NativeHeadQuantizeConstants,
        lane_sums,
        static_cast<std::int32_t>(scratch.frames),
        static_cast<std::int32_t>(head_position_),
    };
    head_position_ = static_cast<std::size_t>(s3_a2full_native_head_block(&args));
    for (std::size_t frame = 0; frame < scratch.frames; ++frame) {
        const float result = head_bias_ + static_cast<float>(lane_sums[frame]) * head_dot_scale;
        output[frame] = result * head_scale_;
    }
#else
    const float head_dot_scale = std::ldexp(head_grid_, kHeadHistoryShift - head_weight_shift_);
    for (std::size_t frame = 0; frame < scratch.frames; ++frame) {
        for (std::size_t channel = 0; channel < kChannels; ++channel) {
            const std::int32_t value = scratch.head_sum[frame][channel];
            constexpr std::int32_t half = 1 << (kHeadHistoryShift - 1);
            const std::int32_t shifted = value >= 0 ? (value + half) >> kHeadHistoryShift
                                                    : -(((-value) + half) >> kHeadHistoryShift);
            head_history_q15_[head_position_][channel] =
                static_cast<std::int16_t>(std::clamp(shifted, static_cast<std::int32_t>(INT16_MIN),
                                                     static_cast<std::int32_t>(INT16_MAX)));
        }
        // Complete the linear oldest-to-newest window the PIE kernel reads.
        // The final tap lives in the mirrored copy of the row just written.
        head_history_q15_[head_position_ + kHeadKernel] = head_history_q15_[head_position_];
        std::int64_t head_lanes[kChannels]{};
        for (std::size_t tap = 0; tap < kHeadKernel; ++tap) {
            const std::size_t delay = kHeadKernel - 1U - tap;
            const std::size_t at = (head_position_ + kHeadKernel - delay) % kHeadKernel;
            for (std::size_t channel = 0; channel < kChannels; ++channel) {
                head_lanes[channel] += static_cast<std::int32_t>(head_history_q15_[at][channel]) *
                                       head_q15_[tap * kChannels + channel];
            }
        }
        std::int64_t dot = 0;
        for (const std::int64_t lane : head_lanes) {
            const std::int64_t narrowed = std::clamp<std::int64_t>(
                (lane + (INT64_C(1) << (kHeadLaneShift - 1))) >> kHeadLaneShift, INT16_MIN,
                INT16_MAX);
            dot += narrowed << kHeadLaneShift;
        }
        const float result = head_bias_ + static_cast<float>(dot) * head_dot_scale;
        output[frame] = result * head_scale_;
        head_position_ = (head_position_ + 1U) % kHeadKernel;
    }
#endif
}

void A2FullS3Native::process(float* const samples, const std::size_t frame_count) noexcept {
    if (!loaded_ || samples == nullptr) {
        return;
    }
#if defined(ESP_PLATFORM)
    // The pedal uses the staged API and owns its two hot scratch blocks in the
    // realtime pipeline. Keep this compatibility entry point from reserving a
    // third 6 KiB block in internal .bss; allocate its scratch lazily in PSRAM
    // only if a caller actually uses process().
    static BlockScratch* scratch_storage = nullptr;
    if (scratch_storage == nullptr) {
        scratch_storage = s3_native_allocate<BlockScratch>(1, false);
        if (scratch_storage == nullptr) {
            return;
        }
    }
    BlockScratch& scratch = *scratch_storage;
#else
    static BlockScratch scratch;
#endif
    for (std::size_t at = 0; at < frame_count; at += kMaxStagedFrames) {
        const std::size_t frames = std::min(kMaxStagedFrames, frame_count - at);
        begin_block(samples + at, frames, scratch);
        process_layers(scratch, 0, kLayerCount);
        finish_block(scratch, samples + at);
    }
}

} // namespace nam_bfp
