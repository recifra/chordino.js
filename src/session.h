#ifndef CHORDINO_SESSION_H
#define CHORDINO_SESSION_H

#include "chordino.h"
#include "realtime.h"
#include "spectrum.h"

// What initializePlugin hands to JS as the instance: a Chordino and what feeds
// it. It does what the Vamp host SDK's PluginBufferingAdapter and
// PluginInputDomainAdapter did here, for one channel and the one plugin, with
// the same results: audio comes in blocks of any size, Chordino gets the
// spectrum (spectrum.c) of blockSize samples every stepSize samples.
struct Session {
    chordino_t *chordino;
    float sampleRate;
    bool realtime;          // see initializePlugin in chordextract.cpp
    int inputBlockSize;     // samples per sessionProcess
    int stepSize;           // CHORDINO_STEP_SIZE and CHORDINO_BLOCK_SIZE
    int blockSize;
    float *pending;         // audio fed and not yet stepped past
    int pendingCount;
    long frame;             // where pending starts, once something was fed
    bool fed;
    float *shifted;         // not realtime: see analyse in session.cpp
    int analysed;           // blocks analysed since the start or the last reset
    spectrum_t *spectrum;
};

Session *sessionCreate(float sampleRate, int inputBlockSize, bool realtime);
void sessionRelease(Session *session);
// Forgets the audio fed so far
void sessionReset(Session *session);
// Feeds inputBlockSize samples, the first of them at timestamp
void sessionProcess(Session *session, const float *samples, realtime_t timestamp);
// Pads what is still pending with zeros, analyses it, and decodes every frame:
// for the end of the audio
const chordino_result_t *sessionFinish(Session *session);
// Decodes the frames so far and leaves what is pending as it is: for a caller
// that goes on feeding audio
const chordino_result_t *sessionDecode(Session *session);
// Has every decode report its progress, from 0 to 1, to progress; null stops it
void sessionSetProgress(void (*progress)(double done));

#endif
