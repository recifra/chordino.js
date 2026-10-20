#ifndef CHORDINO_RHYTHM_H
#define CHORDINO_RHYTHM_H

#include "chordino.h"
#include "onset.h"
#include "vector.h"

#ifdef __cplusplus
extern "C" {
#endif

// The beats of a whole recording, with its tempo and time signature, from the
// onset strength (onset.c) of its audio and the chords Chordino read in it.
// The tempo is the period the onset strength repeats at most, weighed towards
// a moderate one (Ellis's tempo estimate); the beats are the dynamic
// programming of Ellis's beat tracker over it; and the bars are where the
// chords change and the bass comes in, so many beats apart.
typedef struct {
    double time;            // seconds
    int position;           // in its bar, from 1, the downbeat
} rhythm_beat_t;

typedef struct {
    // Beats a minute, of the note the time signature counts: the eighth in 6/8.
    // 0 when no beat was found, with no beats then
    double bpm;
    int numerator;
    int denominator;
    int beatCount;
    const rhythm_beat_t *beats;
} rhythm_result_t;

typedef struct {
    onset_t *onset;
    double start;           // seconds, of the first sample fed
    bool fed;
    vector_t beats;         // rhythm_beat_t
    rhythm_result_t result;
} rhythm_t;

rhythm_t *rhythm_create(float sampleRate);
void rhythm_destroy(rhythm_t *rhythm);
// Forgets the audio fed so far
void rhythm_reset(rhythm_t *rhythm);
// Feeds count more samples, the first of them at timestamp
void rhythm_process(rhythm_t *rhythm, const float *samples, int count, realtime_t timestamp);
// The beats of everything fed, for the end of the audio, with the chords of
// the same audio (null for none). Valid until the next call.
const rhythm_result_t *rhythm_decode(rhythm_t *rhythm, const chordino_result_t *chords);

#ifdef __cplusplus
}
#endif

#endif
