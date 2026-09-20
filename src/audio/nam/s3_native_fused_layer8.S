// The A2 model shape, weight order and inference equations implemented here are
// derived from NeuralAmpModelerCore, MIT, Copyright (c) 2023 Steven Atkinson.
// See THIRD_PARTY_NOTICES.md.

// Complete eight-frame ESP32-S3 A2-Full layer kernel.
//
// A single call owns convolution, bias/condition, PReLU, head accumulation,
// residual and the write into the next layer's quantized ring. Specialized
// entry points keep every topology decision outside the hot loop.

    .section .iram1, "ax"
    .align  4

// S3NativeFusedLayer8Args, all fields are 32-bit:
//   0 high_run0        4 count0          8 high_weights
//  12 high_stride     16 high_shift     20 high_run1
//  24 count1          28 low_run0       32 low_weights
//  36 low_stride      40 low_shift      44 low_run1
//  48 input_q         52 mixin           56 activation_bias
//  60 mixin_sar       64 head_sum        68 head_shift
//  72 residual_high   76 residual_low    80 residual_bias
//  84 residual_high_shift                88 residual_low_shift
//  92 stream          96 next_ring_high 100 next_ring_low
// 104 next_ring_shift 108 next_low_enabled 112 frames
// 116 residual_narrow_shift

// Stack frame (384 bytes): decoded argument cursors at 20..136, layer 0's
// saved high residual at 160..191, activation high at 240..255, activation low
// at 256..271 and the seven-bit low mask at 288. Slots 320..331 cache
// residual-rounding constants, 332..351 hold the exact layer-0 PReLU constants
// and 352..357 the guarded-S16 endpoints and the cached-affine bias factor.

    // Exact low-32 reconstruction. Keep this QACC read order: it is the one
    // verified on silicon, and the host twin cannot model retirement hazards
    // between RUR, SAR and SRC on the physical LX7 accumulator banks.
    .macro QACC_UNPACK_INTERLEAVED_TO_VECTORS affine=0, preload_mixin=0, preserve_a14=0
    .if \affine
    l32i    a8, a1, 76
    ee.vld.128.ip q4, a8, 16
    ee.vld.128.ip q5, a8, 16
    .endif
    rur.qacc_l_0 a9
    rur.qacc_h_0 a10
    ee.movi.32.q q0, a9, 0
    ee.movi.32.q q1, a10, 0

    rur.qacc_l_1 a11
    rur.qacc_h_1 a12
    rur.qacc_l_2 a13
    .if \preserve_a14
    rur.qacc_h_2 a7
    .else
    rur.qacc_h_2 a14
    .endif
    ssai    8
    src     a8, a13, a11
    ee.movi.32.q q0, a8, 1
    .if \preserve_a14
    src     a8, a7, a12
    .else
    src     a8, a14, a12
    .endif
    ee.movi.32.q q1, a8, 1

    rur.qacc_l_3 a11
    rur.qacc_h_3 a12
    ssai    16
    src     a8, a11, a13
    ee.movi.32.q q0, a8, 2
    .if \preserve_a14
    src     a8, a12, a7
    .else
    src     a8, a12, a14
    .endif
    ee.movi.32.q q1, a8, 2

    rur.qacc_l_4 a13
    .if \preserve_a14
    rur.qacc_h_4 a7
    .else
    rur.qacc_h_4 a14
    .endif
    ssai    24
    src     a8, a13, a11
    ee.movi.32.q q0, a8, 3
    .if \preserve_a14
    src     a8, a7, a12
    .else
    src     a8, a14, a12
    .endif
    ee.movi.32.q q1, a8, 3
    .if \affine
    .if \preload_mixin
    // Retire the low affine add while fetching the packed conditioning row.
    // q2 is dead after the QACC reconstruction and arrives live at activation.
    l32i    a8, a1, 72
    ee.vadds.s32.ld.incp q2, a8, q0, q0, q4
    ee.vadds.s32 q1, q1, q5
    .else
    ee.vadds.s32 q0, q0, q4
    ee.vadds.s32 q1, q1, q5
    .endif
    .endif
    .endm

    // Widen an exact signed-S16 extraction and add the affine bias. The guarded
    // caller checks SRCMB's saturation sentinels before entering this path.
    .macro QACC_WIDEN_S16_TO_AFFINE_VECTORS preload_mixin=0
    ee.xorq q3, q3, q3
    ee.vcmp.lt.s16 q3, q2, q3
    ee.vzip.16 q2, q3
    l32i    a8, a1, 76
    ee.vld.128.ip q4, a8, 16
    ee.vld.128.ip q5, a8, 16
    ee.vadds.s32 q0, q2, q4
    ee.vadds.s32 q1, q3, q5
    .if \preload_mixin
    // The guarded short path still owns q2 until widening completes, so its
    // conditioning row cannot share the affine instruction safely.
    l32i    a8, a1, 72
    ee.vld.128.ip q2, a8, 16
    .endif
    .endm

    // SRCMB shifts all eight QACC40 lanes and returns their signed-S16
    // saturation. If no lane equals either saturation endpoint, q2 is the
    // exact convolution result and the short widening path is safe. An endpoint
    // is deliberately treated as ambiguous and falls back to the full QACC40
    // reconstruction, preserving exact arithmetic for every possible input.
    .macro QACC_GUARDED_S16_TO_AFFINE_VECTORS preload_mixin=0, cached_affine=0
    addi    a8, a1, 352
    ee.vldbc.16.ip q4, a8, 2
    ee.vldbc.16.ip q5, a8, 2
    ee.vcmp.eq.s16 q3, q2, q4
    ee.vcmp.eq.s16 q4, q2, q5
    ee.orq   q3, q3, q4
    ee.movi.32.a q3, a8, 0
    ee.movi.32.a q3, a9, 1
    ee.movi.32.a q3, a10, 2
    ee.movi.32.a q3, a11, 3
    or      a8, a8, a9
    or      a10, a10, a11
    or      a8, a8, a10
    bnez    a8, .Lqacc_s16_slow\@
    .if \cached_affine
    // Cached-affine kernels injected the bias into QACC before SRCMB. Widen
    // the already-complete affine value and preserve q6 as the tile-cached
    // conditioning row.
    ee.xorq q3, q3, q3
    ee.vcmp.lt.s16 q3, q2, q3
    ee.vzip.16 q2, q3
    ee.orq   q0, q2, q2
    ee.orq   q1, q3, q3
    .else
    QACC_WIDEN_S16_TO_AFFINE_VECTORS \preload_mixin
    .endif
    j       .Lqacc_s16_done\@
.Lqacc_s16_slow\@:
    .if \cached_affine
    QACC_UNPACK_INTERLEAVED_TO_VECTORS 0, 0, \cached_affine
    .else
    QACC_UNPACK_INTERLEAVED_TO_VECTORS 1, \preload_mixin
    .endif
.Lqacc_s16_done\@:
    .endm

    // Cached high-layer kernels have already shifted the convolution and bias
    // into their final int32 grid. Compute the conditioning term at its usual
    // SAR, then accumulate that signed-S16 result into the shifted QACC lanes.
    // This is arithmetically identical to widening the term and adding it after
    // extraction, while leaving the expensive int32 unpack with less work.
    .macro QACC_ADD_CACHED_MIXIN refresh_s16=0
    ee.vldbc.16.ip q2, a14, 2
    l32i    a8, a1, 80
    wsr     a8, sar
    ee.vmul.s16 q2, q6, q2
    addi    a8, a1, 328
    ee.vldbc.32.ip q5, a8, 0
    ee.vmulas.s16.qacc q2, q5
    .if \refresh_s16
    // The guarded final-layer path consumes q2 as its saturation sentinel.
    // Refresh it after changing QACC; a zero shift leaves QACC unchanged.
    movi.n  a8, 0
    ee.srcmb.s16.qacc q2, a8, 0
    .endif
    .endm

    // The combined high+low kernels for ordinary narrow layers keep packed
    // all-ones live in q7 for the residual path.  Consume the conditioning
    // row while the shifted convolution is still in QACC, then let the usual
    // exact unpack add the int32 bias.  This removes the packed-S16 sign
    // extension and the two post-unpack vector adds from every eligible frame.
    .macro QACC_ADD_LIVE_MIXIN
    l32i    a8, a1, 72
    ee.vld.128.ip q2, a8, 16
    ee.vldbc.16.ip q3, a14, 2
    l32i    a8, a1, 80
    wsr     a8, sar
    ee.vmul.s16 q2, q2, q3
    ee.vmulas.s16.qacc q2, q7
    .endm

    // Export the low int32 of all eight shifted QACC40 lanes directly.  These
    // are the exact wraparound deltas consumed by the scalar implementation;
    // no saturating SRCMB extraction or ten-word scalar unpack is needed.
    .macro QACC_TO_I32_VECTORS preserve_a14=0
    QACC_UNPACK_INTERLEAVED_TO_VECTORS 0, 0, \preserve_a14
    .endm

    // Apply a residual shift (stack slot 104 for the high limb, 108 for layer
    // 0's low limb) to QACC and return eight exact int32 lanes in q0/q1. SRCMB
    // only represents a right shift; negative grid shifts therefore unpack at
    // shift zero and use PIE VSL afterwards. With save, the lanes are also
    // stored at stack+160.
    .macro QACC_SHIFT_TO_I32_VECTORS shift_slot, save=0, oneslive=0, preserve_a14=0
    l32i    a6, a1, \shift_slot
    bgez    a6, .Lqacc_right\@
    // A negative grid shift means a left shift. QACC still contains the raw
    // accumulator here, so a zero-bit SRCMB was a pure no-op before the exact
    // low-32 unpack.
    QACC_TO_I32_VECTORS \preserve_a14
    neg     a6, a6
    wsr     a6, sar
    ee.vsl.32 q0, q0
    ee.vsl.32 q1, q1
    j       .Lqacc_shift_done\@
.Lqacc_right\@:
    // The residual shift is fixed for the whole eight-frame call. Its packed
    // rounding factor and the all-ones multiplier were cached by the
    // prologue, avoiding eight MOVI.Q writes on every audio frame.
    .if \shift_slot == 104
    addi    a8, a1, 320
    ee.vldbc.32.ip q0, a8, 0
    .if !\oneslive
    addi    a8, a1, 328
    ee.vldbc.32.ip q3, a8, 0
    .endif
    .else
    addi    a8, a1, 324
    ee.vldbc.32.ip q0, a8, 0
    addi    a8, a1, 328
    ee.vldbc.32.ip q3, a8, 0
    .endif
    .if \oneslive
    ee.vmulas.s16.qacc q0, q7
    .else
    ee.vmulas.s16.qacc q0, q3
    .endif
    ee.srcmb.s16.qacc q2, a6, 0
    QACC_TO_I32_VECTORS \preserve_a14
.Lqacc_shift_done\@:
    .if \save
    addi    a8, a1, 160
    ee.vst.128.ip q0, a8, 16
    ee.vst.128.ip q1, a8, 16
    .endif
    .endm

    .macro STREAM_ADD_LIVE_BIAS
    l32i    a8, a1, 112
    mov.n   a10, a8
    ee.vld.128.ip q2, a8, 16
    ee.vld.128.ip q3, a8, 16
    l32i    a8, a1, 100
    // Hide both bias loads behind the independent residual+stream adds.
    ee.vadds.s32.ld.incp q4, a8, q0, q0, q2
    ee.vadds.s32.ld.incp q5, a8, q1, q1, q3
    ee.vadds.s32 q0, q0, q4
    // Store the completed low half while the high-half add retires.
    ee.vadds.s32.st.incp q0, a10, q1, q1, q5
    ee.vst.128.ip q1, a10, 16
    s32i    a10, a1, 112
    .endm

    // The late high-only layers tolerate a calibrated S16 residual delta.
    // SRCMB performs the QACC arithmetic shift and emits all eight lanes in
    // one packed vector. Widen it, restore the deliberately discarded low
    // bits, then update the recurrent stream without scalar QACC40 unpacking.
    // a3=stream, a5=bias, a6=base QACC shift; stack+136=extra narrow shift.
    .macro STREAM_ADD_NARROWED_QACC_BIAS
    l32i    a7, a1, 136
    add     a6, a6, a7
    beqz    a6, .Lnarrow_round_done\@
    addi    a8, a1, 320
    ee.vldbc.32.ip q0, a8, 0
    addi    a8, a1, 324
    ee.vldbc.32.ip q1, a8, 0
    ee.vmulas.s16.qacc q0, q1
.Lnarrow_round_done\@:
    ee.srcmb.s16.qacc q2, a6, 0
    ee.xorq q3, q3, q3
    ee.vcmp.lt.s16 q3, q2, q3
    ee.vzip.16 q2, q3
    wsr     a7, sar
    ee.vsl.32 q2, q2
    ee.vsl.32 q3, q3
    ee.vld.128.ip q4, a3, 16
    ee.vld.128.ip q5, a3, 16
    // Preserve q6/q7, which cache the activation mixin and packed bias for
    // the whole high-only tile. q0/q1 are dead after residual convolution and
    // can receive the two residual-bias loads instead.
    ee.vadds.s32.ld.incp q0, a5, q2, q2, q4
    ee.vadds.s32.ld.incp q1, a5, q3, q3, q5
    ee.vadds.s32 q0, q2, q0
    addi    a3, a3, -32
    ee.vadds.s32.st.incp q0, a3, q1, q3, q1
    ee.vst.128.ip q1, a3, 16
    s32i    a3, a1, 112
    .endm

    // Combine a saved high-limb delta with the live low-limb result before
    // touching the stream. Keeping the second QACC result in q0/q1 avoids
    // writing 32 bytes of scratch and immediately loading them again.
    .macro STREAM_ADD_SAVED_HIGH_LIVE_LOW_BIAS
    addi    a8, a1, 160
    ee.vld.128.ip q2, a8, 16
    ee.vld.128.ip q3, a8, 16
    l32i    a8, a1, 112
    mov.n   a10, a8
    ee.vld.128.ip q4, a8, 16
    ee.vld.128.ip q5, a8, 16
    l32i    a8, a1, 100
    ee.vld.128.ip q6, a8, 16
    ee.vld.128.ip q7, a8, 16
    ee.vadds.s32 q0, q0, q2
    ee.vadds.s32 q1, q1, q3
    ee.vadds.s32 q0, q0, q4
    ee.vadds.s32 q1, q1, q5
    ee.vadds.s32 q0, q0, q6
    ee.vadds.s32 q1, q1, q7
    ee.vst.128.ip q0, a10, 16
    ee.vst.128.ip q1, a10, 16
    s32i    a10, a1, 112
    .endm

    // Prime four weight vectors once per dot-product run. VSMULAS then retires
    // the current weight and loads the vector four inputs ahead in the same
    // instruction. Keeping four registers in flight hides LX7's load-to-use
    // latency and cuts a tap from sixteen PIE instructions to nine.
    .macro PRIME_W16 weight_ptr
    ee.vld.128.ip q1, \weight_ptr, 16
    ee.vld.128.ip q2, \weight_ptr, 16
    ee.vld.128.ip q3, \weight_ptr, 16
    ee.vld.128.ip q4, \weight_ptr, 16
    .endm

    // Convolution-loop schedule with both streams in flight. Three weights
    // stay resident in q1..q3. q4 is filled four instructions before use,
    // while the final fused MAC loads the next input row into q0. This keeps
    // the same eight QACC operations and weight order as DOT_W16, but removes
    // the load-q0/immediate-use dependency at the top of every tap.
    .macro PRIME_W16_PIPE weight_ptr
    ee.vld.128.ip q1, \weight_ptr, 16
    ee.vld.128.ip q2, \weight_ptr, 16
    ee.vld.128.ip q3, \weight_ptr, 16
    .endm

    .macro PRIME_W16_PIPE_INPUT input_ptr
    ee.vld.128.ip q0, \input_ptr, 16
    add     \input_ptr, \input_ptr, a6
    .endm

    .macro DOT_W16_PIPE input_ptr, weight_ptr
    ee.vld.128.ip q4, \weight_ptr, 16
    ee.vsmulas.s16.qacc.ld.incp q1, \weight_ptr, q1, q0, 0
    ee.vsmulas.s16.qacc.ld.incp q2, \weight_ptr, q2, q0, 1
    ee.vsmulas.s16.qacc.ld.incp q3, \weight_ptr, q3, q0, 2
    ee.vsmulas.s16.qacc.ld.incp q4, \weight_ptr, q4, q0, 3
    ee.vsmulas.s16.qacc.ld.incp q1, \weight_ptr, q1, q0, 4
    ee.vsmulas.s16.qacc.ld.incp q2, \weight_ptr, q2, q0, 5
    ee.vsmulas.s16.qacc.ld.incp q3, \weight_ptr, q3, q0, 6
    ee.vsmulas.s16.qacc.ld.incp q0, \input_ptr, q4, q0, 7
    // Keep this update behind every QACC write; earlier scheduling has
    // produced stale physical lanes on LX7 despite equivalent dataflow.
    add     \input_ptr, \input_ptr, a6
    .endm

    .macro DOT_W16 input_ptr, weight_ptr
    ee.vld.128.ip q0, \input_ptr, 16
    ee.vsmulas.s16.qacc.ld.incp q1, \weight_ptr, q1, q0, 0
    ee.vsmulas.s16.qacc.ld.incp q2, \weight_ptr, q2, q0, 1
    ee.vsmulas.s16.qacc.ld.incp q3, \weight_ptr, q3, q0, 2
    ee.vsmulas.s16.qacc.ld.incp q4, \weight_ptr, q4, q0, 3
    ee.vsmulas.s16.qacc.ld.incp q1, \weight_ptr, q1, q0, 4
    ee.vsmulas.s16.qacc.ld.incp q2, \weight_ptr, q2, q0, 5
    ee.vsmulas.s16.qacc.ld.incp q3, \weight_ptr, q3, q0, 6
    ee.vsmulas.s16.qacc.ld.incp q4, \weight_ptr, q4, q0, 7
    .endm

    // Load one eight-byte low-limb ring row, sign-extend it to eight S16
    // lanes, then use the exact S16 QACC layout. S8 QACC has sixteen 20-bit
    // lanes on LX7 and therefore cannot represent eight pairwise dot sums.
    .macro DOT_W16_S8_INPUT input_ptr, weight_ptr, advance=0
    ee.vld.l.64.ip q0, \input_ptr, 8
    ee.xorq q5, q5, q5
    ee.vcmp.lt.s8 q5, q0, q5
    ee.vzip.8 q0, q5
    ee.vsmulas.s16.qacc.ld.incp q1, \weight_ptr, q1, q0, 0
    ee.vsmulas.s16.qacc.ld.incp q2, \weight_ptr, q2, q0, 1
    ee.vsmulas.s16.qacc.ld.incp q3, \weight_ptr, q3, q0, 2
    ee.vsmulas.s16.qacc.ld.incp q4, \weight_ptr, q4, q0, 3
    ee.vsmulas.s16.qacc.ld.incp q1, \weight_ptr, q1, q0, 4
    ee.vsmulas.s16.qacc.ld.incp q2, \weight_ptr, q2, q0, 5
    ee.vsmulas.s16.qacc.ld.incp q3, \weight_ptr, q3, q0, 6
    ee.vsmulas.s16.qacc.ld.incp q4, \weight_ptr, q4, q0, 7
    .if \advance
    add     \input_ptr, \input_ptr, a6
    .endif
    .endm

    // The high-limb convolution of a high-only layer. The stack slots hold the
    // two ring runs, the weights, the stride correction and the shift.
    .macro CONV_W16 run0_slot, run1_slot, weights_slot, correction_slot, shift_slot, guarded_s16=0, preload_mixin=0, cached_affine=0, fold_mixin=0
    l32i    a3, a1, \run0_slot
    add     a3, a3, a15
    l32i    a4, a1, 24
    l32i    a5, a1, \weights_slot
    l32i    a6, a1, \correction_slot
    ee.zero.qacc
    PRIME_W16_PIPE a5
    PRIME_W16_PIPE_INPUT a3
    loopnez a4, .Lw16_first_done\@
    DOT_W16_PIPE a3, a5
.Lw16_first_done\@:
.Lw16_second\@:
    l32i    a3, a1, \run1_slot
    add     a3, a3, a15
    l32i    a4, a1, 28
    beqz    a4, .Lw16_extract\@
    PRIME_W16_PIPE_INPUT a3
    loopnez a4, .Lw16_second_done\@
    DOT_W16_PIPE a3, a5
.Lw16_second_done\@:
.Lw16_extract\@:
    .if \cached_affine
    // Add bias in the QACC domain before its arithmetic shift. Bias * 2^shift
    // is exactly equivalent to a post-shift int32 vector add.
    addi    a8, a1, 356
    ee.vldbc.16.ip q0, a8, 0
    ee.vmulas.s16.qacc q7, q0
    .endif
    l32i    a7, a1, \shift_slot
    ee.srcmb.s16.qacc q2, a7, 0
    .if \fold_mixin
    QACC_ADD_CACHED_MIXIN \guarded_s16
    .endif
    .if \guarded_s16
    QACC_GUARDED_S16_TO_AFFINE_VECTORS \preload_mixin, \cached_affine
    .elseif \cached_affine
    QACC_UNPACK_INTERLEAVED_TO_VECTORS 0, 0, 1
    .else
    QACC_UNPACK_INTERLEAVED_TO_VECTORS 1, \preload_mixin
    .endif
    .endm

    // Both convolution limbs differ by two binary places. The low table is
    // pre-scaled by four, so accumulate both dot products into one QACC and
    // perform a single extraction at the high-limb shift.
    .macro CONV_COMBINED_W16_LOW_RING preload_mixin=0, guarded_s16=0, fold_mixin=0
    l32i    a3, a1, 20
    add     a3, a3, a15
    l32i    a4, a1, 24
    l32i    a5, a1, 32
    l32i    a6, a1, 36
    ee.zero.qacc
    PRIME_W16_PIPE a5
    PRIME_W16_PIPE_INPUT a3
    loopnez a4, .Lcombined_high_first_done\@
    DOT_W16_PIPE a3, a5
.Lcombined_high_first_done\@:
.Lcombined_high_second\@:
    l32i    a3, a1, 44
    add     a3, a3, a15
    l32i    a4, a1, 28
    beqz    a4, .Lcombined_low_first\@
    PRIME_W16_PIPE_INPUT a3
    loopnez a4, .Lcombined_high_second_done\@
    DOT_W16_PIPE a3, a5
.Lcombined_high_second_done\@:
.Lcombined_low_first\@:
    srai    a8, a15, 1
    l32i    a3, a1, 48
    add     a3, a3, a8
    l32i    a4, a1, 24
    l32i    a5, a1, 56
    l32i    a6, a1, 60
    PRIME_W16 a5
    loopnez a4, .Lcombined_low_first_done\@
    DOT_W16_S8_INPUT a3, a5, 1
.Lcombined_low_first_done\@:
.Lcombined_low_second\@:
    l32i    a3, a1, 52
    add     a3, a3, a8
    l32i    a4, a1, 28
    beqz    a4, .Lcombined_extract\@
    loopnez a4, .Lcombined_low_second_done\@
    DOT_W16_S8_INPUT a3, a5, 1
.Lcombined_low_second_done\@:
.Lcombined_extract\@:
    l32i    a7, a1, 40
    ee.srcmb.s16.qacc q2, a7, 0
    .if \fold_mixin
    QACC_ADD_LIVE_MIXIN
    .endif
    .if \guarded_s16
    QACC_GUARDED_S16_TO_AFFINE_VECTORS \preload_mixin
    .else
    QACC_UNPACK_INTERLEAVED_TO_VECTORS 1, (\preload_mixin && !\fold_mixin), \fold_mixin
    .endif
    .endm

    // Eight-channel LX7 epilogue for the rational-PReLU layers.  The
    // convolution affine arrives in q0/q1 as eight signed int32 lanes.  Keep
    // the conditioning multiply, PReLU, head accumulation and residual
    // limb split in PIE registers; a scalar lane loop costs almost as much as
    // the convolution itself on the small high-only layers.
    //
    // Unless preconditioned (the convolution already folded it into QACC), the
    // conditioning row arrives in q2, or in q6 when mixincached.
    .macro ACTIVATE_VECTOR_RATIONAL residualmode, wide=0, mixincached=0, preconditioned=0
    // condition * mixin, exactly matching MUL16S followed by scalar SRA.
    .if !\preconditioned
    l32i    a12, a1, 68
    ee.vldbc.16.ip q3, a12, 2
    s32i    a12, a1, 68
    l32i    a13, a1, 80
    .if \wide
    // S32 conditioning for blocks whose input peak could overflow the S16
    // product: take the 16-bit product windows at SAR and SAR + 16 and zip
    // them into full int32 lanes.
    wsr     a13, sar
    .if \mixincached
    ee.vmul.s16 q4, q6, q3
    .else
    ee.vmul.s16 q4, q2, q3
    .endif
    addi.n  a13, a13, 16
    wsr     a13, sar
    .if \mixincached
    ee.vmul.s16 q5, q6, q3
    .else
    ee.vmul.s16 q5, q2, q3
    .endif
    ee.vzip.16 q4, q5
    .else
    // Fast packed path for blocks whose shifted product is proven to fit S16.
    wsr     a13, sar
    ee.vmul.s16 q2, q2, q3
    ee.xorq q3, q3, q3
    ee.vcmp.lt.s16 q3, q2, q3
    ee.vzip.16 q2, q3
    .endif
    .if \wide
    ee.vadds.s32 q0, q0, q4
    ee.vadds.s32 q1, q1, q5
    .else
    ee.vadds.s32 q0, q0, q2
    ee.vadds.s32 q1, q1, q3
    .endif
    .endif

    // 41/4096 = 0.010009765625 approximates the 0.01 PReLU slope; the host
    // twin and the import tuner measure exactly this arithmetic. Three
    // arithmetic shifts and two adds are far cheaper on PIE v1 than narrowing
    // to S16, multiplying, widening again, and building two lane masks.  VMAX
    // performs the selection for free: positive x is larger than 0.01*x, while
    // negative 0.01*x is larger than x.
    ssai    7
    ee.vsr.32 q2, q0
    ee.vsr.32 q3, q1
    ssai    9
    ee.vsr.32 q4, q0
    ee.vsr.32 q5, q1
    ee.vadds.s32 q2, q2, q4
    ee.vadds.s32 q3, q3, q5
    // Reuse x>>9: arithmetic shifts compose exactly, so (x>>9)>>3 is x>>12.
    ssai    3
    ee.vsr.32 q4, q4
    ee.vsr.32 q5, q5
    ee.vadds.s32 q2, q2, q4
    ee.vadds.s32 q3, q3, q5
    // The head row is needed after the PReLU on every rational layer. Load
    // both halves while VMAX performs the final positive/negative selection.
    l32i    a10, a1, 84
    ee.vmax.s32.ld.incp q4, a10, q0, q0, q2
    ee.vmax.s32.ld.incp q5, a10, q1, q1, q3

    .if \residualmode == 4
    // The high-narrow layers only need the high residual limb. Emit it first,
    // then let head accumulation consume q0/q1 directly.
    addi    a8, a1, 240
    ssai    7
    ee.vsr.32 q2, q0
    ee.vsr.32 q3, q1
    ee.vunzip.16 q2, q3
    ee.vst.128.ip q2, a8, 16
    l32i    a13, a1, 88
    wsr     a13, sar
    ee.vsl.32 q0, q0
    ee.vsl.32 q1, q1
    ee.vadds.s32 q0, q0, q4
    addi    a10, a10, -32
    ee.vadds.s32.st.incp q0, a10, q1, q1, q5
    ee.vst.128.ip q1, a10, 16
    s32i    a10, a1, 84
    .else
    .if \residualmode == 0
    // The final layer has no residual consumer, so accumulate the head in
    // q0/q1 directly instead of copying both vectors first.
    l32i    a13, a1, 88
    wsr     a13, sar
    ee.vsl.32 q0, q0
    ee.vsl.32 q1, q1
    ee.vadds.s32 q0, q0, q4
    addi    a10, a10, -32
    ee.vadds.s32.st.incp q0, a10, q1, q1, q5
    ee.vst.128.ip q1, a10, 16
    s32i    a10, a1, 84
    .else
    // Preserve q0/q1 for the residual split while accumulating exact
    // activations into the head.
    ee.orq q2, q0, q0
    ee.orq q3, q1, q1
    l32i    a13, a1, 88
    wsr     a13, sar
    ee.vsl.32 q2, q2
    ee.vsl.32 q3, q3
    ee.vadds.s32 q2, q2, q4
    addi    a10, a10, -32
    ee.vadds.s32.st.incp q2, a10, q3, q3, q5
    ee.vst.128.ip q3, a10, 16
    s32i    a10, a1, 84
    .endif

    .if \residualmode != 0
    addi    a8, a1, 240
    addi    a9, a1, 256
    ssai    7
    ee.vsr.32 q2, q0
    ee.vsr.32 q3, q1
    ee.vunzip.16 q2, q3
    ee.vst.128.ip q2, a8, 16
    .if \residualmode == 5
    ee.vunzip.16 q0, q1
    ee.vunzip.8 q0, q1
    // q6 is unused by the combined high+low residual path, so the mask loaded
    // in the tile prologue remains live for both consumers in every frame.
    ee.andq q0, q0, q6
    ee.vst.l.64.ip q0, a9, 8
    .endif
    .endif
    .endif
    .endm

    // Exact vector epilogue for layer 0. The ordinary vector kernels use
    // 41/4096 for their qualified PReLU approximation, but layer 0 is too
    // sensitive to that substitution. Divide the negative magnitude by 100
    // with three unsigned-16 QACC stages instead:
    //
    //   n = A*2^16 + B, C = 0x51eb851f = Ch*2^16 + Cl
    //   T = (B*Cl) >> 16
    //   U = (A*Cl + B*Ch + T) >> 16
    //   q = (A*Ch + U) >> 5
    //
    // This is exactly (n*C)>>37 for every clamped A2 activation. Adding 50
    // to the magnitude before the multiply preserves the scalar round-to-
    // nearest division by 100. VMAX then selects x for non-negative lanes
    // and -q for negative lanes.
    //
    // CONDITION_L0_SCALAR_TO_VECTORS has already added the exact scalar
    // MUL16S/SRA conditioning result to the live affine vectors in q0/q1.
    .macro ACTIVATE_VECTOR_L0_EXACT

    // n = max(-x, 0) + 50. Clamping the magnitude to zero keeps positive
    // lanes out of the unsigned division while avoiding a separate mask.
    ee.xorq q4, q4, q4
    ee.vsubs.s32 q2, q4, q0
    ee.vsubs.s32 q3, q4, q1
    ee.vmax.s32 q2, q2, q4
    ee.vmax.s32 q3, q3, q4
    addi    a12, a1, 332
    ee.vldbc.32.ip q5, a12, 0
    ee.vadds.s32 q2, q2, q5
    ee.vadds.s32 q3, q3, q5
    // q2 = packed B, q3 = packed A.
    ee.vunzip.16 q2, q3

    // T = (B*Cl) >> 16.
    addi    a12, a1, 336
    ee.vldbc.16.ip q4, a12, 0
    ee.zero.qacc
    ee.vmulas.u16.qacc q2, q4
    movi.n  a13, 16
    ee.srcmb.s16.qacc q5, a13, 0

    // U = (A*Cl + B*Ch + T) >> 16.
    ee.zero.qacc
    ee.vmulas.u16.qacc q3, q4
    addi.n  a12, a12, 2
    ee.vldbc.16.ip q4, a12, 0
    ee.vmulas.u16.qacc q2, q4
    addi.n  a12, a12, 2
    ee.vldbc.16.ip q6, a12, 0
    ee.vmulas.u16.qacc q5, q6
    ee.srcmb.s16.qacc q5, a13, 0

    // q = (A*Ch + U) >> 5.
    ee.zero.qacc
    addi.n  a12, a12, -2
    ee.vldbc.16.ip q4, a12, 0
    ee.vmulas.u16.qacc q3, q4
    ee.vmulas.u16.qacc q5, q6
    movi.n  a13, 5
    ee.srcmb.s16.qacc q5, a13, 0

    // Widen q, negate it and select the exact PReLU result.
    ee.xorq q6, q6, q6
    ee.vzip.16 q5, q6
    ee.xorq q4, q4, q4
    ee.vsubs.s32 q5, q4, q5
    ee.vsubs.s32 q6, q4, q6
    ee.vmax.s32 q0, q0, q5
    ee.vmax.s32 q1, q1, q6

    // Exact A22 clamp.
    addi    a12, a1, 344
    ee.vldbc.32.ip q2, a12, 4
    ee.vldbc.32.ip q3, a12, 0
    ee.vmax.s32 q0, q0, q2
    ee.vmax.s32 q1, q1, q2
    ee.vmin.s32 q0, q0, q3
    ee.vmin.s32 q1, q1, q3

    // Accumulate the activated row into the head while preserving q0/q1 for
    // layer 0's high+W16-low residual split.
    ee.orq q2, q0, q0
    ee.orq q3, q1, q1
    l32i    a13, a1, 88
    wsr     a13, sar
    ee.vsl.32 q2, q2
    ee.vsl.32 q3, q3
    l32i    a10, a1, 84
    ee.vld.128.ip q4, a10, 16
    ee.vld.128.ip q5, a10, 16
    ee.vadds.s32 q2, q2, q4
    ee.vadds.s32 q3, q3, q5
    addi    a10, a10, -32
    ee.vst.128.ip q2, a10, 16
    ee.vst.128.ip q3, a10, 16
    s32i    a10, a1, 84

    // Layer 0 emits a signed high limb and an unsigned seven-bit low limb
    // widened to S16 for its exact residual tables.
    addi    a8, a1, 240
    ssai    7
    ee.vsr.32 q2, q0
    ee.vsr.32 q3, q1
    ee.vunzip.16 q2, q3
    ee.vst.128.ip q2, a8, 16
    ee.vunzip.16 q0, q1
    addi    a13, a1, 288
    ee.vldbc.16.ip q2, a13, 0
    ee.andq q0, q0, q2
    addi    a9, a1, 256
    ee.vst.128.ip q0, a9, 16
    .endm

    // Preserve layer 0's exact scalar conditioning multiply while leaving its
    // divide-by-100 PReLU to the vector epilogue above. Build the eight scalar
    // products directly in q2/q3, then add them to the live affine vectors.
    // This removes the affine scratch round-trip without changing arithmetic.
    .macro CONDITION_L0_SCALAR_TO_VECTORS
    l32i    a6, a1, 72
    l32i    a11, a1, 68
    srai    a12, a15, 3
    add     a11, a11, a12
    l16si   a11, a11, 0
    l32i    a13, a1, 80
    ssr     a13
    .set clane, 0
    .rept 8
    l16si   a14, a6, (clane * 2)
    mul16s  a14, a14, a11
    sra     a14, a14
    .if clane < 4
    ee.movi.32.q q2, a14, clane
    .else
    ee.movi.32.q q3, a14, (clane - 4)
    .endif
    .set clane, clane + 1
    .endr
    ee.vadds.s32 q0, q0, q2
    ee.vadds.s32 q1, q1, q3
    .endm

    // Split the updated stream into the next layer's high and S8 low rings.
    // Residual accumulation leaves the stream live in q0/q1.
    .macro QUANTIZE_NEXT_LANES_LIVE masklive=0
    // Shift four signed stream lanes at once.  EE.VSR.32 is an arithmetic
    // per-lane shift and therefore matches scalar SRA exactly.  There is no
    // A22 clamp: the calibrated grids keep the stream inside the ring range.
    // Reuse the consumed exact-activation scratch for the narrowed values.
    ee.vsr.32 q0, q0
    ee.vsr.32 q1, q1

    // High limb: arithmetic >> 7, then collect the low half of each int32
    // into the packed S16 ring vector.
    ssai    7
    ee.vsr.32 q3, q0
    ee.vsr.32 q4, q1
    ee.vunzip.16 q3, q4
    ee.vst.128.ip q3, a3, 16
    s32i    a3, a1, 116

    // Low limb: collect the low byte of each int32 and clear bit 7, matching
    // scalar extui(value, 0, 7), then store all eight lanes at once.
    ee.vunzip.16 q0, q1
    ee.vunzip.8 q0, q1
    .if \masklive
    ee.andq q0, q0, q6
    .else
    addi    a6, a1, 288
    ee.vldbc.8.ip q2, a6, 0
    ee.andq q0, q0, q2
    .endif
    ee.vst.l.64.ip q0, a4, 8
    s32i    a4, a1, 120
    .endm

    // High-only layers use the same signed narrowing rule as
    // s3_a2full_quantize8_narrow: shift by the ring grid plus the seven-bit
    // limb boundary and retain the low 16 bits. VUNZIP.16 collects the even
    // halfwords (the low half of each shifted int32) from both vectors into
    // one packed eight-lane vector. There is deliberately no A22 clamp here.
    .macro QUANTIZE_NEXT_HIGH_ONLY_LANES_LIVE
    ee.vsr.32 q0, q0
    ee.vsr.32 q1, q1
    ee.vunzip.16 q0, q1
    ee.vst.128.ip q0, a3, 16
    s32i    a3, a1, 116
    .endm

    // Layers 0..10 always feed a split high+low ring, so both pointers are
    // guaranteed here.
    .macro QUANTIZE_NEXT_LIVE_BOTH masklive=0
    l32i    a3, a1, 116
    l32i    a4, a1, 120
    l32i    a5, a1, 124
    ssr     a5
    QUANTIZE_NEXT_LANES_LIVE \masklive
    .endm

    // Layers 11..21 can only feed another high-only ring. The caller only
    // selects this kernel when the destination is present, so no per-frame
    // null test is needed.
    .macro QUANTIZE_NEXT_LIVE_HIGH_ONLY
    l32i    a3, a1, 116
    l32i    a5, a1, 124
    addi.n  a5, a5, 7
    ssr     a5
    QUANTIZE_NEXT_HIGH_ONLY_LANES_LIVE
    .endm

    // One eight-frame layer call.
    //
    //   low:          0 high-only history ring, 1 split ring with an S8 low limb
    //   rational:     1 rational PReLU with S16 conditioning, 3 the same with
    //                 S32 conditioning, 4 layer 0 (scalar conditioning, exact
    //                 divide-by-100 PReLU)
    //   residualmode: 0 final layer (no residual), 1 exact high-only residual,
    //                 3 layer 0 (high and S16-widened low limbs rounded
    //                 separately), 4 narrowed high-only residual, 5 high and S8
    //                 low limbs in one QACC
    //   nextmode:     0 no next ring, 2 high-only next ring, 3 split next ring
    //   convguarded:  extract the convolution as S16 when no lane saturates
    //   cached_affine: fold bias and conditioning into QACC before extraction
    .macro FUSED_FUNCTION name, low, rational, residualmode, nextmode, convguarded=0, cached_affine=0
    // One input section per entry point, so the linker's --gc-sections keeps
    // only the variants the C++ actually references; on the S3 every IRAM byte
    // is a DRAM byte the heap never gets. The literals follow their function
    // into .iram1.\name.literal.
    .section .iram1.\name, "ax"
    .align 4
    .global \name
    .type \name, @function
\name:
    entry   a1, 384
    mov.n   a14, a2
    // Keep one negative byte offset live for the whole block. The ring-window
    // bases below point one block past frame zero, so adding a15 selects the
    // current high row; a15>>1 selects the packed low row.
    l32i    a15, a14, 112
    slli    a15, a15, 4
    l32i    a13, a14, 0
    add     a13, a13, a15
    s32i    a13, a1, 20
    l32i    a13, a14, 4
    s32i    a13, a1, 24
    l32i    a13, a14, 24
    s32i    a13, a1, 28
    l32i    a13, a14, 8
    s32i    a13, a1, 32
    l32i    a13, a14, 12
    addi    a13, a13, -16
    s32i    a13, a1, 36
    l32i    a13, a14, 16
    s32i    a13, a1, 40
    l32i    a13, a14, 20
    add     a13, a13, a15
    s32i    a13, a1, 44
    .if \low
    srai    a12, a15, 1
    l32i    a13, a14, 28
    add     a13, a13, a12
    s32i    a13, a1, 48
    l32i    a13, a14, 44
    add     a13, a13, a12
    s32i    a13, a1, 52
    l32i    a13, a14, 32
    s32i    a13, a1, 56
    l32i    a13, a14, 36
    addi    a13, a13, -8
    s32i    a13, a1, 60
    l32i    a13, a14, 40
    s32i    a13, a1, 64
    .endif
    l32i    a13, a14, 48
    .if \rational == 4
    srai    a12, a15, 3
    add     a13, a13, a12
    .endif
    s32i    a13, a1, 68
    l32i    a13, a14, 52
    s32i    a13, a1, 72
    l32i    a13, a14, 56
    s32i    a13, a1, 76
    l32i    a13, a14, 60
    s32i    a13, a1, 80
    l32i    a13, a14, 64
    s32i    a13, a1, 84
    l32i    a13, a14, 68
    s32i    a13, a1, 88
    .if \residualmode != 0
    l32i    a13, a14, 72
    s32i    a13, a1, 92
    .endif
    .if \residualmode == 5
    l32i    a13, a14, 76
    s32i    a13, a1, 96
    .endif
    .if \residualmode != 0
    l32i    a13, a14, 80
    s32i    a13, a1, 100
    l32i    a13, a14, 84
    s32i    a13, a1, 104
    .endif
    .if \residualmode != 0
    // Cache the two packed-S16 rounding operands once per eight-frame tile.
    // Slots 320/324 normally hold high/low half-LSBs; slot 328 holds ones.
    // The narrowed path instead uses 320/324 for two factors: a half-LSB of
    // 32768 cannot fit positive S16. For shifts 16..31, use the positive
    // product (-32768) * -(1 << (shift - 16)). Keep slot 328 as ones because
    // cached conditioning also uses it.
    movi.n  a9, 0
    .if \residualmode == 4
    movi.n  a12, 1
    // The narrowed path shifts QACC by the ordinary residual grid plus its
    // model-derived S16 safety shift. Cache the half-LSB for that total.
    l32i    a10, a14, 116
    add     a13, a13, a10
    .endif
    bltz    a13, .Lround_high_cached\@
    beqz    a13, .Lround_high_cached\@
    .if \residualmode == 4
    addi    a10, a13, -16
    bgez    a10, .Lround_high_wide\@
    .endif
    addi.n  a10, a13, -1
    movi.n  a9, 1
    ssl     a10
    sll     a9, a9
    .if \residualmode == 4
    j       .Lround_high_pack\@
.Lround_high_wide\@:
    movi    a9, -32768
    movi.n  a12, -1
    ssl     a10
    sll     a12, a12
.Lround_high_pack\@:
    extui   a9, a9, 0, 16
    extui   a12, a12, 0, 16
    slli    a10, a12, 16
    or      a12, a12, a10
    .endif
    slli    a10, a9, 16
    or      a9, a9, a10
.Lround_high_cached\@:
    s32i    a9, a1, 320
    .if \residualmode == 4
    s32i    a12, a1, 324
    .endif
    movi    a9, 0x00010001
    s32i    a9, a1, 328
    .if \residualmode == 5
    addi    a10, a1, 328
    ee.vldbc.32.ip q7, a10, 0
    .endif
    .endif
    .if \residualmode == 3
    l32i    a13, a14, 88
    s32i    a13, a1, 108
    movi.n  a9, 0
    bltz    a13, .Lround_low_cached\@
    beqz    a13, .Lround_low_cached\@
    addi.n  a10, a13, -1
    movi.n  a9, 1
    ssl     a10
    sll     a9, a9
    slli    a10, a9, 16
    or      a9, a9, a10
.Lround_low_cached\@:
    s32i    a9, a1, 324
    .endif
    .if \residualmode != 0
    l32i    a13, a14, 92
    s32i    a13, a1, 112
    .endif
    .if \nextmode != 0
    l32i    a13, a14, 96
    s32i    a13, a1, 116
    .endif
    .if \nextmode == 3
    l32i    a13, a14, 100
    s32i    a13, a1, 120
    .endif
    .if \nextmode != 0
    l32i    a13, a14, 104
    s32i    a13, a1, 124
    .endif
    .if \residualmode == 4
    l32i    a13, a14, 116
    s32i    a13, a1, 136
    .endif
    .if \residualmode == 3 || \residualmode == 5 || \nextmode == 3
    // Activation-low and next-ring-low extraction share this mask. The value
    // remains live in stack scratch for every frame in the tile.
    movi.n  a13, 127
    s32i    a13, a1, 288
    .if \residualmode == 5
    addi    a13, a1, 288
    ee.vldbc.8.ip q6, a13, 0
    .endif
    .endif
    .if \rational == 4
    // Constants for the exact layer-0 QACC PReLU. These are loaded once per
    // tile and broadcast by the vector epilogue in each frame.
    movi.n  a13, 50
    s32i    a13, a1, 332
    movi    a13, 0x851f
    s16i    a13, a1, 336
    movi    a13, 0x51eb
    s16i    a13, a1, 338
    movi.n  a13, 1
    s16i    a13, a1, 340
    movi    a13, -2097151
    s32i    a13, a1, 344
    movi    a13, 2097151
    s32i    a13, a1, 348
    .endif
    .if \convguarded
    // Ambiguous SRCMB endpoints for the exact guarded-S16 convolution path.
    movi    a13, 32767
    s16i    a13, a1, 352
    movi    a13, -32768
    s16i    a13, a1, 354
    .endif
    .if \cached_affine
    // Cache the high-only layer's eight S16 mixin coefficients in q6 and its
    // eight qualified S16 bias values in q7 for all eight frames in the tile.
    l32i    a8, a1, 76
    ee.vld.128.ip q7, a8, 16
    ee.vld.128.ip q6, a8, 16
    ee.vunzip.16 q7, q6
    l32i    a8, a1, 72
    ee.vld.128.ip q6, a8, 16
    l32i    a13, a1, 40
    movi.n  a9, 1
    ssl     a13
    sll     a9, a9
    s16i    a9, a1, 356
    .if \rational == 1
    movi    a9, 0x00010001
    s32i    a9, a1, 328
    // The narrow cached path consumes its conditioning row before QACC
    // extraction, so no later stage needs the stack copy of this cursor.
    // Keep it advancing in a14 across the complete eight-frame tile.
    l32i    a14, a1, 68
    .endif
    .endif
    .if !\cached_affine && \rational == 1 && \residualmode == 5 && !\convguarded
    // The ordinary narrow low-layer path also consumes conditioning before
    // extraction. Preserve its advancing cursor through both QACC unpacks.
    l32i    a14, a1, 68
    .endif
    neg     a15, a15
    // GAS pads .align with zero bytes.  Keep those bytes out of executable
    // fall-through when a different link layout moves this prologue off the
    // requested boundary.
    j       .Lfused_frame\@
    .align  4
.Lfused_frame\@:
    // The convolution leaves its affine result in q0/q1 for the activation.
    .if \low
    CONV_COMBINED_W16_LOW_RING (\rational != 4 && !\cached_affine), \convguarded, (\rational == 1 && \residualmode == 5 && !\convguarded)
    .else
    CONV_W16 20, 44, 32, 36, 40, \convguarded, (\rational != 4 && !\cached_affine), \cached_affine, (\cached_affine && \rational == 1)
    .endif

    .if \rational == 4
    CONDITION_L0_SCALAR_TO_VECTORS
    ACTIVATE_VECTOR_L0_EXACT
    .else
    ACTIVATE_VECTOR_RATIONAL \residualmode, (\rational == 3), \cached_affine, ((\cached_affine && \rational == 1) || (\rational == 1 && \residualmode == 5 && !\convguarded))
    .endif

    .if \residualmode != 0
    addi    a2, a1, 240
    l32i    a4, a1, 92
    .if \residualmode == 4
    // The narrowed path consumes these pointers and the shift directly.
    // Exact paths reload the shift inside QACC_SHIFT_TO_I32_VECTORS and
    // reload stream/bias in their vector add helper, so preloading them here
    // only creates dead instructions in every frame.
    l32i    a3, a1, 112
    l32i    a5, a1, 100
    l32i    a6, a1, 104
    .endif
    ee.zero.qacc
    PRIME_W16 a4
    DOT_W16 a2, a4
    .if \residualmode == 5
    // Layers 1..11 use residual_low_shift = residual_high_shift - 2.
    // Their low table is pre-scaled by four at model load, so both limbs can
    // share this QACC and one final round/unpack instead of serializing QACC
    // twice per frame.
    addi    a2, a1, 256
    l32i    a4, a1, 96
    PRIME_W16 a4
    DOT_W16_S8_INPUT a2, a4
    QACC_SHIFT_TO_I32_VECTORS 104, 0, 1, (\rational == 1 && !\convguarded)
    STREAM_ADD_LIVE_BIAS
    .elseif \residualmode == 4
    STREAM_ADD_NARROWED_QACC_BIAS
    .elseif \residualmode == 1
    QACC_SHIFT_TO_I32_VECTORS 104
    STREAM_ADD_LIVE_BIAS
    .else
    // Layer 0: the high limb is saved at stack+160, then the S16-widened low
    // limb is rounded at its own shift and both are added to the stream.
    QACC_SHIFT_TO_I32_VECTORS 104, 1
    addi    a2, a1, 256
    l32i    a4, a1, 92
    ee.zero.qacc
    PRIME_W16 a4
    DOT_W16 a2, a4
    QACC_SHIFT_TO_I32_VECTORS 108
    STREAM_ADD_SAVED_HIGH_LIVE_LOW_BIAS
    .endif
    .if \nextmode == 2
    QUANTIZE_NEXT_LIVE_HIGH_ONLY
    .elseif \nextmode == 3
    QUANTIZE_NEXT_LIVE_BOTH (\residualmode == 5)
    .endif
    .endif

    addi    a15, a15, 16
    bnez    a15, .Lfused_frame\@
    retw
    .size \name, .-\name
    .endm

    // Layer 0 keeps its scalar-exact conditioning multiply, then uses the
    // exact vector /100 PReLU; narrowing the conditioning product destabilizes
    // the recurrent state. Its convolution frequently fits the exact
    // packed-S16 result. Endpoint lanes are ambiguous (a real endpoint or
    // saturation), so they branch to the full QACC40 reconstruction for that
    // frame.
    FUSED_FUNCTION s3_a2full_fused_layer8_l0_s8, 1, 4, 3, 3, 1
    FUSED_FUNCTION s3_a2full_fused_layer8_low_s8, 1, 1, 5, 3
    FUSED_FUNCTION s3_a2full_fused_layer8_low_s8_next_high, 1, 1, 5, 2
    // Layer 5 stays inside signed S16 for most qualified frames. Ambiguous
    // saturation endpoints fall back to the exact QACC40 reconstruction.
    FUSED_FUNCTION s3_a2full_fused_layer8_low_s8_conv_s16_guarded, 1, 1, 5, 3, 1
    FUSED_FUNCTION s3_a2full_fused_layer8_high_narrow_uncached, 0, 1, 4, 2
    FUSED_FUNCTION s3_a2full_fused_layer8_high_narrow, 0, 1, 4, 2, 0, 1
    // High-only layers the tuner marked for exact residual extraction.
    FUSED_FUNCTION s3_a2full_fused_layer8_high_exact_uncached, 0, 1, 1, 2
    // Exact residual extraction must also survive a full-width input mixin.
    FUSED_FUNCTION s3_a2full_fused_layer8_high_exact_wide_mixin_trunc, 0, 3, 1, 2
    FUSED_FUNCTION s3_a2full_fused_layer8_final_conv_s16_guarded, 0, 1, 0, 0, 1
    // The final layer has the same model-qualified S16 bias/shift form as the
    // other high-only layers. Keep its bias and conditioning row resident for
    // the eight-frame tile and complete the affine in QACC before extraction.
    FUSED_FUNCTION s3_a2full_fused_layer8_final_cached_conv_s16_guarded, 0, 1, 0, 0, 1, 1
    // Range-selected calls use 32-bit conditioning while retaining the
    // faster truncating convolution path.
    FUSED_FUNCTION s3_a2full_fused_layer8_low_s8_wide_mixin_trunc, 1, 3, 5, 3
    FUSED_FUNCTION s3_a2full_fused_layer8_low_s8_conv_s16_guarded_wide_mixin_trunc, 1, 3, 5, 3, 1
    FUSED_FUNCTION s3_a2full_fused_layer8_low_s8_next_high_wide_mixin_trunc, 1, 3, 5, 2
    FUSED_FUNCTION s3_a2full_fused_layer8_high_narrow_wide_mixin_trunc_uncached, 0, 3, 4, 2
    FUSED_FUNCTION s3_a2full_fused_layer8_high_narrow_wide_mixin_trunc, 0, 3, 4, 2, 0, 1
    FUSED_FUNCTION s3_a2full_fused_layer8_final_wide_mixin_trunc_conv_s16_guarded, 0, 3, 0, 0, 1
    FUSED_FUNCTION s3_a2full_fused_layer8_final_cached_wide_mixin_trunc_conv_s16_guarded, 0, 3, 0, 0, 1, 1
