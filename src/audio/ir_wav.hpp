#pragma once
#include <cstddef>
#include <cstdio>

// Loads the first 1024 samples of a mono/stereo, 48 kHz cabinet WAV.
// PCM 16/24/32 and IEEE float32 are supported; stereo is averaged to mono.
// RIFF chunks are walked rather than assuming a fixed 44-byte header.
bool pedalboard_ir_read_wav(FILE* file, float* taps, unsigned& count, char* error,
                            std::size_t capacity);
