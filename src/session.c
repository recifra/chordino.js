#include "session.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static void (*decodeProgress)(double done) = NULL;

session_t *session_create(float sampleRate, int inputBlockSize, bool realtime) {
    int stepSize = CHORDINO_STEP_SIZE;
    int blockSize = CHORDINO_BLOCK_SIZE;
    session_t *session = malloc(sizeof(session_t));
    session->chordino = chordino_create(sampleRate);
    session->sampleRate = sampleRate;
    session->realtime = realtime;
    session->inputBlockSize = inputBlockSize;
    session->stepSize = stepSize;
    session->blockSize = blockSize;
    session->pending = malloc((blockSize + inputBlockSize) * sizeof(float));
    session->shifted = malloc((blockSize + blockSize / 2) * sizeof(float));
    session->spectrum = spectrum_create(blockSize);
    session_reset(session);
    return session;
}

void session_destroy(session_t *session) {
    chordino_destroy(session->chordino);
    free(session->pending);
    free(session->shifted);
    spectrum_destroy(session->spectrum);
    free(session);
}

void session_reset(session_t *session) {
    session->pendingCount = 0;
    session->frame = 0;
    session->fed = false;
    session->analysed = 0;
    chordino_reset(session->chordino);
}

// Frames are timed on the sample rate rounded to a whole number
static unsigned int frame_rate(const session_t *session) {
    return (unsigned int) (int) (session->sampleRate + 0.5);
}

// Hands Chordino the spectrum of the blockSize samples at block, which start at
// timestamp. Chordino times each frame by the centre of its window, so either
// the timestamp moves forward half a block (realtime), or the audio is delayed
// half a block behind it, starting from silence (offline: the first frame is
// then centred on the first sample, and the chords start at 0).
static void analyse(session_t *session, const float *block, realtime_t timestamp) {
    int blockSize = session->blockSize;
    int half = blockSize / 2;
    const float *input = block;
    if (session->realtime) {
        timestamp = realtime_add(timestamp, realtime_from_frame(half, frame_rate(session)));
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

    chordino_process(session->chordino, spectrum_compute(session->spectrum, input), timestamp);
}

// Analyses the block at the start of pending and steps past stepSize samples of it
static void step(session_t *session) {
    analyse(session, session->pending, realtime_from_frame(session->frame, frame_rate(session)));
    session->pendingCount -= session->stepSize;
    memmove(session->pending, session->pending + session->stepSize, session->pendingCount * sizeof(float));
    session->frame += session->stepSize;
}

void session_process(session_t *session, const float *samples, realtime_t timestamp) {
    if (!session->fed) {
        session->frame = realtime_to_frame(timestamp, frame_rate(session));
        session->fed = true;
    }
    // pending never holds a whole block between calls, so this always fits
    memcpy(session->pending + session->pendingCount, samples, session->inputBlockSize * sizeof(float));
    session->pendingCount += session->inputBlockSize;
    while (session->pendingCount >= session->blockSize) {
        step(session);
    }
}

const chordino_result_t *session_finish(session_t *session) {
    // Only one block more, padded: the frames the last few samples were
    // centred on are not made up
    if (session->pendingCount > 0) {
        memset(session->pending + session->pendingCount, 0,
               (session->blockSize - session->pendingCount) * sizeof(float));
        session->pendingCount = session->blockSize;
        step(session);
    }
    return session_decode(session);
}

const chordino_result_t *session_decode(session_t *session) {
    return chordino_decode(session->chordino, decodeProgress);
}

void session_set_progress(void (*progress)(double done)) {
    decodeProgress = progress;
}
