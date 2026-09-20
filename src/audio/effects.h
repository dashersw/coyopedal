#ifndef COYOPEDAL_PEDAL_EFFECTS_H_
#define COYOPEDAL_PEDAL_EFFECTS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The pedal's effects chain around the NAM profile.
//
//   gate -> compressor -> modulation -> overdrive -> NAM profile
//        -> delay -> reverb
//
// Order is not adjustable and the reasons are audible rather than arbitrary. The
// gate is first because the profile has enormous gain: a guitar idling at -80
// dBFS leaves the amp at -19 dBFS, so anything downstream is chasing noise the
// amp has already amplified. The compressor follows the gate rather than leading
// it, because raising quiet passages is the compressor's whole job and doing that
// before the gate hands the gate a moving target. The reverb is last so it hears
// the amp rather than being distorted by it.
//
// The chorus sits in front of the amp, so it is driven into the profile's
// distortion, as a pedal on the floor would be.
//
// There is no cabinet block: the shipped profiles are full-rig captures with the
// speaker already in them.
//
// Parameters are integers, like the panel's existing knobs, so nothing here
// needs float formatting or invites -Wfloat-equal. Each parameter carries its own
// unit and step; the UI is expected to walk this table rather than hard-code
// ranges, so adding a control needs no UI change.

// The order here is the order blocks were added, not the order they run in: the
// modulation block sits after the overdrive and the reverb in this enum but not in the
// signal path, because these values reach the presets stored in flash and renumbering
// them would silently reinterpret every stored preset. coyopedal_fx_process_* is where
// the chain order actually lives.
typedef enum {
    COYOPEDAL_FX_GATE = 0,
    COYOPEDAL_FX_COMPRESSOR,
    COYOPEDAL_FX_OVERDRIVE,
    COYOPEDAL_FX_REVERB,
    COYOPEDAL_FX_MODULATION,
    COYOPEDAL_FX_DELAY,
    COYOPEDAL_FX_BLOCK_COUNT,
} coyopedal_fx_block_t;

typedef enum {
    // Tenths of a decibel: -185 displays as "-18.5 dB".
    COYOPEDAL_FX_UNIT_DECIBEL_TENTHS = 0,
    // Whole milliseconds. Display as seconds once past 1000.
    COYOPEDAL_FX_UNIT_MILLISECONDS,
    // Whole percent.
    COYOPEDAL_FX_UNIT_PERCENT,
    // Tenths of a compression ratio: 40 displays as "4.0:1".
    COYOPEDAL_FX_UNIT_RATIO_TENTHS,
    // Whole hertz. Display as kHz once past 1000.
    COYOPEDAL_FX_UNIT_HERTZ,
    // Tenths of a hertz: 35 displays as "3.5 Hz". Modulation rates need it - the
    // useful range for a chorus starts below 1 Hz, where whole hertz has no steps
    // left.
    COYOPEDAL_FX_UNIT_HERTZ_TENTHS,
    // Reverb routing. Zero is stereo, so a preset image whose fifth reverb
    // parameter is zero-filled plays in stereo.
    COYOPEDAL_FX_UNIT_OUTPUT_MODE,
} coyopedal_fx_unit_t;

typedef struct {
    const char* label; // <= 7 characters, to fit the knob row
    coyopedal_fx_unit_t unit;
    int16_t minimum;
    int16_t maximum;
    int16_t step;    // one increment of the knob
    int16_t initial; // value at power-on
    int16_t neutral; // value a knob reset returns to
} coyopedal_fx_param_info_t;

// Resets every block to its initial values and clears all delay lines. Not
// real-time safe: it zeroes the reverb's buffers. Call before audio starts.
void coyopedal_fx_init(void);
// Caller owns the attached memory and must stop DSP before detaching it.
void coyopedal_fx_detach(void);

// The reverb's core state is target-owned, so it can be allocated after the model
// has claimed its latency-critical internal RAM. Attach it before coyopedal_fx_init.
size_t coyopedal_fx_reverb_state_size(void);
bool coyopedal_fx_reverb_attach(void* memory, size_t bytes);

// The reverb's four-kilobyte predelay ring is target-owned separately from its
// core state, so the core -- the scalars, cursors and filter state that every
// tank tick sweeps -- can be placed in fast memory on its own. The ring is read
// and written sequentially, so it is the part that belongs in PSRAM. Attach it
// after coyopedal_fx_reverb_attach and before coyopedal_fx_init.
size_t coyopedal_fx_reverb_predelay_state_size(void);
bool coyopedal_fx_reverb_predelay_attach(void* memory, size_t bytes);

// The reverb tank exposes each delay line as a separate target-owned region.
// Firmware may pack those regions into one allocation or place them in different
// memory classes.
size_t coyopedal_fx_reverb_line_count(void);
size_t coyopedal_fx_reverb_line_state_size(size_t line);
bool coyopedal_fx_reverb_line_attach(size_t line, void* memory, size_t bytes);

// Moves the modulation delay line to faster caller-owned memory while audio is
// paused. The line contents are copied and processing continues from the new
// storage.
size_t coyopedal_fx_modulation_line_state_size(void);
bool coyopedal_fx_modulation_line_migrate(void* memory, size_t bytes);

// The delay block's memory, which it does not own.
//
// A delay long enough to be musical does not fit in internal RAM: two seconds of mono
// at 48 kHz is 384 KB, and internal DRAM has well under half that left once the model,
// the reverb tanks and the framebuffer are in it. So the block is handed a buffer, and
// on the pedal that buffer is in the 8 MB of PSRAM - which is the ideal load for it,
// one sequential read and one sequential write per sample, the opposite of the model's
// scattered dilation history.
//
// It is passed in rather than allocated here because this file is also built for the
// host, where there is no PSRAM and a test wants ordinary memory. Nothing is copied and
// nothing is freed: the caller owns the buffer and must outlive the audio path.
//
// Returns false if the buffer is too small to be worth having, in which case the block
// reports itself unavailable and is skipped rather than running with a truncated line
// that would quietly ignore the TIME control.
bool coyopedal_fx_delay_attach(float* memory, size_t samples);

// Whether a buffer has been attached. A front end should show the block as absent
// rather than off: switched off is a choice, and this is not one.
bool coyopedal_fx_delay_ready(void);

// Blocks are individually switchable. A disabled block is skipped entirely
// rather than run at a null setting, so it costs nothing.
void coyopedal_fx_set_enabled(coyopedal_fx_block_t block, bool enabled);
bool coyopedal_fx_enabled(coyopedal_fx_block_t block);
bool coyopedal_fx_any_enabled(void);

// Every block is one pedal: a hard gate, a VCA compressor, a Klon-style drive, a
// spring-voiced reverb, a chorus and a digital delay.
// Its name, as the panel shows it.
const char* coyopedal_fx_name(coyopedal_fx_block_t block);

// The block's parameters.
uint8_t coyopedal_fx_param_count(coyopedal_fx_block_t block);
const coyopedal_fx_param_info_t* coyopedal_fx_param_info(coyopedal_fx_block_t block, uint8_t index);

// Current value, and setting it. Out-of-range values are clamped, so the UI does
// not have to. Setting a parameter is real-time safe and allocation-free: it
// recomputes coefficients, and no block reallocates or clears its state, so
// turning a knob never clicks or drops the reverb tail.
int16_t coyopedal_fx_param(coyopedal_fx_block_t block, uint8_t index);
void coyopedal_fx_set_param(coyopedal_fx_block_t block, uint8_t index, int16_t value);

// True while the gate is holding the signal down, for a panel indicator. The
// pedal looks broken when a gate closes and nothing says so.
bool coyopedal_fx_gate_closed(void);

// Gain the compressor is currently applying, in tenths of a decibel and always
// negative or zero, for a reduction meter.
int16_t coyopedal_fx_compressor_reduction(void);

// Runs the blocks that sit before the profile, in place, mono.
void coyopedal_fx_process_pre(float* samples, size_t frame_count);

// The blocks after the profile, in the order a rig's effects loop runs them: the
// delay, in place and mono, then the reverb, which takes that centered signal in
// `left` and writes the stereo pair to `left` and `right`. They are separate calls
// so the target can schedule them on different cores.
void coyopedal_fx_process_delay(float* samples, size_t frame_count);
void coyopedal_fx_process_reverb_stereo(float* left, float* right, size_t frame_count);

// Cycle attribution for the reverb on the device. The counters separate the
// 48->24 kHz input diffuser from the recursive tank; the effect profile reads them.
// They read zero on the host.
void coyopedal_fx_reverb_profile_reset(void);
void coyopedal_fx_reverb_profile_read(uint32_t* input_cycles, uint32_t* tank_cycles,
                                      uint32_t* blocks);

#ifdef __cplusplus
}
#endif

#endif // COYOPEDAL_PEDAL_EFFECTS_H_
