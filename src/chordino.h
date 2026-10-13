#ifndef CHORDINO_CHORDINO_H
#define CHORDINO_CHORDINO_H

#include "realtime.h"

#ifdef __cplusplus
extern "C" {
#endif

// Matthias Mauch's Chordino (src/nnls-chroma/chordino.c): chords from the
// spectra of a recording, frame by frame. What was a Vamp plugin, with the
// player's interface of descriptors and parameters, is these functions: frames
// go in through chordino_process and chordino_decode reads every output from
// the frames so far, as often as needed.

// Samples whose spectrum (spectrum_compute) makes a frame, and between frames
#define CHORDINO_BLOCK_SIZE 16384
#define CHORDINO_STEP_SIZE 2048

typedef struct chordino chordino_t;

typedef struct {
    const char *label;      // "N" for no chord
    realtime_t time;        // when it starts
} chordino_chord_t;

// A note of a chord, for as long as the chord lasts
typedef struct {
    int midi;
    realtime_t time;
    realtime_t duration;
} chordino_note_t;

typedef struct {
    // Each chord change, and an "N" at the last frame that closes the last one
    int chordCount;
    const chordino_chord_t *chords;
    // The notes of every chord but N, bass first
    int noteCount;
    const chordino_note_t *notes;
    // Per frame: its time, how likely the chord changes there, and the log
    // likelihood of the chord decoded there
    int frameCount;
    const realtime_t *times;
    const float *harmonicChange;
    const float *logLikelihood;
} chordino_result_t;

chordino_t *chordino_create(float sampleRate);
void chordino_destroy(chordino_t *chordino);
// Forgets every frame, and the tuning estimated from them
void chordino_reset(chordino_t *chordino);
// Adds the frame centred on time, from the CHORDINO_BLOCK_SIZE / 2 + 1 bins of
// its spectrum: re, im, re, ...
void chordino_process(chordino_t *chordino, const float *bins, realtime_t time);
// Drops the frames before time, so that each chordino_decode of a caller still
// feeding audio decodes a bounded window. The tuning estimate keeps counting
// every frame since the start or the last reset.
void chordino_forget_before(chordino_t *chordino, realtime_t time);
// Decodes the frames so far, calling progress (if not null) with done rising
// from 0 to 1, as it takes seconds for a whole song. Valid until the next
// chordino_decode, chordino_reset or chordino_destroy.
const chordino_result_t *chordino_decode(chordino_t *chordino, void (*progress)(double done));

#ifdef __cplusplus
}
#endif

#endif
