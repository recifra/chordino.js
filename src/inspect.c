// Inspection build only (CHORDINO_INSPECT, see BUILD.md): exposes the per-frame
// bins Chordino computes while decoding, for the examples/player and
// examples/realtime visualizations. It is linked next to chordextract.c, which
// still provides initializePlugin, processBuffer and releasePlugin; this file
// only adds getAnalysis and getPartialAnalysis, which src/inspect.js wraps.

#include "nnls-chroma/chordino_inspect.h"
#include "session.h"

#include <emscripten/emscripten.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Makes room for count elements of size bytes at *array, which has room for *capacity
static void reserve(void **array, int *capacity, int count, size_t size) {
    if (count <= *capacity) return;
    int grown = *capacity > 0 ? *capacity : 256;
    while (grown < count) grown *= 2;
    *array = realloc(*array, grown * size);
    *capacity = grown;
}

// A growing array of floats
typedef struct {
    float *values;
    int count;
    int capacity;
} floats_t;

static void floats_add(floats_t *floats, const float *values, int count) {
    reserve((void **) &floats->values, &floats->capacity, floats->count + count, sizeof(float));
    memcpy(floats->values + floats->count, values, count * sizeof(float));
    floats->count += count;
}

static void floats_add_doubles(floats_t *floats, const double *values, int count) {
    reserve((void **) &floats->values, &floats->capacity, floats->count + count, sizeof(float));
    for (int i = 0; i < count; i++) floats->values[floats->count + i] = values[i];
    floats->count += count;
}

// Filled by the hooks during a single chordino_decode() call; matrices are
// row-major, one row per frame.
static struct {
    int nChord;
    char *const *chordNames;
    floats_t times;
    floats_t chroma;
    floats_t smoothedChroma;
    floats_t salience;
    floats_t semitones;
    uint8_t *gate;
    int gateCount;
    int gateCapacity;
    bool anyFrame;
    double firstFrame;
} collected;

static void forgetCollected(void) {
    collected.nChord = 0;
    collected.chordNames = NULL;
    collected.times.count = 0;
    collected.chroma.count = 0;
    collected.smoothedChroma.count = 0;
    collected.salience.count = 0;
    collected.semitones.count = 0;
    collected.gateCount = 0;
    collected.anyFrame = false;
    collected.firstFrame = 0;
}

// Chordino hands over a frame's chroma and semitones only the first time it
// decodes it (see rawChromas in chordino.c), so while getPartialAnalysis is in use
// they are kept here, by frame time, for the calls after that: in order of time,
// one entry per time.
typedef struct {
    double time;
    float chroma[24];
    float semitones[84];
} remembered_t;

static remembered_t *remembered;
static int nRemembered;
static int rememberedCapacity;
static const float noChroma[24];
static const float noSemitones[84];

// The first entry not before time
static int rememberedFrom(double time) {
    int low = 0, high = nRemembered;
    while (low < high) {
        int middle = (low + high) / 2;
        if (remembered[middle].time < time) low = middle + 1;
        else high = middle;
    }
    return low;
}

static void remember(double time, const float *chroma, const float *semitones) {
    int at = rememberedFrom(time);
    if (at == nRemembered || remembered[at].time != time) {
        reserve((void **) &remembered, &rememberedCapacity, nRemembered + 1, sizeof(remembered_t));
        memmove(remembered + at + 1, remembered + at, (nRemembered - at) * sizeof(remembered_t));
        nRemembered++;
        remembered[at].time = time;
    }
    memcpy(remembered[at].chroma, chroma, sizeof(remembered[at].chroma));
    memcpy(remembered[at].semitones, semitones, sizeof(remembered[at].semitones));
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
    if (!collected.anyFrame) {
        collected.anyFrame = true;
        collected.firstFrame = time;
    }
    if (chroma && partial) {
        remember(time, chroma, semitones);
    }
    // As a float, like the times handed to JS, which is where collectAfter comes from
    if (partial && (float) time <= collectAfter) {
        return;
    }
    if (!chroma) {
        int at = rememberedFrom(time);
        bool kept = at < nRemembered && remembered[at].time == time;
        // Decoded before getPartialAnalysis was first called: nothing was kept
        chroma = kept ? remembered[at].chroma : noChroma;
        semitones = kept ? remembered[at].semitones : noSemitones;
    }
    float frameTime = time;
    floats_add(&collected.times, &frameTime, 1);
    floats_add(&collected.chroma, chroma, 24);
    floats_add_doubles(&collected.smoothedChroma, smoothedChroma, 24);
    floats_add_doubles(&collected.salience, salience, collected.nChord);
    floats_add(&collected.semitones, semitones, 84);
    reserve((void **) &collected.gate, &collected.gateCapacity, collected.gateCount + 1, sizeof(uint8_t));
    collected.gate[collected.gateCount++] = gate;
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
    addAnalysisBins(collected.times.count, collected.times.values, collected.chroma.values,
                    collected.smoothedChroma.values, collected.nChord, collected.chordNames,
                    collected.salience.values, collected.semitones.values, collected.gate);
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
    // Frames before the first one decoded have been forgotten for good
    if (collected.anyFrame) {
        int keep = rememberedFrom(collected.firstFrame);
        nRemembered -= keep;
        memmove(remembered, remembered + keep, nRemembered * sizeof(remembered_t));
    }
    analysisResult(decoded);
}
