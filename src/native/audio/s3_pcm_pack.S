// ESP32-S3 stereo float-to-UAC2 PCM packer.
//
// Keeps both samples and all conversion constants in FPU registers. GCC's
// equivalent C++ loop spills the clamped floats and selected scale factors to
// the stack before every conversion.
//
// void coyopedal_s3_pack_pcm_stereo(uint64_t* destination,
//                                const float* left,
//                                const float* right,
//                                size_t frames);

    .section .iram1, "ax"
    .align  4
    .literal_position
    .literal .Lpcm_scale,      0x47000000
    .global coyopedal_s3_pack_pcm_stereo
    .type   coyopedal_s3_pack_pcm_stereo, @function

coyopedal_s3_pack_pcm_stereo:
    entry   a1, 16
    beqz    a5, .Lpcm_done

    l32r    a6, .Lpcm_scale
    wfr     f15, a6
    movi    a11, -32768
    movi    a12, 32767

    srli    a8, a5, 1
    beqz    a8, .Lpcm_tail

    // Convert two frames together. Four independent FPU chains cover the
    // in-order core's compare/multiply latency, and the three source/dest
    // pointer updates are paid once per pair.
    loop    a8, .Lpcm_pairs_done
    lsi     f0, a3, 0
    lsi     f1, a4, 0
    lsi     f4, a3, 4
    lsi     f5, a4, 4

    // Convert with the conventional signed-Q15 scale, then clamp in integer
    // registers, which avoids a float clamp and a sign-selected multiplier per
    // sample. Positive values differ from the mono path's asymmetric scale in
    // usb_audio.cpp by at most one PCM16 LSB.
    mul.s   f0, f0, f15
    mul.s   f1, f1, f15
    mul.s   f4, f4, f15
    mul.s   f5, f5, f15
    trunc.s a6, f0, 0
    trunc.s a7, f1, 0
    trunc.s a9, f4, 0
    trunc.s a10, f5, 0
    max     a6, a6, a11
    max     a7, a7, a11
    max     a9, a9, a11
    max     a10, a10, a11
    min     a6, a6, a12
    min     a7, a7, a12
    min     a9, a9, a12
    min     a10, a10, a12
    slli    a6, a6, 16
    slli    a7, a7, 16
    slli    a9, a9, 16
    slli    a10, a10, 16
    s32i    a6, a2, 0
    s32i    a7, a2, 4
    s32i    a9, a2, 8
    s32i    a10, a2, 12

    addi    a2, a2, 16
    addi    a3, a3, 8
    addi    a4, a4, 8
.Lpcm_pairs_done:
    extui   a5, a5, 0, 1
    beqz    a5, .Lpcm_done

.Lpcm_tail:
    lsi     f0, a3, 0
    lsi     f1, a4, 0
    mul.s   f0, f0, f15
    mul.s   f1, f1, f15
    trunc.s a6, f0, 0
    trunc.s a7, f1, 0
    max     a6, a6, a11
    max     a7, a7, a11
    min     a6, a6, a12
    min     a7, a7, a12
    slli    a6, a6, 16
    slli    a7, a7, 16
    s32i    a6, a2, 0
    s32i    a7, a2, 4
.Lpcm_done:
    retw.n

    .size coyopedal_s3_pack_pcm_stereo, .-coyopedal_s3_pack_pcm_stereo

    .align  4
    .global coyopedal_s3_s32_to_float
    .type   coyopedal_s3_s32_to_float, @function

// Convert signed 32-bit USB capture slots to float. Four independent FPU
// chains hide the LX7 conversion/multiply latency before the PSRAM stores.
//
// void coyopedal_s3_s32_to_float(float* destination,
//                             const int32_t* source,
//                             size_t frames,
//                             float scale);
coyopedal_s3_s32_to_float:
    entry   a1, 16
    beqz    a4, .Ls32_float_done
    wfr     f4, a5
    srli    a6, a4, 2
    beqz    a6, .Ls32_float_tail
    loop    a6, .Ls32_float_groups_done
    l32i    a7, a3, 0
    l32i    a8, a3, 4
    l32i    a9, a3, 8
    l32i    a10, a3, 12
    float.s f0, a7, 0
    float.s f1, a8, 0
    float.s f2, a9, 0
    float.s f3, a10, 0
    mul.s   f0, f0, f4
    mul.s   f1, f1, f4
    mul.s   f2, f2, f4
    mul.s   f3, f3, f4
    ssi     f0, a2, 0
    ssi     f1, a2, 4
    ssi     f2, a2, 8
    ssi     f3, a2, 12
    addi    a3, a3, 16
    addi    a2, a2, 16
.Ls32_float_groups_done:
    extui   a4, a4, 0, 2
    beqz    a4, .Ls32_float_done
.Ls32_float_tail:
    loopnez a4, .Ls32_float_done
    l32i    a7, a3, 0
    addi.n  a3, a3, 4
    float.s f0, a7, 0
    mul.s   f0, f0, f4
    ssi     f0, a2, 0
    addi.n  a2, a2, 4
.Ls32_float_done:
    retw.n

    .size coyopedal_s3_s32_to_float, .-coyopedal_s3_s32_to_float
