// The A2 model shape, weight order and inference equations implemented here are
// derived from NeuralAmpModelerCore, MIT, Copyright (c) 2023 Steven Atkinson.
// See THIRD_PARTY_NOTICES.md.

// Shared host/device import-time tuner. Included inside namespace nam_bfp.
bool A2FullS3Native::prepare_tuned(const std::uint8_t* data, std::size_t size,
                                   std::uint8_t* trailer, const TuningOptions& options,
                                   TuningReport& report, char* error,
                                   std::size_t capacity) noexcept {
    report = {};
    if (!data || !trailer || size != kFileSize || options.max_trials == 0U ||
        !std::isfinite(options.target_peak) || options.target_peak <= 0.0 ||
        !std::isfinite(options.probe_gain) || options.probe_gain <= 0.0F ||
        options.probe_gain > 8.0F ||
        (options.float_block_frames != 8U && options.float_block_frames != 16U &&
         options.float_block_frames != 32U && options.float_block_frames != 64U) ||
        options.native_split_layer >= kLayerCount) {
        copy_error("invalid sweep tuning arguments", error, capacity);
        return false;
    }
#if defined(ESP_PLATFORM)
    heap_caps_malloc_extmem_enable(0U);
    struct RestoreTuningState {
        esp_log_level_t nam_log_level = esp_log_level_get("nam");
        RestoreTuningState() {
            // Per-layer allocation/grid logs multiply by every search trial.
            // Keep warnings and errors, then restore normal model-load logs.
            esp_log_level_set("nam", ESP_LOG_WARN);
        }
        ~RestoreTuningState() {
            esp_log_level_set("nam", nam_log_level);
            heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
        }
    } restore_state;
#endif
#if defined(ESP_PLATFORM)
    const auto profile_start = esp_timer_get_time();
    std::int64_t profile_prepare = 0, profile_warmup = 0, profile_render = 0;
#endif
    constexpr std::size_t count = 12000U;
    std::unique_ptr<FloatModel> model(parse_float_model(data, size, error, capacity, true));
    std::unique_ptr<float[]> input(new (std::nothrow) float[count]);
    std::unique_ptr<float[]> reference(new (std::nothrow) float[count]);
    std::unique_ptr<std::uint8_t[]> prepared(new (std::nothrow) std::uint8_t[kPreparedFileSize]);
    std::unique_ptr<A2FullS3Native> candidate(new (std::nothrow) A2FullS3Native(true));
    struct ScratchOwner {
        BlockScratch* value = s3_native_allocate<BlockScratch>(1U, false);
        ~ScratchOwner() {
            s3_native_free(value);
        }
    } scratch;
    if (!model || !input || !reference || !prepared || !candidate || !scratch.value) {
        copy_error("sweep tuning workspace allocation failed", error, capacity);
        return false;
    }
    model->tuning_block_frames = options.float_block_frames;
    report.float_block_frames = options.float_block_frames;
#if defined(ESP_PLATFORM)
    model->parallel_tuning = ensure_tuning_worker();
    report.parallel_float = model->parallel_tuning;
    struct NativeSlot {
        BlockScratch scratch;
        std::array<float, 64> audio{};
        A2FullS3Native* owner = nullptr;
        std::size_t offset = 0, frames = 0, split = 0;
    };
    struct NativeSlots {
        NativeSlot* value = nullptr;
        ~NativeSlots() {
            s3_native_free(value);
        }
    } native_slots;
    if (model->parallel_tuning && options.native_split_layer != 0U)
        native_slots.value = s3_native_allocate<NativeSlot>(2U, false);
    report.native_split_layer = native_slots.value ? options.native_split_layer : 0U;
#endif
    // Candidates are warmed only after their final quantizers are installed.
    candidate->defer_tuning_prewarm_ = true;
    std::memcpy(prepared.get(), data, size);
    auto yield = [](std::size_t block) {
#if defined(ESP_PLATFORM)
        if ((block & 15U) == 15U)
            vTaskDelay(1);
#else
        (void)block;
#endif
    };
    fill_calibration(input.get(), count);
    model->run(input.get(), count);
    const float base_stream = prepared_stream_margin(data);
    std::array<std::uint8_t, kDiagnosticPrecisionSize> base_record{};
    make_sweep_precision(*model, base_stream, 1.6F, 1.6F, base_record);
    // Keep the original sweep baseline independent of subsequent probe ranges.
    wrap_precision(data, base_record.data(), kS3NativeExactResidualMask, trailer);

    // Extend the envelope after preserving the original sweep baseline. The
    // logarithmic chirp gives low-frequency responses more settling time.
    for (std::size_t at = 0; at < count; ++at)
        input[at] = static_cast<float>(kS3TuningCalibration[at]) * (0.95F / 32767.0F);
    model->run(input.get(), count);

    // Matched 8192-frame zero prewarm, identical to a normally loaded engine.
    model->reset_state();
    std::fill_n(input.get(), 8192U, 0.0F);
    model->run(input.get(), 8192U);
    for (std::size_t at = 0; at < count; ++at)
        input[at] = static_cast<float>(kS3TuningProbe[at]) * (options.probe_gain / 32768.0F);
    model->run(input.get(), reference.get(), count);
#if defined(ESP_PLATFORM)
    const auto profile_float = esp_timer_get_time() - profile_start;
#endif
    struct Choice {
        float stream, head, layer;
        bool measured;
        std::uint32_t mask;
    };
    Choice selected{base_stream, 1.6F, 1.6F, false, kS3NativeExactResidualMask};
    double best_peak = std::numeric_limits<double>::infinity();
    double best_rms = std::numeric_limits<double>::infinity();
    bool failed = false;
    struct CachedTrial {
        std::array<std::uint8_t, kPreparedTrailerSize> parameters;
        bool measured;
        bool range_safe;
    };
    // Exact byte comparisons, not hashes. A previously evaluated candidate
    // cannot beat the monotonically improving incumbent on another visit.
    // If PSRAM is exhausted, run the original evaluations without caching.
    constexpr std::size_t cache_capacity = 96U;
    std::unique_ptr<CachedTrial[]> cache(new (std::nothrow) CachedTrial[cache_capacity]);
    std::size_t cache_count = 0U, cache_hits = 0U;
    auto evaluate = [&](Choice choice, bool baseline = false) {
        if (failed || report.trials >= options.max_trials)
            return;
#if defined(ESP_PLATFORM)
        const auto profile_candidate = esp_timer_get_time();
#endif
        std::array<std::uint8_t, kDiagnosticPrecisionSize> record = base_record;
        if (!baseline)
            make_sweep_precision(*model, choice.stream, choice.head, choice.layer, record, true);
        if (!wrap_precision(data, record.data(), choice.mask, prepared.get() + kFileSize) ||
            !validate_prepared(prepared.get(), kPreparedFileSize)) {
            ++report.trials;
            ++report.invalid_candidates;
            if (baseline) {
                copy_error("invalid baseline sweep parameters", error, capacity);
                failed = true;
            }
            return;
        }
        for (std::size_t i = 0; i < cache_count; ++i) {
            if (cache[i].measured == choice.measured &&
                std::memcmp(cache[i].parameters.data(), prepared.get() + kFileSize,
                            kPreparedTrailerSize) == 0) {
                ++report.trials;
                if (!cache[i].range_safe && !baseline)
                    ++report.invalid_candidates;
                ++cache_hits;
                return;
            }
        }
        if (!candidate->load_namb(prepared.get(), kPreparedFileSize, error, capacity)) {
            failed = true;
            return;
        }
        bool range_safe = true;
        for (std::size_t layer = 0; layer < kLayerCount; ++layer) {
            const auto& parameters = candidate->layers_[layer];
            const float maximum = layer == 22U ? 32767.0F
                                  : candidate->full_activation_lanes_ && layer != 0U
                                      ? 32767.0F * 128.0F
                                      : 2097151.0F;
            const float capacity =
                std::ldexp(candidate->head_grid_, parameters.head_shift) * maximum;
            range_safe &= capacity >= stable_calibration_peak(model->max_activation[layer]);
            const float ring_maximum =
                candidate->full_activation_lanes_ && layer != 0U ? 32767.0F * 128.0F : 2097151.0F;
            const float ring_capacity =
                std::ldexp(candidate->stream_scale_, parameters.ring_shift) * ring_maximum;
            range_safe &= ring_capacity >= stable_calibration_peak(model->max_stream[layer]);
        }
        if (cache && cache_count < cache_capacity) {
            auto& entry = cache[cache_count++];
            std::memcpy(entry.parameters.data(), prepared.get() + kFileSize, kPreparedTrailerSize);
            entry.measured = choice.measured;
            entry.range_safe = range_safe;
        }
        if (!range_safe && !baseline) {
            ++report.trials;
            ++report.invalid_candidates;
            return;
        }
        if (choice.measured) {
            for (std::size_t layer = 12U; layer <= 21U; ++layer) {
                // Bound each quantized dot product using independently measured
                // channel envelopes. A small residual sum can hide cancellation
                // between large channels, so do not narrow from that sum alone.
                const auto& parameters = candidate->layers_[layer];
                const float activation_grid =
                    std::ldexp(candidate->head_grid_, parameters.head_shift);
                const auto* weights = candidate->residual_high_for_layer(layer);
                std::int64_t bound = 0;
                for (std::size_t out = 0; out < kChannels; ++out) {
                    std::int64_t sum = 0;
                    for (std::size_t in = 0; in < kChannels; ++in) {
                        const float peak =
                            stable_calibration_peak(model->max_activation_channel[layer][in]);
                        const auto high = static_cast<std::int64_t>(
                            std::min(candidate->full_activation_lanes_ ? 32768.0F : 16384.0F,
                                     std::ceil(peak * 1.6F / (activation_grid * 128.0F)) + 1.0F));
                        sum += std::abs(static_cast<std::int64_t>(weights[in * kChannels + out])) *
                               high;
                    }
                    bound = std::max(bound, sum);
                }
                const int base = parameters.residual_shift - 7;
                int total = std::max(base, 0);
                while (total < 31 && bound > (INT64_C(32767) << total))
                    ++total;
                candidate->layers_[layer].residual_narrow_shift =
                    std::min(parameters.residual_narrow_shift, std::max(0, total - base));
            }
            candidate->tuned_narrowing_ = true;
        }
#if defined(ESP_PLATFORM)
        const auto profile_warm_start = esp_timer_get_time();
        profile_prepare += profile_warm_start - profile_candidate;
#endif
        candidate->defer_tuning_prewarm_ = false;
        candidate->reset_tuning_steady(*scratch.value);
        candidate->defer_tuning_prewarm_ = true;
#if defined(ESP_PLATFORM)
        const auto profile_render_start = esp_timer_get_time();
        profile_warmup += profile_render_start - profile_warm_start;
#endif
        double peak = 0.0, energy = 0.0;
        bool complete = true;
        auto consume = [&](const float* work, std::size_t at, std::size_t frames) {
            for (std::size_t i = 0; i < frames; ++i) {
                const double value = static_cast<double>(reference[at + i]) - work[i];
                if (!std::isfinite(value)) {
                    peak = std::numeric_limits<double>::infinity();
                    break;
                }
                peak = std::max(peak, std::fabs(value));
                energy += value * value;
            }
            return !(peak > best_peak);
        };
#if defined(ESP_PLATFORM)
        if (model->parallel_tuning && native_slots.value) {
            const std::size_t split = options.native_split_layer;
            NativeSlot* pending = nullptr;
            for (std::size_t at = 0, number = 0; at < count; at += 64U, ++number) {
                auto& slot = native_slots.value[number & 1U];
                slot.owner = candidate.get();
                slot.offset = at;
                slot.split = split;
                slot.frames = std::min<std::size_t>(64U, count - at);
                std::copy_n(input.get() + at, slot.frames, slot.audio.data());
                candidate->begin_block(slot.audio.data(), slot.frames, slot.scratch);
                // The last prefix layer writes the next core's input ring.
                // Overlap only the preceding layers, then wait before that
                // handoff so the worker has finished reading its prior block.
                candidate->process_layers(slot.scratch, 0U, split - 1U);
                if (pending) {
                    wait_tuning_work();
                    complete = consume(pending->audio.data(), pending->offset, pending->frames);
                    pending = nullptr;
                    if (!complete)
                        break;
                }
                candidate->process_layers(slot.scratch, split - 1U, split);
                submit_tuning_work(
                    [](void* p) {
                        auto& work = *static_cast<NativeSlot*>(p);
                        work.owner->process_layers(work.scratch, work.split, kLayerCount);
                        work.owner->finish_block(work.scratch, work.audio.data());
                    },
                    &slot);
                pending = &slot;
                yield(number);
            }
            if (pending) {
                wait_tuning_work();
                complete = consume(pending->audio.data(), pending->offset, pending->frames);
            }
        } else
#endif
        {
            std::array<float, 64> work{};
            for (std::size_t at = 0; at < count; at += 64U) {
                const std::size_t frames = std::min<std::size_t>(64U, count - at);
                std::copy_n(input.get() + at, frames, work.data());
                candidate->begin_block(work.data(), frames, *scratch.value);
                candidate->process_layers(*scratch.value, 0U, kStageSplitLayer);
                candidate->process_layers(*scratch.value, kStageSplitLayer, kLayerCount);
                candidate->finish_block(*scratch.value, work.data());
                if (!consume(work.data(), at, frames)) {
                    complete = false;
                    break;
                }
                yield(at / 64U);
            }
        }
#if defined(ESP_PLATFORM)
        profile_render += esp_timer_get_time() - profile_render_start;
#endif
        ++report.trials;
        if (baseline)
            report.baseline_peak = peak;
        const double rms = std::sqrt(energy / static_cast<double>(count));
        if (range_safe && complete && (peak < best_peak || (peak == best_peak && rms < best_rms))) {
            best_peak = peak;
            best_rms = rms;
            selected = choice;
            if (!candidate->export_precision(trailer, kPreparedTrailerSize))
                failed = true;
        }
    };
    evaluate(selected, true);
    // Test output headroom and measured residual precision before fine search.
    evaluate({base_stream, 2.0F, 1.6F, false, 0U});
    evaluate({base_stream, 1.6F, 1.6F, true, 0U});
    evaluate({base_stream, 2.0F, 1.6F, true, 0U});
    for (float head : {1.0F, 1.25F, 1.5F, 1.75F, 2.25F, 2.5F, 3.0F, 3.2F}) {
        evaluate({base_stream, head, 1.6F, false, 0U});
        evaluate({base_stream, head, 1.6F, true, 0U});
    }
    for (float margin : {1.25F, 1.5F, 1.75F, 2.0F, 2.5F, 3.2F}) {
        evaluate({margin, selected.head, selected.layer, selected.measured, 0U});
        evaluate({selected.stream, selected.head, margin, selected.measured, 0U});
    }
    // Revisit output-grid precision after stream/layer changes: their optimum
    // need not coincide with the first head search at the baseline grids.
    for (int pass = 0; pass < 2; ++pass) {
        const Choice anchor = selected;
        const float step = pass == 0 ? 0.125F : 0.03125F;
        for (int offset : {-4, -3, -2, -1, 1, 2, 3, 4}) {
            const float head = anchor.head + offset * step;
            if (head >= 1.0F && head <= 4.0F) {
                evaluate({anchor.stream, head, anchor.layer, false, 0U});
                evaluate({anchor.stream, head, anchor.layer, true, 0U});
            }
        }
    }
    // A small final precision budget is available only if range tuning fails.
    if (best_peak > options.target_peak * 0.5) {
        const Choice anchor = selected;
        evaluate({anchor.stream, anchor.head, anchor.layer, anchor.measured, 0x003ff000U});
        for (std::size_t layer = 12U; layer <= 21U; ++layer) {
            // The all-exact candidate may already have won. Re-evaluating it
            // for every layer cannot change the parameters or score.
            if ((selected.mask & (1U << layer)) != 0U)
                continue;
            evaluate({selected.stream, selected.head, selected.layer, selected.measured,
                      selected.mask | (1U << layer)});
        }
    }
    // Exact residual arithmetic changes which grids minimize error. Revisit
    // their ranges after selecting it rather than retaining an optimum found
    // for the narrower residual path.
    if (selected.mask != 0U && best_peak > options.target_peak * 0.5) {
        for (float margin : {1.25F, 1.5F, 1.75F, 2.0F, 2.5F, 3.2F}) {
            evaluate({margin, selected.head, selected.layer, selected.measured, selected.mask});
            evaluate({selected.stream, selected.head, margin, selected.measured, selected.mask});
        }
        for (float head : {1.0F, 1.25F, 1.5F, 1.75F, 2.0F, 2.5F, 3.0F})
            evaluate({selected.stream, head, selected.layer, selected.measured, selected.mask});
        const Choice anchor = selected;
        for (int offset : {-4, -3, -2, -1, 1, 2, 3, 4}) {
            const float head = anchor.head + offset * 0.03125F;
            if (head >= 1.0F && head <= 4.0F)
                evaluate({anchor.stream, head, anchor.layer, anchor.measured, anchor.mask});
        }
    }
#if defined(ESP_PLATFORM)
    ESP_LOGI("nam_tuning",
             "TUNING_US float=%lld prepare=%lld warmup=%lld render=%lld total=%lld duplicates=%u "
             "float_block=%u native_split=%u stack_free=%u",
             (long long)profile_float, (long long)profile_prepare, (long long)profile_warmup,
             (long long)profile_render, (long long)(esp_timer_get_time() - profile_start),
             unsigned(cache_hits), unsigned(options.float_block_frames),
             unsigned(report.native_split_layer), unsigned(uxTaskGetStackHighWaterMark(nullptr)));
#endif
    report.peak = best_peak;
    report.rms = best_rms;
    report.target_met = !failed && best_peak <= options.target_peak;
    report.stream_margin = selected.stream;
    report.head_margin = selected.head;
    report.layer_margin = selected.layer;
    report.measured_residual = selected.measured;
    report.exact_mask = selected.mask;
    if (failed)
        return false;
    if (!report.target_met) {
        copy_error("sweep tuning did not meet the peak-null target", error, capacity);
        return false;
    }
    copy_error("", error, capacity);
    return true;
}
