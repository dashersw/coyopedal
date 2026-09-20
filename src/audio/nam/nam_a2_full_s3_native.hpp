// The A2 model shape, weight order and inference equations implemented here are
// derived from NeuralAmpModelerCore, MIT, Copyright (c) 2023 Steven Atkinson.
// See THIRD_PARTY_NOTICES.md.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace nam_bfp {

#ifndef COYOPEDAL_PEDAL_BLOCK_FRAMES
#define COYOPEDAL_PEDAL_BLOCK_FRAMES 64
#endif
// Which layer starts stage B, i.e. how the 23 layers divide between the cores.
// The default is 8: core 0 runs layers 0-7 and core 1 runs layers 8-22.
#ifndef COYOPEDAL_PEDAL_S3_SPLIT_LAYER
#define COYOPEDAL_PEDAL_S3_SPLIT_LAYER 8
#endif

class A2FullS3Native {
  public:
    // Only while all audio workers are quiescent. Reversible via load_namb().
    void release() noexcept;
    static constexpr std::size_t kChannels = 8;
    static constexpr std::size_t kLayerCount = 23;
    static constexpr std::size_t kStagedLayerCount = kLayerCount;
    static constexpr std::size_t kHeadKernel = 16;
    // Keep the arithmetic/ring geometry qualified by the 64-frame engine even
    // when the transport scheduler feeds smaller blocks. A 64-frame engine is
    // already bit-exact when begin_block() receives 16 frames; recompiling its
    // ring mirror and full-block fast-path threshold at 16 changed arithmetic.
    static constexpr std::size_t kBlockFrames = 64U;
    static constexpr std::size_t kMaxStagedFrames = kBlockFrames;
    static constexpr std::size_t kWeightCount = 12146;
    static constexpr std::size_t kFileSize = 32U + kWeightCount * sizeof(float);
    // S3P1/S3P2 grids are wrapped by S3P3 (derived residual narrowing) or S3P4
    // (tuned residual narrowing), with a residual mask and CRCs binding the
    // model.
    static constexpr std::size_t kDiagnosticPrecisionSize = 12U + 48U * kLayerCount;
    static constexpr std::size_t kPreparedTrailerSize = 16U + kDiagnosticPrecisionSize;
    static constexpr std::size_t kPreparedFileSize = kFileSize + kPreparedTrailerSize;
    bool export_precision(std::uint8_t* out, std::size_t size) const noexcept;
    static bool prepare_sweep(const std::uint8_t* data, std::size_t size, std::uint8_t* trailer,
                              char* error, std::size_t capacity) noexcept;
    // Import-time tuning validates each candidate on a short corpus.
    struct TuningOptions {
        std::uint32_t max_trials = 96;
        double target_peak = 0.001;
        float probe_gain = 1.0F;
        std::uint32_t float_block_frames = 64U;
        std::uint32_t native_split_layer = 9U; // 0 keeps native validation serial.
    };
    struct TuningReport {
        std::uint32_t trials = 0;
        std::uint32_t invalid_candidates = 0;
        double baseline_peak = 0;
        double peak = 0;
        double rms = 0;
        bool target_met = false;
        float stream_margin = 1.6F;
        float head_margin = 1.6F;
        float layer_margin = 1.6F;
        bool measured_residual = false;
        std::uint32_t exact_mask = 0;
        std::uint32_t float_block_frames = 64U;
        std::uint32_t native_split_layer = 0U;
        bool parallel_float = false;
    };
    static bool prepare_tuned(const std::uint8_t* data, std::size_t size, std::uint8_t* trailer,
                              const TuningOptions& options, TuningReport& report, char* error,
                              std::size_t capacity) noexcept;
    static bool validate_prepared(const std::uint8_t* data, std::size_t size) noexcept;
    static bool wrap_precision(const std::uint8_t* model, const std::uint8_t* parameters,
                               std::uint32_t mask, std::uint8_t* out) noexcept;
    // Layers 0-11 keep an S8 low limb below their S16 history; layers 12-22
    // store only the high limb.
    static constexpr std::uint32_t kLowLayerMask = 0x000FFFU;
    // Every low history is S8; no layer uses an S16 low ring.
    static constexpr std::uint32_t kLowRingW16Mask = 0x000000U;
    static constexpr std::size_t kStageSplitLayer = COYOPEDAL_PEDAL_S3_SPLIT_LAYER;
    static constexpr std::uint32_t kSkippedLowLayerMask = 0x7FF000U;
    // Every one of the 23 histories lives in internal SRAM, which keeps the LX7
    // MAC kernels off the shared external-memory bus. The effects' internal
    // memory is paid for out of the heap allocations that precede the model,
    // never by moving a history to PSRAM.
    static constexpr std::uint32_t kInternalHighRingMask = 0x7FFFFFU;

    struct BlockScratch {
        alignas(16) std::int32_t stream[kMaxStagedFrames][kChannels]{};
        alignas(16) std::int32_t head_sum[kMaxStagedFrames][kChannels]{};
        alignas(16) std::int16_t input_q[kMaxStagedFrames]{};
        std::uint32_t input_peak = 0;
        std::size_t frames = 0;
    };

    explicit A2FullS3Native(bool external_workspace = false) noexcept
        : external_workspace_(external_workspace) {}
    ~A2FullS3Native();
    A2FullS3Native(const A2FullS3Native&) = delete;
    A2FullS3Native& operator=(const A2FullS3Native&) = delete;

    bool load_namb(const std::uint8_t* data, std::size_t size, char* error_message,
                   std::size_t error_message_capacity) noexcept;
    void reset() noexcept;
    void process(float* samples, std::size_t frame_count) noexcept;
    void begin_block(const float* samples, std::size_t frames, BlockScratch& scratch) noexcept;
    void process_layers(BlockScratch& scratch, std::size_t first, std::size_t last) noexcept;
    void finish_block(BlockScratch& scratch, float* output) noexcept;

    [[nodiscard]] bool loaded() const noexcept {
        return loaded_;
    }
    [[nodiscard]] std::uint32_t overflow_count() const noexcept {
        return overflows_;
    }

  private:
    bool exact_residual_layer(std::size_t index) const noexcept;
    bool defer_tuning_prewarm_ = false;
    bool external_workspace_ = false;
    bool tuned_narrowing_ = false;
    bool full_activation_lanes_ = false;
    std::uint32_t model_crc_ = 0;
    std::uint32_t exact_residual_mask_ = 0;
    static constexpr std::size_t kRingMirror = kBlockFrames;

    struct Layer {
        std::size_t kernel = 0;
        std::size_t dilation = 0;
        std::size_t capacity = 0;
        std::size_t mirror = 0;
        std::size_t span = 0;
        std::size_t position = 0;
        std::size_t ring_high_offset = 0;
        std::size_t ring_low_offset = 0;
        std::size_t convolution_high_offset = 0;
        std::size_t convolution_low_offset = 0;
        std::size_t residual_high_offset = 0;
        std::size_t residual_low_offset = 0;
        int ring_shift = 0;
        int convolution_shift = 0;
        int convolution_low_shift = 0;
        int residual_shift = 0;
        int residual_low_shift = 0;
        // Extra right shift used by the fast S16 QACC extraction. This is
        // derived from the loaded residual weights and the exact A22
        // activation limits, rather than a library-calibrated layer table.
        int residual_narrow_shift = 0;
        int head_shift = 0;
        int activation_mixin_shift = 0;
        std::uint32_t narrow_mixin_input_limit = 0;
        // The cached QACC affine kernel packs the eight activation biases as
        // S16 and materializes 1 << (convolution_shift - 7) as S16. Select it
        // only when the loaded model proves both representations exact.
        bool cached_affine_safe = false;
        alignas(16) std::array<std::int32_t, kChannels> activation_bias{};
        alignas(16) std::array<std::int16_t, kChannels> activation_mixin{};
        alignas(16) std::array<std::int32_t, kChannels> residual_bias{};
    };

    void reset_tuning_steady(BlockScratch& scratch) noexcept;

    void run_layer(BlockScratch& scratch, std::size_t index, std::size_t first_frame,
                   std::size_t frames, bool ring_ready, bool direct_next) noexcept;
    [[nodiscard]] std::int16_t* convolution_high_for_layer(std::size_t index) const noexcept;
    [[nodiscard]] std::int16_t* convolution_low_for_layer(std::size_t index) const noexcept;
    [[nodiscard]] std::int16_t* residual_high_for_layer(std::size_t index) const noexcept;
    [[nodiscard]] std::int16_t* residual_low_for_layer(std::size_t index) const noexcept;

    std::array<Layer, kLayerCount> layers_{};
    std::array<std::int16_t, kChannels> rechannel_q15_{};
    int rechannel_shift_ = 0;
    alignas(16) std::array<std::int16_t, kHeadKernel * kChannels> head_q15_{};
    int head_weight_shift_ = 0;
    float head_bias_ = 0.0F;
    float head_scale_ = 0.0F;
    float stream_scale_ = 1.0F;
    float head_grid_ = 1.0F;
    alignas(
        16) std::array<std::array<std::int16_t, kChannels>, 2 * kHeadKernel> head_history_q15_{};
    std::size_t head_position_ = 0;

    std::int16_t* ring_high_ = nullptr;
    std::array<std::int16_t*, kLayerCount> ring_high_internal_{};
    std::int8_t* ring_low_l0_s8_ = nullptr;
    std::uint8_t* ring_low_ = nullptr;
    std::int16_t* convolution_high_ = nullptr;
    std::int16_t* convolution_high_stage_b_ = nullptr;
    std::int16_t* convolution_low_l0_s8_ = nullptr;
    std::int16_t* convolution_low_ = nullptr;
    std::int16_t* convolution_low_stage_b_ = nullptr;
    std::int16_t* residual_high_ = nullptr;
    std::int16_t* residual_high_stage_b_ = nullptr;
    std::int16_t* residual_low_ = nullptr;
    std::int16_t* residual_low_stage_b_ = nullptr;
    std::size_t ring_high_count_ = 0;
    std::size_t ring_low_count_ = 0; // bytes
    std::size_t convolution_high_count_ = 0;
    std::size_t convolution_high_stage_b_offset_ = 0;
    std::size_t convolution_low_count_ = 0;
    std::size_t convolution_low_stage_b_offset_ = 0;
    std::size_t residual_high_count_ = 0;
    std::size_t residual_high_stage_b_offset_ = 0;
    std::size_t residual_low_count_ = 0;
    std::size_t residual_low_stage_b_offset_ = 0;
    std::uint32_t overflows_ = 0;
    bool loaded_ = false;
};

} // namespace nam_bfp
