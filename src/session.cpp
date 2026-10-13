#include "session.h"

#include <cmath>
#include <cstring>

Session *sessionCreate(float sampleRate, int inputBlockSize, bool realtime) {
    auto *chordino = new Chordino(sampleRate);
    int stepSize = int(chordino->getPreferredStepSize());
    int blockSize = int(chordino->getPreferredBlockSize());
    if (!chordino->initialise(1, stepSize, blockSize)) {
        delete chordino;
        return nullptr;
    }
    // Numbers the outputs (m_outputChords and the like), which
    // getRemainingFeatures files its features under
    (void) chordino->getOutputDescriptors();

    auto *session = new Session();
    session->chordino = chordino;
    session->sampleRate = sampleRate;
    session->realtime = realtime;
    session->inputBlockSize = inputBlockSize;
    session->stepSize = stepSize;
    session->blockSize = blockSize;
    session->pending = new float[blockSize + inputBlockSize];
    session->shifted = new float[blockSize + blockSize / 2];
    session->window = window_create_hann(blockSize);
    session->windowed = new double[blockSize];
    session->fft = kiss_fftr_alloc(blockSize, 0, nullptr, nullptr);
    session->spectrum = new kiss_fft_cpx[blockSize / 2 + 1];
    session->bins = new float[blockSize + 2];
    sessionReset(session);
    return session;
}

void sessionRelease(Session *session) {
    delete session->chordino;
    delete[] session->pending;
    delete[] session->shifted;
    window_destroy(session->window);
    delete[] session->windowed;
    kiss_fftr_free(session->fft);
    delete[] session->spectrum;
    delete[] session->bins;
    delete session;
}

void sessionReset(Session *session) {
    session->pendingCount = 0;
    session->frame = 0;
    session->fed = false;
    session->analysed = 0;
    session->chordino->reset();
}

namespace {

// Frames are timed on the sample rate rounded to a whole number
unsigned int frameRate(const Session *session) {
    return (unsigned int) int(session->sampleRate + 0.5);
}

// Hands Chordino the spectrum of the blockSize samples at block, which start at
// timestamp. Chordino times each frame by the centre of its window, so either
// the timestamp moves forward half a block (realtime), or the audio is delayed
// half a block behind it, starting from silence (offline: the first frame is
// then centred on the first sample, and the chords start at 0).
void analyse(Session *session, const float *block, realtime_t timestamp) {
    int blockSize = session->blockSize;
    int half = blockSize / 2;
    const float *input = block;
    if (session->realtime) {
        timestamp = realtime_add(timestamp, realtime_from_frame(half, frameRate(session)));
        // One nanosecond more when the sum rounded down to just before a frame
        unsigned int roundedRate = session->sampleRate > 0.f ? (unsigned int) round(session->sampleRate) : 1;
        realtime_t later = realtime_add(timestamp, realtime_make(0, 1));
        if (realtime_to_frame(timestamp, roundedRate) < realtime_to_frame(later, roundedRate)) {
            timestamp = later;
        }
    } else {
        // shifted holds half a block from before this one, then this one
        float *shifted = session->shifted;
        if (session->analysed == 0) {
            memset(shifted, 0, (blockSize + half) * sizeof(float));
        }
        memmove(shifted, shifted + session->stepSize, (blockSize + half - session->stepSize) * sizeof(float));
        memcpy(shifted + half, block, blockSize * sizeof(float));
        input = shifted;
    }
    session->analysed++;

    double *windowed = session->windowed;
    window_cut(session->window, input, windowed);
    // Centred on the start of the FFT, so the phases are those of the middle
    for (int i = 0; i < half; ++i) {
        double value = windowed[i];
        windowed[i] = windowed[i + half];
        windowed[i + half] = value;
    }
    kiss_fftr(session->fft, windowed, session->spectrum);
    for (int i = 0; i <= half; ++i) {
        session->bins[i * 2] = float(session->spectrum[i].r);
        session->bins[i * 2 + 1] = float(session->spectrum[i].i);
    }
    session->chordino->process(&session->bins, Vamp::RealTime(timestamp.sec, timestamp.nsec));
}

// Analyses the block at the start of pending and steps past stepSize samples of it
void step(Session *session) {
    analyse(session, session->pending, realtime_from_frame(session->frame, frameRate(session)));
    session->pendingCount -= session->stepSize;
    memmove(session->pending, session->pending + session->stepSize, session->pendingCount * sizeof(float));
    session->frame += session->stepSize;
}

}

void sessionProcess(Session *session, const float *samples, realtime_t timestamp) {
    if (!session->fed) {
        session->frame = realtime_to_frame(timestamp, frameRate(session));
        session->fed = true;
    }
    // pending never holds a whole block between calls, so this always fits
    memcpy(session->pending + session->pendingCount, samples, session->inputBlockSize * sizeof(float));
    session->pendingCount += session->inputBlockSize;
    while (session->pendingCount >= session->blockSize) {
        step(session);
    }
}

Vamp::Plugin::FeatureSet sessionFinish(Session *session) {
    // Only one block more, padded: the frames the last few samples were
    // centred on are not made up
    if (session->pendingCount > 0) {
        memset(session->pending + session->pendingCount, 0,
               (session->blockSize - session->pendingCount) * sizeof(float));
        session->pendingCount = session->blockSize;
        step(session);
    }
    return session->chordino->getRemainingFeatures();
}
