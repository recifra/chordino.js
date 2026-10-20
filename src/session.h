#ifndef CHORDINO_SESSION_H
#define CHORDINO_SESSION_H

#include <stdbool.h>

#include "chordino.h"
#include "realtime.h"
#include "rhythm.h"
#include "spectrum.h"

#ifdef __cplusplus
extern "C" {
#endif

// What initializePlugin hands to JS as the instance: a Chordino and what feeds
// it. It does what the Vamp host SDK's PluginBufferingAdapter and
// PluginInputDomainAdapter did here, for one channel and the one plugin, with
// the same results: audio comes in blocks of any size, Chordino gets the
// spectrum (spectrum.c) of blockSize samples every stepSize samples. Offline,
// the same audio goes to a rhythm_t too, for the beats of the recording.
typedef struct {
    chordino_t *chordino;
    float sampleRate;
    bool realtime;          // see initializePlugin in chordextract.c
    int inputBlockSize;     // samples per session_process
    int stepSize;           // CHORDINO_STEP_SIZE and CHORDINO_BLOCK_SIZE
    int blockSize;
    float *pending;         // audio fed and not yet stepped past
    int pendingCount;
    long frame;             // where pending starts, once something was fed
    bool fed;
    float *shifted;         // not realtime: see analyse in session.c
    int analysed;           // blocks analysed since the start or the last reset
    spectrum_t *spectrum;
    rhythm_t *rhythm;       // null when realtime
    const rhythm_result_t *beats;   // since session_finish, or null
} session_t;

session_t *session_create(float sampleRate, int inputBlockSize, bool realtime);
void session_destroy(session_t *session);
// Forgets the audio fed so far
void session_reset(session_t *session);
// Feeds inputBlockSize samples, the first of them at timestamp
void session_process(session_t *session, const float *samples, realtime_t timestamp);
// Pads what is still pending with zeros, analyses it, and decodes every frame:
// for the end of the audio. Offline, it finds the beats too, into beats.
const chordino_result_t *session_finish(session_t *session);
// Decodes the frames so far and leaves what is pending as it is: for a caller
// that goes on feeding audio
const chordino_result_t *session_decode(session_t *session);
// Has every decode report its progress, from 0 to 1, to progress; null stops it
void session_set_progress(void (*progress)(double done));

#ifdef __cplusplus
}
#endif

#endif
