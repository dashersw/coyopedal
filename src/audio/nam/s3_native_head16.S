// The A2 model shape, weight order and inference equations implemented here are
// derived from NeuralAmpModelerCore, MIT, Copyright (c) 2023 Steven Atkinson.
// See THIRD_PARTY_NOTICES.md.

// A2-Full block entry and head kernels for ESP32-S3 PIE: the Q15 input
// conversion, the rechannel into the int32 stream, and the 16x8 output head.

    .section .iram1, "ax"
    .align 4
    .global s3_a2full_native_head_block
    .type s3_a2full_native_head_block, @function

// Run the complete 16x8 head for a block. Each frame rounds the eight head
// sums to Q15, writes them to both halves of the mirrored history ring, and
// accumulates the sixteen-tap dot product as eight independent QACC lanes.
// Each lane is rounded to a signed 16-bit value on a 2^14 grid, and the lane
// sums are returned per frame. The five constant vectors remain resident
// across frames, while the history cursor and mirrored write row are advanced
// inside one zero-overhead loop.
//
// int32_t s3_a2full_native_head_block(const S3NativeHeadBlockArgs* args);
s3_a2full_native_head_block:
    entry   a1, 48
    mov.n   a15, a2
    l32i    a3, a15, 4             // history base
    l32i    a4, a15, 8             // weights
    l32i    a5, a15, 12            // constants
    l32i    a6, a15, 16            // lane sums
    l32i    a7, a15, 20            // frame count
    l32i    a8, a15, 24            // ring position
    l32i    a2, a15, 0             // input head sums

    // Keep quantizer bounds and dot-round operands in vector registers for
    // the whole block instead of reloading them every frame.
    ee.vld.128.ip q4, a5, 16        // 16384 int32 rounding half
    ee.vld.128.ip q5, a5, 16        // INT16_MIN as int32
    ee.vld.128.ip q6, a5, 16        // INT16_MAX as int32
    ee.vld.128.ip q7, a5, 16        // eight signed-16 1024 values
    ee.vld.128.ip q3, a5, 16        // eight signed-16 8 values

    beqz    a7, .Lhead_block_done
    loopnez a7, .Lhead_block_loop_done
    ee.vld.128.ip q0, a2, 16
    ee.vld.128.ip q1, a2, 16

    // Symmetric Q15 rounding. Reuse q2 for each half so q3/q7 stay live.
    ssai    31
    ee.vsr.32 q2, q0
    ee.vadds.s32 q0, q0, q4
    ee.vadds.s32 q0, q0, q2
    ssai    15
    ee.vsr.32 q0, q0
    ssai    31
    ee.vsr.32 q2, q1
    ee.vadds.s32 q1, q1, q4
    ee.vadds.s32 q1, q1, q2
    ssai    15
    ee.vsr.32 q1, q1
    ee.vmax.s32 q0, q0, q5
    ee.vmax.s32 q1, q1, q5
    ee.vmin.s32 q0, q0, q6
    ee.vmin.s32 q1, q1, q6
    ee.vunzip.16 q0, q1

    // Write both halves of the mirrored 16-row history ring.
    slli    a9, a8, 4
    add     a9, a3, a9
    addi    a10, a9, 256
    ee.vst.128.ip q0, a9, 16
    ee.vst.128.ip q0, a10, 16

    // The oldest-to-newest 16-row window begins one row after the write row.
    addi.n  a11, a8, 1
    slli    a11, a11, 4
    add     a11, a3, a11
    mov.n   a12, a4
    ee.zero.qacc
    // Software-pipeline the two vector streams through q0/q1/q2.  LX7 is
    // in-order: a load/load/MAC sequence stalls on both freshly loaded
    // operands at every tap.  Each MAC below keeps the tap order while the
    // following history row is already resident and the current weight load
    // has one independent vector load to mature behind.
    ee.vld.128.ip q0, a11, 16
    ee.vld.128.ip q1, a12, 16
    ee.vld.128.ip q2, a11, 16
    ee.vmulas.s16.qacc q0, q1
    ee.vld.128.ip q0, a12, 16
    ee.vld.128.ip q1, a11, 16
    ee.vmulas.s16.qacc q2, q0
    ee.vld.128.ip q2, a12, 16
    ee.vld.128.ip q0, a11, 16
    ee.vmulas.s16.qacc q1, q2
    .rept 4
    ee.vld.128.ip q1, a12, 16
    ee.vld.128.ip q2, a11, 16
    ee.vmulas.s16.qacc q0, q1
    ee.vld.128.ip q0, a12, 16
    ee.vld.128.ip q1, a11, 16
    ee.vmulas.s16.qacc q2, q0
    ee.vld.128.ip q2, a12, 16
    ee.vld.128.ip q0, a11, 16
    ee.vmulas.s16.qacc q1, q2
    .endr
    ee.vld.128.ip q1, a12, 16
    ee.vmulas.s16.qacc q0, q1
    ee.vmulas.s16.qacc q7, q3
    movi.n  a9, 14
    ee.srcmb.s16.qacc q2, a9, 0
    addi    a9, a1, 16
    ee.vst.128.ip q2, a9, 16

    l16si   a9, a1, 16
    l16si   a10, a1, 18
    add.n   a9, a9, a10
    l16si   a10, a1, 20
    add.n   a9, a9, a10
    l16si   a10, a1, 22
    add.n   a9, a9, a10
    l16si   a10, a1, 24
    add.n   a9, a9, a10
    l16si   a10, a1, 26
    add.n   a9, a9, a10
    l16si   a10, a1, 28
    add.n   a9, a9, a10
    l16si   a10, a1, 30
    add.n   a9, a9, a10
    s32i    a9, a6, 0
    addi.n  a6, a6, 4
    addi.n  a8, a8, 1
    extui   a8, a8, 0, 4
.Lhead_block_loop_done:
.Lhead_block_done:
    mov.n   a2, a8
    retw

    .size s3_a2full_native_head_block, .-s3_a2full_native_head_block

    .align 4
    .global s3_a2full_native_input_q15
    .type s3_a2full_native_input_q15, @function

// uint32_t s3_a2full_native_input_q15(const float* input, int16_t* output,
//                                     int32_t frames);
// Convert a whole block with the LX7 FPU's nearest-integer instruction and
// return the maximum absolute Q15 magnitude in a2.
s3_a2full_native_input_q15:
    entry   a1, 32
    movi    a5, 0x47000000          // 32768.0f
    wfr     f1, a5
    movi    a6, -32768
    movi    a7, 32767
    movi.n  a5, 0
    beqz    a4, .Linput_q15_done
    loopnez a4, .Linput_q15_loop_done
    lsi     f0, a2, 0
    mul.s   f0, f0, f1
    round.s a8, f0, 0
    max     a8, a8, a6
    min     a8, a8, a7
    s16i    a8, a3, 0
    abs     a9, a8
    maxu    a5, a5, a9
    addi.n  a2, a2, 4
    addi.n  a3, a3, 2
.Linput_q15_loop_done:
.Linput_q15_done:
    mov.n   a2, a5
    retw

    .size s3_a2full_native_input_q15, .-s3_a2full_native_input_q15

    .align 4
    .global s3_a2full_native_rechannel8
    .type s3_a2full_native_rechannel8, @function

    .macro RECHANNEL_LANE lane
    l16si   a9, a3, (\lane * 2)
    mul16s  a9, a9, a8
    bgez    a9, .Lrechannel_positive\@
    sub     a9, a10, a9
    sra     a9, a9
    neg     a9, a9
    j       .Lrechannel_store\@
.Lrechannel_positive\@:
    add     a9, a9, a10
    sra     a9, a9
.Lrechannel_store\@:
    s32i    a9, a5, (\lane * 4)
    .endm

// void s3_a2full_native_rechannel8(const int16_t* input,
//     const int16_t* weights, int32_t shift, int32_t* stream,
//     int32_t* head_sum, int32_t frames);
s3_a2full_native_rechannel8:
    entry   a1, 32
    ee.xorq q0, q0, q0
    beqz    a7, .Lrechannel_done
    beqz    a4, .Lrechannel_raw
    addi.n  a11, a4, -1
    movi.n  a10, 1
    ssl     a11
    sll     a10, a10
    // SSL and SSR share the SAR.  Install the right-shift count only after
    // constructing the rounding half above.
    ssr     a4
    loopnez a7, .Lrechannel_loop_done
    l16si   a8, a2, 0
    RECHANNEL_LANE 0
    RECHANNEL_LANE 1
    RECHANNEL_LANE 2
    RECHANNEL_LANE 3
    RECHANNEL_LANE 4
    RECHANNEL_LANE 5
    RECHANNEL_LANE 6
    RECHANNEL_LANE 7
    ee.vst.128.ip q0, a6, 16
    ee.vst.128.ip q0, a6, 16
    addi.n  a2, a2, 2
    addi    a5, a5, 32
.Lrechannel_loop_done:
    retw

.Lrechannel_raw:
    loopnez a7, .Lrechannel_raw_done
    l16si   a8, a2, 0
    l16si   a9, a3, 0
    mul16s  a9, a9, a8
    s32i    a9, a5, 0
    l16si   a9, a3, 2
    mul16s  a9, a9, a8
    s32i    a9, a5, 4
    l16si   a9, a3, 4
    mul16s  a9, a9, a8
    s32i    a9, a5, 8
    l16si   a9, a3, 6
    mul16s  a9, a9, a8
    s32i    a9, a5, 12
    l16si   a9, a3, 8
    mul16s  a9, a9, a8
    s32i    a9, a5, 16
    l16si   a9, a3, 10
    mul16s  a9, a9, a8
    s32i    a9, a5, 20
    l16si   a9, a3, 12
    mul16s  a9, a9, a8
    s32i    a9, a5, 24
    l16si   a9, a3, 14
    mul16s  a9, a9, a8
    s32i    a9, a5, 28
    ee.vst.128.ip q0, a6, 16
    ee.vst.128.ip q0, a6, 16
    addi.n  a2, a2, 2
    addi    a5, a5, 32
.Lrechannel_raw_done:
.Lrechannel_done:
    retw

    .size s3_a2full_native_rechannel8, .-s3_a2full_native_rechannel8
