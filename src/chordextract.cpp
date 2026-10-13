#include "session.h"

#include <emscripten/emscripten.h>
#include <cmath>

// The functions exported here are plain C ones; src/chordextract.js wraps them
// in the API src/js/chordino.d.mts describes, without embind and its runtime.

EM_JS_DEPS(chordextract, "$UTF8ToString");

// Adds a chord to the list chordextract.js hands back from getResult and
// getPartialResult
EM_JS(void, pushChord, (const char *label, double time), {
    chordList.push({ 'chord': UTF8ToString(label), 'time': time });
});

// Calls the function setProgressCallback was given
EM_JS(void, reportProgress, (double done), {
    progressCallback(done);
});

namespace {

int reportedPercent = -1;

void forwardProgress(double done) {
    // Chordino reports every frame; JS only hears about whole percents
    int percent = (int) floor(done * 100);
    if (percent == reportedPercent) return;
    reportedPercent = percent;
    reportProgress(done);
}

}

// Has getResult() report its progress, from 0 to 1, while it works through the
// buffered audio. Feeding the audio is quick in comparison, the caller can
// report that part itself.
extern "C" EMSCRIPTEN_KEEPALIVE
void setProgressReports(bool enabled) {
    reportedPercent = -1;
    session_set_progress(enabled ? &forwardProgress : nullptr);
}

// Every frame is centred on its timestamp. Offline, that is done by delaying the
// audio half a block, so the first frame is centred on 0 and the chords start at
// 0, as the offline extraction reports them; but then a frame only comes out half
// a block (186 ms at 44.1 kHz) after the end of its window. With realtime set,
// each block is analysed as it comes and its timestamp moved to its centre
// instead: the same frames minus the first few, each out half a block sooner,
// which is what the realtime worklet wants.
extern "C" EMSCRIPTEN_KEEPALIVE
session_t *initializePlugin(
    float samplerate,
    size_t blocksize,
    bool realtime
) {
    return session_create(samplerate, int(blocksize), realtime);
}

extern "C" EMSCRIPTEN_KEEPALIVE
void releasePlugin(session_t *session) {
    session_destroy(session);
}

extern "C" EMSCRIPTEN_KEEPALIVE
void resetProcess(session_t *session) {
    session_reset(session);
}

extern "C" EMSCRIPTEN_KEEPALIVE
void processBuffer(
    session_t *session,
    float *buffer,
    long milliseconds
) {
    session_process(session, buffer, realtime_from_milliseconds((int) milliseconds));
}

namespace {

void pushChords(const chordino_result_t *result) {
    for (int i = 0; i < result->chordCount; ++i) {
        pushChord(result->chords[i].label, realtime_to_seconds(result->chords[i].time));
    }
}

}

// Chords of everything fed, once all of it has been: the last few samples still
// pending are padded with zeros to make up a whole frame.
extern "C" EMSCRIPTEN_KEEPALIVE
void getResult(session_t *session) {
    // Chordino does nearly all of its work here
    pushChords(session_finish(session));
}

// Chords of the frames analysed so far, for a caller that goes on feeding audio
// afterwards (the realtime worklet). Chordino can decode its frames any number
// of times. Finishing the session like getResult does would pad its pending
// samples with zeros, and the audio fed next would then follow those zeros: a
// gap in the signal on every read.
extern "C" EMSCRIPTEN_KEEPALIVE
void getPartialResult(session_t *session) {
    pushChords(session_decode(session));
}

// Forgets the frames from before the given time (in the clock processBuffer was
// given), so getPartialResult decodes a bounded window however long the audio
// goes on. The tuning estimate still counts them.
extern "C" EMSCRIPTEN_KEEPALIVE
void forgetBefore(session_t *session, long milliseconds) {
    chordino_forget_before(session->chordino, realtime_from_seconds(milliseconds / 1000.0));
}
