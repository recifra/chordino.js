// Inspection build only (CHORDINO_INSPECT, see BUILD.md): exposes the per-frame
// bins Chordino computes while decoding, for the examples/player and
// examples/realtime visualizations. It is linked next to chordextract.c, which
// still provides initializePlugin, processBuffer and releasePlugin; this file
// only adds getAnalysis and getPartialAnalysis, which src/inspect.js wraps.

#include "nnls-chroma/chordino_inspect.h"
#include "session.h"
#include "vector.h"

#include <emscripten/emscripten.h>
#include <stdbool.h>
#include <stdint.h>

// Adds count doubles to a vector of floats
static void pushDoubles(vector_t *floats, const double *values, int count) {
    float *added = vector_push(floats, NULL, count);
    for (int i = 0; i < count; i++) added[i] = values[i];
}

// Filled by the hooks during a single chordino_decode() call: floats, but for
// the uint8_t gate; matrices are row-major, one row per frame.
static struct {
    int nChord;
    char *const *chordNames;
    vector_t times;
    vector_t chroma;
    vector_t smoothedChroma;
    vector_t salience;
    vector_t semitones;
    vector_t gate;
} collected = {
    .times = {.size = sizeof(float)},
    .chroma = {.size = sizeof(float)},
    .smoothedChroma = {.size = sizeof(float)},
    .salience = {.size = sizeof(float)},
    .semitones = {.size = sizeof(float)},
    .gate = {.size = sizeof(uint8_t)},
};

static void forgetCollected(void) {
    collected.nChord = 0;
    collected.chordNames = NULL;
    vector_clear(&collected.times);
    vector_clear(&collected.chroma);
    vector_clear(&collected.smoothedChroma);
    vector_clear(&collected.salience);
    vector_clear(&collected.semitones);
    vector_clear(&collected.gate);
}

// Set while getPartialAnalysis runs: frames up to this time are not collected.
static bool partial = false;
static double collectAfter = 0;

void chordino_inspect_begin(int nChord, char *const *chordNames) {
    forgetCollected();
    collected.nChord = nChord;
    collected.chordNames = chordNames;
}

void chordino_inspect_frame(double time,
                            const float *chroma,
                            const double *smoothedChroma,
                            const double *salience,
                            const float *semitones,
                            int gate) {
    // As a float, like the times handed to JS, which is where collectAfter comes from
    if (partial && (float) time <= collectAfter) {
        return;
    }
    float frameTime = time;
    vector_push(&collected.times, &frameTime, 1);
    vector_push(&collected.chroma, chroma, 24);
    pushDoubles(&collected.smoothedChroma, smoothedChroma, 24);
    pushDoubles(&collected.salience, salience, collected.nChord);
    vector_push(&collected.semitones, semitones, 84);
    vector_push(&collected.gate, &(uint8_t) {gate}, 1);
}

EM_JS_DEPS(inspect, "$UTF8ToString");

// Starts the object getAnalysis returns, with the chords first
EM_JS(void, startAnalysis, (void), {
    analysis = { 'chords': [] };
});

EM_JS(void, addAnalysisChord, (const char *label, double time), {
    analysis['chords'].push({ 'chord': UTF8ToString(label), 'time': time });
});

// Adds the bins collected, as Float32Arrays of nFrame rows, each with the names
// of its columns
EM_JS(void, addAnalysisBins, (int nFrame, const float *times, const float *chroma,
                              const float *smoothedChroma, int nChord, char *const *chordNames,
                              const float *salience, const float *semitones, const uint8_t *gate), {
    var floats = (pointer, count) => HEAPF32.slice(pointer >> 2, (pointer >> 2) + count);
    var bins = (names, values) => ({ 'binNames': names, 'binCount': names.length, 'values': values });
    var chromaNames = [
        'A (bass)', 'Bb (bass)', 'B (bass)', 'C (bass)', 'C# (bass)', 'D (bass)',
        'Eb (bass)', 'E (bass)', 'F (bass)', 'F# (bass)', 'G (bass)', 'Ab (bass)',
        'A', 'Bb', 'B', 'C', 'C#', 'D', 'Eb', 'E', 'F', 'F#', 'G', 'Ab'];
    var names = [];
    for (var i = 0; i < nChord; i++) names.push(UTF8ToString(HEAPU32[(chordNames >> 2) + i]));
    // Semitone bin i is MIDI note 21 + i (A0 upwards)
    var pitchNames = ['C', 'C#', 'D', 'Eb', 'E', 'F', 'F#', 'G', 'Ab', 'A', 'Bb', 'B'];
    var semitoneNames = [];
    for (var midi = 21; midi < 21 + 84; midi++) semitoneNames.push(pitchNames[midi % 12] + (Math.floor(midi / 12) - 1));
    analysis['times'] = floats(times, nFrame);
    analysis['chroma'] = bins(chromaNames, floats(chroma, nFrame * 24));
    analysis['smoothedchroma'] = bins(chromaNames, floats(smoothedChroma, nFrame * 24));
    analysis['chordsalience'] = bins(names, floats(salience, nFrame * nChord));
    analysis['semitonespectrum'] = bins(semitoneNames, floats(semitones, nFrame * 84));
    analysis['gate'] = HEAPU8.slice(gate, gate + nFrame);
});

// The chords of the decode plus the bins the hooks collected, which it then
// clears, for inspect.js to hand back.
static void analysisResult(const chordino_result_t *decoded) {
    startAnalysis();
    for (int i = 0; i < decoded->chordCount; ++i) {
        addAnalysisChord(decoded->chords[i].label, realtime_to_seconds(decoded->chords[i].time));
    }
    addAnalysisBins(collected.times.count, collected.times.data, collected.chroma.data,
                    collected.smoothedChroma.data, collected.nChord, collected.chordNames,
                    collected.salience.data, collected.semitones.data, collected.gate.data);
    forgetCollected();
}

EMSCRIPTEN_KEEPALIVE
void getAnalysis(session_t *session) {
    forgetCollected();
    // Chordino does all its work here, calling the hooks once per frame
    analysisResult(session_finish(session));
}

// Like getAnalysis, for a caller still feeding audio (examples/realtime): it
// decodes without finishing the session, as getPartialResult does, and returns the
// chords of every frame left (see forgetBefore) but the bins of the frames after
// afterSeconds only, with the values they have now.
EMSCRIPTEN_KEEPALIVE
void getPartialAnalysis(session_t *session, double afterSeconds) {
    forgetCollected();
    partial = true;
    collectAfter = afterSeconds;
    const chordino_result_t *decoded = session_decode(session);
    partial = false;
    analysisResult(decoded);
}
