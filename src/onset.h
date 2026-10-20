#ifndef CHORDINO_ONSET_H
#define CHORDINO_ONSET_H

#include <stdbool.h>

#include "spectrum.h"
#include "vector.h"

#ifdef __cplusplus
extern "C" {
#endif

// How much new sound starts at each moment of a recording, the onset strength
// a beat tracker follows: the spectral flux of madmom's SuperFlux, frame by
// frame. Each frame is the spectrum (spectrum.c) of a short window, summed into
// log-spaced bands and compressed; its strength is how far the bands rose above
// the frame before, each against the loudest of its neighbours there, so a
// vibrato that only moves between bands adds nothing. Frame i is centred on
// sample i * hop, the first one on the first sample fed.
typedef struct {
    double period;          // seconds between frames
    int hop;                // samples between frames
    int size;               // of the window
    spectrum_t *spectrum;
    int bandCount;
    int bassBands;          // the first ones, under ONSET_BASS_HZ
    int *firstBin;          // of each band
    int *firstWeight;       // of each band in weights, and one past the last
    float *weights;         // of the bins of each band, from its first one on
    float *previous;        // bands of the frame before,
    float *current;         // and of this one
    bool started;           // previous holds a frame
    float *pending;         // samples fed and not yet stepped past
    int pendingCount;
    long fed;               // samples, since the start
    vector_t strength;      // per frame, floats
    vector_t bass;          // per frame, the strength of the bass bands alone
} onset_t;

// Frames a second, about
#define ONSET_RATE 100
// The bass bands, where a kick drum and the bass notes are
#define ONSET_BASS_HZ 200

onset_t *onset_create(float sampleRate);
void onset_destroy(onset_t *onset);
// Forgets every frame
void onset_reset(onset_t *onset);
// Adds the frames that count more samples complete
void onset_process(onset_t *onset, const float *samples, int count);
// Adds the frames centred on the samples still pending, padding them with
// zeros: for the end of the audio
void onset_finish(onset_t *onset);

#ifdef __cplusplus
}
#endif

#endif
