#ifndef CHORDINO_SESSION_H
#define CHORDINO_SESSION_H

#include "nnls-chroma/Chordino.h"
#include "kissfft/kiss_fftr.h"
#include "realtime.h"

// What initializePlugin hands to JS as the instance: a Chordino and what feeds
// it. It does what the Vamp host SDK's PluginBufferingAdapter and
// PluginInputDomainAdapter did here, for one channel and the one plugin, with
// the same results: audio comes in blocks of any size, Chordino gets Hann
// windowed spectra of blockSize samples every stepSize samples.
struct Session {
    Chordino *chordino;
    float sampleRate;
    bool realtime;          // see initializePlugin in chordextract.cpp
    int inputBlockSize;     // samples per sessionProcess
    int stepSize;           // Chordino's preferred ones
    int blockSize;
    float *pending;         // audio fed and not yet stepped past
    int pendingCount;
    long frame;             // where pending starts, once something was fed
    bool fed;
    float *shifted;         // not realtime: see analyse in session.cpp
    int analysed;           // blocks analysed since the start or the last reset
    double *window;
    double *windowed;
    kiss_fftr_cfg fft;
    kiss_fft_cpx *spectrum;
    float *bins;            // the spectrum as Chordino takes it: re, im, re, ...
};

// Null when Chordino does not initialise
Session *sessionCreate(float sampleRate, int inputBlockSize, bool realtime);
void sessionRelease(Session *session);
// Forgets the audio fed so far
void sessionReset(Session *session);
// Feeds inputBlockSize samples, the first of them at timestamp
void sessionProcess(Session *session, const float *samples, realtime_t timestamp);
// Pads what is still pending with zeros, analyses it, and returns Chordino's
// features: for the end of the audio
Vamp::Plugin::FeatureSet sessionFinish(Session *session);

#endif
