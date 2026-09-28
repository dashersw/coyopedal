// The A2 model shape, weight order and inference equations implemented here are
// derived from NeuralAmpModelerCore, MIT, Copyright (c) 2023 Steven Atkinson.
// See THIRD_PARTY_NOTICES.md.

// ESP32-S3 A2-Full int32 stream -> history ring quantizers.
//
// One call handles a contiguous run of complete eight-channel frames. The
// narrow form writes a high-only ring: stream >> right_shift, keeping the low
// 16 bits. The A22 form writes a split ring:
//
//   q = clamp(stream >> right_shift, -2097151, 2097151)
//   high = q >> 7
//   low = q & 127

    .section .iram1, "ax"
    .align  4

    .global s3_a2full_quantize8_narrow
    .type   s3_a2full_quantize8_narrow, @function

// void s3_a2full_quantize8_narrow(const int32_t* input, int16_t* output,
//                                  int right_shift, int frames)
s3_a2full_quantize8_narrow:
    entry   a1, 32
    ssr     a4
    beqz    a5, .Lnarrow_done
    loopnez a5, .Lnarrow_loop_done
    l32i    a6,  a2, 0
    l32i    a7,  a2, 4
    l32i    a8,  a2, 8
    l32i    a9,  a2, 12
    l32i    a10, a2, 16
    l32i    a11, a2, 20
    l32i    a12, a2, 24
    l32i    a13, a2, 28
    sra     a6,  a6
    sra     a7,  a7
    sra     a8,  a8
    sra     a9,  a9
    sra     a10, a10
    sra     a11, a11
    sra     a12, a12
    sra     a13, a13
    s16i    a6,  a3, 0
    s16i    a7,  a3, 2
    s16i    a8,  a3, 4
    s16i    a9,  a3, 6
    s16i    a10, a3, 8
    s16i    a11, a3, 10
    s16i    a12, a3, 12
    s16i    a13, a3, 14
    addi    a2, a2, 32
    addi    a3, a3, 16
.Lnarrow_loop_done:
.Lnarrow_done:
    retw

    .size s3_a2full_quantize8_narrow, .-s3_a2full_quantize8_narrow

    .align  4
    .global s3_a2full_quantize8_a22_s7_s8
    .type   s3_a2full_quantize8_a22_s7_s8, @function

// void s3_a2full_quantize8_a22_s7_s8(const int32_t* input, int16_t* high,
//                                     int8_t* low, int right_shift,
//                                     int frames)
//
// S3-native A2-Full ring format: q = high*128 + low, low in 0..127.
// Each frame is eight contiguous int32 lanes. Shift and clamp before packing
// to preserve scalar SRA/MAX/MIN semantics at both A22 endpoints. All input
// and high-ring rows are 16-byte aligned; low-ring rows are eight-byte aligned.
s3_a2full_quantize8_a22_s7_s8:
    entry   a1, 48
    beqz    a6, .La22_s7_done
    movi    a8, -2097151
    s32i    a8, a1, 16
    movi    a8, 2097151
    s32i    a8, a1, 20
    movi    a8, 127
    s32i    a8, a1, 24
    addi    a7, a1, 16
    ee.vldbc.32.ip q5, a7, 4
    ee.vldbc.32.ip q6, a7, 4
    ee.vldbc.8.ip q7, a7, 0
    loopnez a6, .La22_s7_loop_done
    ee.vld.128.ip q0, a2, 16
    ee.vld.128.ip q1, a2, 16
    ssr     a5
    ee.vsr.32 q0, q0
    ee.vsr.32 q1, q1
    ee.vmax.s32 q0, q0, q5
    ee.vmax.s32 q1, q1, q5
    ee.vmin.s32 q0, q0, q6
    ee.vmin.s32 q1, q1, q6
    ssai    7
    ee.vsr.32 q2, q0
    ee.vsr.32 q3, q1
    ee.vunzip.16 q2, q3
    ee.vst.128.ip q2, a3, 16
    ee.vunzip.16 q0, q1
    ee.vunzip.8 q0, q1
    ee.andq q0, q0, q7
    ee.vst.l.64.ip q0, a4, 8
.La22_s7_loop_done:
.La22_s7_done:
    retw

    .size s3_a2full_quantize8_a22_s7_s8, .-s3_a2full_quantize8_a22_s7_s8
