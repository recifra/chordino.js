// Inspection build only (CHORDINO_INSPECT, see BUILD.md): exposes the per-frame
// bins Chordino computes while decoding, for the examples/player and
// examples/realtime visualizations. It is linked next to chordextract.cpp, which
// still provides initializePlugin, processBuffer and releasePlugin; this file
// only adds getAnalysis and getPartialAnalysis.

#include "nnls-chroma/ChordinoInspect.h"
#include "session.h"

#include <map>
#include <string>
#include <vector>
#include <emscripten/bind.h>

using namespace std;
using namespace Vamp;

namespace {

// Filled by the hooks during a single getRemainingFeatures() call; matrices are
// row-major, one row per frame.
struct Collected {
    vector<string> chordNames;
    vector<float> times;
    vector<float> chroma;
    vector<float> smoothedChroma;
    vector<float> salience;
    vector<float> semitones;
    vector<uint8_t> gate;
    bool anyFrame = false;
    double firstFrame = 0;
};

Collected collected;

// Chordino hands over a frame's chroma and semitones only the first time it
// decodes it (see Chordino::m_rawChromas), so while getPartialAnalysis is in use
// they are kept here, by frame time, for the calls after that.
struct Remembered {
    vector<float> chroma;
    vector<float> semitones;
};
map<double, Remembered> remembered;
const vector<float> noChroma(24, 0.f);
const vector<float> noSemitones(84, 0.f);

// Set while getPartialAnalysis runs: frames up to this time are not collected.
bool partial = false;
double collectAfter = 0;

const char *chromaNames[24] = {
    "A (bass)", "Bb (bass)", "B (bass)", "C (bass)", "C# (bass)", "D (bass)",
    "Eb (bass)", "E (bass)", "F (bass)", "F# (bass)", "G (bass)", "Ab (bass)",
    "A", "Bb", "B", "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab"};

const char *pitchNames[12] = {
    "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};

// Semitone bin i is MIDI note 21 + i (A0 upwards).
vector<string> semitoneNames() {
    vector<string> names;
    for (int i = 0; i < 84; i++) {
        int midi = 21 + i;
        names.push_back(string(pitchNames[midi % 12]) + to_string(midi / 12 - 1));
    }
    return names;
}

emscripten::val toFloat32Array(const vector<float> &values) {
    emscripten::val array = emscripten::val::global("Float32Array").new_(values.size());
    array.call<void>("set", emscripten::val(emscripten::typed_memory_view(values.size(), values.data())));
    return array;
}

emscripten::val toUint8Array(const vector<uint8_t> &values) {
    emscripten::val array = emscripten::val::global("Uint8Array").new_(values.size());
    array.call<void>("set", emscripten::val(emscripten::typed_memory_view(values.size(), values.data())));
    return array;
}

emscripten::val bins(const vector<string> &names, const vector<float> &values) {
    emscripten::val binNames = emscripten::val::array();
    for (const string &name : names) {
        binNames.call<void>("push", name);
    }
    emscripten::val out = emscripten::val::object();
    out.set("binNames", binNames);
    out.set("binCount", (int) names.size());
    out.set("values", toFloat32Array(values));
    return out;
}

double seconds(const RealTime &timestamp) {
    return timestamp.sec + timestamp.nsec / 1e9;
}

}

void chordinoInspectBegin(const vector<string> &chordNames) {
    collected = Collected();
    collected.chordNames = chordNames;
}

void chordinoInspectFrame(double time,
                          const vector<float> *chroma,
                          const vector<double> &smoothedChroma,
                          const vector<double> &salience,
                          const float *semitones,
                          int gate) {
    if (!collected.anyFrame) {
        collected.anyFrame = true;
        collected.firstFrame = time;
    }
    if (chroma && partial) {
        remembered[time] = Remembered{*chroma, vector<float>(semitones, semitones + 84)};
    }
    // As a float, like the times handed to JS, which is where collectAfter comes from
    if (partial && float(time) <= collectAfter) {
        return;
    }
    if (!chroma) {
        auto it = remembered.find(time);
        bool kept = it != remembered.end();
        // Decoded before getPartialAnalysis was first called: nothing was kept
        chroma = kept ? &it->second.chroma : &noChroma;
        semitones = kept ? it->second.semitones.data() : noSemitones.data();
    }
    collected.times.push_back(time);
    collected.chroma.insert(collected.chroma.end(), chroma->begin(), chroma->end());
    collected.smoothedChroma.insert(collected.smoothedChroma.end(), smoothedChroma.begin(), smoothedChroma.end());
    collected.salience.insert(collected.salience.end(), salience.begin(), salience.end());
    collected.semitones.insert(collected.semitones.end(), semitones, semitones + 84);
    collected.gate.push_back(gate);
}

namespace {

int chordOutput(Plugin *plugin) {
    Plugin::OutputList outputs = plugin->getOutputDescriptors();
    for (int i = 0; i < int(outputs.size()); ++i)
    {
        if (outputs[i].identifier == "simplechord")
        {
            return i;
        }
    }
    return -1;
}

// The chords in fs plus the bins the hooks collected, which it then clears.
emscripten::val analysisResult(Plugin::FeatureSet &fs, int chordFeatureNo) {
    emscripten::val chords = emscripten::val::array();
    if (chordFeatureNo >= 0)
    {
        Plugin::FeatureList& chordFeatures = fs[chordFeatureNo];
        for (int i = 0; i < (int)chordFeatures.size(); ++i)
        {
            emscripten::val item = emscripten::val::object();
            item.set("chord", chordFeatures[i].label);
            item.set("time", seconds(chordFeatures[i].timestamp));
            chords.call<void>("push", item);
        }
    }

    vector<string> chroma(chromaNames, chromaNames + 24);
    emscripten::val result = emscripten::val::object();
    result.set("chords", chords);
    result.set("times", toFloat32Array(collected.times));
    result.set("chroma", bins(chroma, collected.chroma));
    result.set("smoothedchroma", bins(chroma, collected.smoothedChroma));
    result.set("chordsalience", bins(collected.chordNames, collected.salience));
    result.set("semitonespectrum", bins(semitoneNames(), collected.semitones));
    result.set("gate", toUint8Array(collected.gate));
    collected = Collected();
    return result;
}

}

emscripten::val getAnalysis(uintptr_t lpSession) {
    auto *session = reinterpret_cast<Session*>(lpSession);
    int chordFeatureNo = chordOutput(session->chordino);
    collected = Collected();
    // Chordino does all its work here, calling the hooks once per frame
    Plugin::FeatureSet fs = sessionFinish(session);
    return analysisResult(fs, chordFeatureNo);
}

// Like getAnalysis, for a caller still feeding audio (examples/realtime): it
// decodes through Chordino directly, as getPartialResult does, and returns the
// chords of every frame left (see forgetBefore) but the bins of the frames after
// afterSeconds only, with the values they have now.
emscripten::val getPartialAnalysis(uintptr_t lpSession, double afterSeconds) {
    auto *chordino = reinterpret_cast<Session*>(lpSession)->chordino;
    int chordFeatureNo = chordOutput(chordino);
    collected = Collected();
    partial = true;
    collectAfter = afterSeconds;
    Plugin::FeatureSet fs = chordino->getRemainingFeatures();
    partial = false;
    // Frames before the first one decoded have been forgotten for good
    if (collected.anyFrame) {
        remembered.erase(remembered.begin(), remembered.lower_bound(collected.firstFrame));
    }
    return analysisResult(fs, chordFeatureNo);
}

EMSCRIPTEN_BINDINGS(inspect) {
    emscripten::function("getAnalysis", &getAnalysis);
    emscripten::function("getPartialAnalysis", &getPartialAnalysis);
}
