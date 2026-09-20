#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// A chromatic tuner over the guitar's range, fed from the dry signal.
//
// Split in two on purpose. Collecting samples is real-time safe and happens on the
// audio path; finding the pitch is a few hundred thousand multiply-accumulates and
// happens on the UI task. A detector that ran inside the audio path would have to
// be budgeted against NAM inference, and there is no reason to pay that: a tuner
// answers a few times a second, not every block.
//
// Engaging the tuner mutes the pedal and suspends the amp block.
//
// Reference is A = 440 Hz, equal temperament. There is deliberately no adjustment:
// a reference control is a preference, and nothing here forecloses adding one.

typedef struct {
    // False when nothing is being played, when the signal is too quiet to trust, or
    // when it has no periodicity - noise, a hand muting the strings, a room. The
    // panel shows a dash rather than parking on whatever the noise floor suggested.
    bool voiced;

    float frequency; // Hz, or 0 when not voiced
    int note;        // 0..11, 0 = C. Name it with coyopedal_tuner_note_name().
    int octave;      // scientific pitch notation, so A440 is octave 4
    float cents;     // -50..+50 against the nearest equal-tempered note

    // Why a reading is not voiced, which is otherwise invisible: `level` is the
    // window's RMS and `clarity` the best periodicity score it found, where lower is
    // more periodic. One of the two gates rejected it and these say which.
    float level;
    float clarity;
} coyopedal_tuner_reading_t;

// Clears the history and the last reading. Call when the tuner is engaged, so it
// does not open on a stale note from the last time.
void coyopedal_tuner_reset(void);

// Collects one block of dry mono samples at 48 kHz. Real-time safe: it decimates
// and stores, and never searches. Safe to call whether or not the tuner is on;
// what costs is coyopedal_tuner_update().
void coyopedal_tuner_feed(const float* samples, size_t frame_count);

// Searches for a pitch if a full window has arrived since the last search, and
// returns true when it produced a new reading. Not real-time safe. Call from the
// UI task, next to the panel.
bool coyopedal_tuner_update(void);

// The most recent reading. Never blocks and never searches.
void coyopedal_tuner_read(coyopedal_tuner_reading_t* out);

// "C", "C#", "D" ... for a note index. Sharps rather than flats, because the
// panel has one name per pitch class and a guitarist reads sharps.
const char* coyopedal_tuner_note_name(int note);

#ifdef __cplusplus
}
#endif
