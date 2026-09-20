#ifndef COYOPEDAL_PEDAL_CONTROL_PROTOCOL_H_
#define COYOPEDAL_PEDAL_CONTROL_PROTOCOL_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The fixed-size snapshot a front end reads the pedal's state through
// (src/native/ui/control.h). It is packed and carries
// COYOPEDAL_CONTROL_PROTOCOL_VERSION.

#define COYOPEDAL_CONTROL_PROTOCOL_VERSION 2u

// The largest number of parameters any one block offers. It is a ceiling the
// format is sized for rather than a count: a block reports its own param_count
// and the entries past it are zero.
#define COYOPEDAL_CONTROL_MAX_PARAMS 5u

// The amp is addressed as one past the last effects block. It is not one of the
// engine's blocks - it is the profile plus its gains - but every front end
// wants to reach it the same way it reaches the others, and giving it a number
// here is cheaper than a parallel set of calls.
#define COYOPEDAL_CONTROL_AMP_BLOCK 6u
#define COYOPEDAL_CONTROL_BLOCK_SLOTS 7u

// Input, drive, level, bass, mid, treble, in tenths of a decibel. Six rather than
// three because the amp block carries the tone stack, and a preset that did not
// would leave the previous preset's tone under a new profile.
#define COYOPEDAL_CONTROL_AMP_GAINS 6u

#if defined(__GNUC__) || defined(__clang__)
#define COYOPEDAL_CONTROL_PACKED __attribute__((packed))
#else
#define COYOPEDAL_CONTROL_PACKED
#endif

typedef struct COYOPEDAL_CONTROL_PACKED {
    uint8_t present; // whether the slot holds a pedal at all
    uint8_t enabled; // whether that pedal is switched on
    uint8_t param_count;
    int16_t param[COYOPEDAL_CONTROL_MAX_PARAMS];
} coyopedal_control_block_t;

typedef struct COYOPEDAL_CONTROL_PACKED {
    uint32_t revision;
    uint16_t protocol_version;
    uint8_t screen; // 0 home, 1 chain, 2 editor, 3 tuner
    uint8_t engaged;
    uint8_t tuner;
    uint16_t preset; // index into the preset list, or 0xFFFF when none has been recalled
    uint16_t model;  // index into the library, or 0xFFFF when unknown
    int16_t amp[COYOPEDAL_CONTROL_AMP_GAINS];
    uint8_t amp_present;
    uint8_t usb; // coyopedal_ui_usb_t: unmounted, mounted, streaming, suspended
    coyopedal_control_block_t block[COYOPEDAL_CONTROL_BLOCK_SLOTS];

    // Live readings, so a front end's meters come from the same detectors the
    // panel's do rather than from a second implementation on the host.
    uint8_t gate_closed;
    uint8_t tuner_voiced;
    int16_t compressor_reduction; // tenths of a dB, always <= 0
    int16_t tuner_cents;          // -50..+50
    int16_t tuner_note;           // 0..11, 0 = C, or -1
    int16_t tuner_octave;
    uint32_t tuner_millihertz;
} coyopedal_control_state_t;

#ifdef __cplusplus
}
#endif

#endif // COYOPEDAL_PEDAL_CONTROL_PROTOCOL_H_
