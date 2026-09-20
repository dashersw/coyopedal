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
s3_a2full_quantize8_a22_s7_s8:
    entry   a1, 32
    ssr     a5
    beqz    a6, .La22_s7_done
    loopnez a6, .La22_s7_loop_done
    l32i    a7,  a2, 0
    l32i    a8,  a2, 4
    l32i    a9,  a2, 8
    l32i    a10, a2, 12
    l32i    a11, a2, 16
    l32i    a12, a2, 20
    l32i    a13, a2, 24
    l32i    a14, a2, 28
    sra     a7,  a7
    sra     a8,  a8
    sra     a9,  a9
    sra     a10, a10
    sra     a11, a11
    sra     a12, a12
    sra     a13, a13
    sra     a14, a14

    movi    a15, -2097151
    max     a7,  a7,  a15
    max     a8,  a8,  a15
    max     a9,  a9,  a15
    max     a10, a10, a15
    max     a11, a11, a15
    max     a12, a12, a15
    max     a13, a13, a15
    max     a14, a14, a15
    movi    a15, 2097151
    min     a7,  a7,  a15
    min     a8,  a8,  a15
    min     a9,  a9,  a15
    min     a10, a10, a15
    min     a11, a11, a15
    min     a12, a12, a15
    min     a13, a13, a15
    min     a14, a14, a15

    extui   a15, a7, 0, 7
    s8i     a15, a4, 0
    srai    a7, a7, 7
    s16i    a7, a3, 0
    extui   a15, a8, 0, 7
    s8i     a15, a4, 1
    srai    a8, a8, 7
    s16i    a8, a3, 2
    extui   a15, a9, 0, 7
    s8i     a15, a4, 2
    srai    a9, a9, 7
    s16i    a9, a3, 4
    extui   a15, a10, 0, 7
    s8i     a15, a4, 3
    srai    a10, a10, 7
    s16i    a10, a3, 6
    extui   a15, a11, 0, 7
    s8i     a15, a4, 4
    srai    a11, a11, 7
    s16i    a11, a3, 8
    extui   a15, a12, 0, 7
    s8i     a15, a4, 5
    srai    a12, a12, 7
    s16i    a12, a3, 10
    extui   a15, a13, 0, 7
    s8i     a15, a4, 6
    srai    a13, a13, 7
    s16i    a13, a3, 12
    extui   a15, a14, 0, 7
    s8i     a15, a4, 7
    srai    a14, a14, 7
    s16i    a14, a3, 14

    addi    a2, a2, 32
    addi    a3, a3, 16
    addi    a4, a4, 8
    ssr     a5
.La22_s7_loop_done:
.La22_s7_done:
    retw

    .size s3_a2full_quantize8_a22_s7_s8, .-s3_a2full_quantize8_a22_s7_s8
