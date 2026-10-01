/*
 * SPDX-FileCopyrightText: 2018-2025 Espressif Systems (Shanghai) CO LTD
 * SPDX-FileContributor: 2024 f4lc0n @ Libera Chat IRC
 * SPDX-License-Identifier: Apache-2.0
 *
 * Adapted from esp-dsp modules/fft/float/dsps_fft4r_fc32_aes3_.S:
 * fixed 64-point transform, caller-owned 48-complex twiddle table,
 * forward/inverse sign, no library globals or allocations.
 * Data must be 8-byte aligned. Output is base-four digit-reversed.
 */
    .section .iram1.cabinet_fft,"ax",@progbits
    .align 4
    .global pedal_cabinet_fft64
    .type pedal_cabinet_fft64,@function
pedal_cabinet_fft64:
    entry a1, 16
    wfr f15, a4          # sign: +1 forward, -1 inverse
    mov.n a4, a3        # twiddle table (positive sine)
    movi a3, 64
    movi.n a5, 2        # groups per stage, doubled
    movi.n a6, 2        # twiddle stride in floats
    movi.n a7, 3        # radix-four stages
.Lcab_stage:
	srli    a3, a3, 2     # N >>= 2
    beqi a3, 1, .Lcab_unity_stage

	movi.n  a8, 0         # j = 0

.Lcab_group:
	mov.n   a9, a4        # w0 = w
	mov.n   a10, a4       # w1 = w
	mov.n   a11, a4       # w2 = w

	mul16u  a12, a8, a3
	slli    a12, a12, 1   # start_index = (j * N) << 1

	addx8   a12, a12, a2  # p0 = data + (start_index << 1)
	addx8   a13, a3, a12  # p1 = p0 + (N << 1)
	addx8   a14, a3, a13  # p2 = p1 + (N << 1)
	addx8   a15, a3, a14  # p3 = p2 + (N << 1)

	loopnez a3, .Lcab_bfly_end # for (uint k = 0; k < N; k++)
	ee.ldf.64.ip f1, f0, a12, 0 # f0 = in0.re = *p0, f1 = in0.im = *(p0 + 1)
	ee.ldf.64.ip f3, f2, a14, 0 # f2 = in2.re = *p2, f3 = in2.im = *(p2 + 1)
	add.s   f5, f1, f3    #  f5 = in0.im + in2.im
	sub.s   f7, f1, f3    #  f7 = in0.im - in2.im
	add.s   f4, f0, f2    #  f4 = in0.re + in2.re
	sub.s   f6, f0, f2    #  f6 = in0.re - in2.re
	ee.ldf.64.ip f1, f0, a13, 0 # f0 = in1.re = *p1, f1 = in1.im = *(p1 + 1)
	ee.ldf.64.ip f3, f2, a15, 0 # f2 = in3.re = *p3, f3 = in3.im = *(p3 + 1)
	add.s   f9, f1, f3    #  f9 = in1.im + in3.im
	sub.s   f11, f1, f3   # f11 = in1.im - in3.im
	lsi     f12, a9, 0    # f12 = w0->re
	lsi     f13, a10, 0   # f13 = w1->re
	lsi     f14, a11, 0   # f14 = w2->re
	add.s   f8, f0, f2    #  f8 = in1.re + in3.re
	sub.s   f10, f0, f2   # f10 = in1.re - in3.re
	mul.s   f10, f10, f15
	mul.s   f11, f11, f15
	sub.s   f1, f5, f9    #  f1 = bf2.im = in0.im + in2.im - in1.im - in3.im
	add.s   f5, f5, f9    #  f5 = bf0.im = in0.im + in2.im + in1.im + in3.im
	add.s   f2, f6, f11   #  f2 = bf1.re = in0.re - in2.re + in1.im - in3.im
	sub.s   f6, f6, f11   #  f6 = bf3.re = in0.re - in2.re - in1.im + in3.im
	sub.s   f0, f4, f8    #  f0 = bf2.re = in0.re + in2.re - in1.re - in3.re
	add.s   f4, f4, f8    #  f4 = bf0.re = in0.re + in2.re + in1.re + in3.re
	sub.s   f3, f7, f10   #  f3 = bf1.im = in0.im - in2.im - in1.re + in3.re
	add.s   f7, f7, f10   #  f7 = bf3.im = in0.im - in2.im + in1.re - in3.re
	mul.s   f10, f6, f14  # f10 = bf3.re * w2->re
	ee.stf.64.ip f5, f4, a12, 8 # *p0 = f4 = bf0.re, *(p0 + 1) = f5 = bf0.im, p0 += 2
	mul.s   f4, f2, f12   #  f4 = bf1.re * w0->re
	mul.s   f11, f7, f14  # f11 = bf3.im * w2->re
	mul.s   f5, f3, f12   #  f5 = bf1.im * w0->re
	mul.s   f8, f0, f13   #  f8 = bf2.re * w1->re
	mul.s   f9, f1, f13   #  f9 = bf2.im * w1->re
	lsi     f12, a9, 4    # f12 = w0->im
	lsi     f13, a10, 4   # f13 = w1->im
	lsi     f14, a11, 4   # f14 = w2->im
	mul.s   f12, f12, f15
	mul.s   f13, f13, f15
	mul.s   f14, f14, f15
	msub.s  f5, f2, f12   #  f5 = bf1.im * w0->re - bf1.re * w0->im
	madd.s  f4, f3, f12   #  f4 = bf1.re * w0->re + bf1.im * w0->im
	msub.s  f9, f0, f13   #  f9 = bf2.im * w1->re - bf2.re * w1->im
	madd.s  f8, f1, f13   #  f8 = bf2.re * w1->re + bf2.im * w1->im
	msub.s  f11, f6, f14  # f11 = bf3.im * w2->re - bf3.re * w2->im
	madd.s  f10, f7, f14  # f10 = bf3.re * w2->re + bf3.im * w2->im
	addx4   a9, a6, a9    # w0 += w_step
	addx8   a10, a6, a10  # w1 += 2 * w_step
	addx4   a11, a6, a11
	addx8   a11, a6, a11  # w2 += 3 * w_step
	ee.stf.64.ip f5, f4, a13, 8 # *p1 = f4, *(p1 + 1) = f5, p1 += 2
	ee.stf.64.ip f9, f8, a14, 8 # *p2 = f8, *(p2 + 1) = f9, p2 += 2
	ee.stf.64.ip f11, f10, a15, 8 # *p3 = f10, *(p3 + 1) = f11, p3 += 2
.Lcab_bfly_end:

	addi.n  a8, a8, 2     # j += 2
	bgeu    a8, a5, .Lcab_stage_next # if j >= m
	j       .Lcab_group

.Lcab_stage_next:
	slli    a5, a5, 2     # m <<= 2
	slli    a6, a6, 2     # w_step <<= 2
	addi.n  a7, a7, -1    # log4N--
	bnez    a7, .Lcab_stage    # if log4N > 0

    retw.n

// Every butterfly in the final radix-four stage has twiddle (1, 0).
// Skip its six twiddle loads, nine multiplies and six multiply-adds.
.Lcab_unity_stage:
    mov.n a12, a2
    movi a3, 16
    loopnez a3, .Lcab_unity_done
    ee.ldf.64.ip f1, f0, a12, 0
    addi a13, a12, 16
    ee.ldf.64.ip f3, f2, a13, 0
    add.s f5, f1, f3
    sub.s f7, f1, f3
    add.s f4, f0, f2
    sub.s f6, f0, f2
    addi a13, a12, 8
    ee.ldf.64.ip f1, f0, a13, 0
    addi a13, a12, 24
    ee.ldf.64.ip f3, f2, a13, 0
    add.s f9, f1, f3
    sub.s f11, f1, f3
    add.s f8, f0, f2
    sub.s f10, f0, f2
    mul.s f10, f10, f15
    mul.s f11, f11, f15
    sub.s f1, f5, f9
    add.s f5, f5, f9
    add.s f2, f6, f11
    sub.s f6, f6, f11
    sub.s f0, f4, f8
    add.s f4, f4, f8
    sub.s f3, f7, f10
    add.s f7, f7, f10
    ee.stf.64.ip f5, f4, a12, 8
    ee.stf.64.ip f3, f2, a12, 8
    ee.stf.64.ip f1, f0, a12, 8
    ee.stf.64.ip f7, f6, a12, 8
.Lcab_unity_done:
    retw.n
    .size pedal_cabinet_fft64, .-pedal_cabinet_fft64

// Four adjacent bins, oldest-to-newest coefficient order and descending
// input-history rows. All eight accumulators remain live across partitions.
    .macro CABINET_MAC4 coefficients, history
    ee.ldf.64.ip f9, f8, \coefficients, 8
    ee.ldf.64.ip f11, f10, \history, 8
    ee.ldf.64.ip f13, f12, \coefficients, 8
    ee.ldf.64.ip f15, f14, \history, 8
    madd.s f0, f8, f10
    madd.s f2, f12, f14
    madd.s f1, f8, f11
    madd.s f3, f12, f15
    msub.s f0, f9, f11
    msub.s f2, f13, f15
    madd.s f1, f9, f10
    madd.s f3, f13, f14
    ee.ldf.64.ip f9, f8, \coefficients, 8
    ee.ldf.64.ip f11, f10, \history, 8
    ee.ldf.64.ip f13, f12, \coefficients, 8
    ee.ldf.64.ip f15, f14, \history, 8
    madd.s f4, f8, f10
    madd.s f6, f12, f14
    madd.s f5, f8, f11
    madd.s f7, f12, f15
    msub.s f4, f9, f11
    msub.s f6, f13, f15
    madd.s f5, f9, f10
    madd.s f7, f13, f14
    addi \history, \history, -64
    .endm

// coefficients/history/cursor/real/imaginary are a2..a6. The 16 rows
// contain four bins each, with strides of 16 and 17 complex-bin groups.
    .align 4
    .global pedal_cabinet_sum64
    .type pedal_cabinet_sum64,@function
pedal_cabinet_sum64:
    entry a1, 16
    movi a7, 16
.Lcab_sum_group:
    movi.n a8, 0
    wfr f0, a8
    wfr f1, a8
    wfr f2, a8
    wfr f3, a8
    wfr f4, a8
    wfr f5, a8
    wfr f6, a8
    wfr f7, a8
    mov.n a9, a2
    slli a10, a4, 5
    add a10, a3, a10
    addi.n a8, a4, 1
    loopnez a8, .Lcab_sum_first_done
    CABINET_MAC4 a9, a10
.Lcab_sum_first_done:
    movi.n a8, 15
    sub a8, a8, a4
    beqz a8, .Lcab_sum_store
    movi a10, 480
    add a10, a3, a10
    loopnez a8, .Lcab_sum_second_done
    CABINET_MAC4 a9, a10
.Lcab_sum_second_done:
.Lcab_sum_store:
    ssi f0, a5, 0
    ssi f2, a5, 4
    ssi f4, a5, 8
    ssi f6, a5, 12
    ssi f1, a6, 0
    ssi f3, a6, 4
    ssi f5, a6, 8
    ssi f7, a6, 12
    addi a5, a5, 16
    addi a6, a6, 16
    addmi a2, a2, 512
    addmi a3, a3, 512
    addi a3, a3, 32
    addi.n a7, a7, -1
    bnez a7, .Lcab_sum_group
    retw.n
    .size pedal_cabinet_sum64, .-pedal_cabinet_sum64
