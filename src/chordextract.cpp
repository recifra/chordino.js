#include <string>

#include "vamp-hostsdk/PluginInputDomainAdapter.h"
#include "vamp-hostsdk/PluginBufferingAdapter.h"

#include "nnls-chroma/Chordino.h"
#include "session.h"

#include <iostream>
#include <string>
#include <emscripten/emscripten.h>
#include <emscripten/bind.h>
#include <stdlib.h>
#include <cmath>

using namespace std;
using namespace Vamp;
using namespace Vamp::HostExt;

int main() {
    return 0;
}

namespace {

// See setProgressCallback
emscripten::val progressCallback = emscripten::val::null();
int reportedPercent = -1;

void forwardProgress(double done) {
    // Chordino reports every frame; JS only hears about whole percents
    int percent = (int) floor(done * 100);
    if (percent == reportedPercent) return;
    reportedPercent = percent;
    progressCallback(done);
}

}

// Sets the function getResult() calls with its progress, from 0 to 1, while it
// works through the buffered audio; null or undefined stops the reports. Feeding
// the audio is quick in comparison, the caller can report that part itself.
void setProgressCallback(emscripten::val callback) {
    bool enabled = !callback.isNull() && !callback.isUndefined();
    progressCallback = enabled ? callback : emscripten::val::null();
    reportedPercent = -1;
    chordinoProgress = enabled ? &forwardProgress : nullptr;
}

uintptr_t initializePlugin(
    float samplerate,
    size_t blocksize
) {
    auto *chordino = new Chordino(samplerate);
    auto *ia = new PluginInputDomainAdapter(chordino);
    ia->setProcessTimestampMethod(PluginInputDomainAdapter::ShiftData);
    auto *adapter = new PluginBufferingAdapter(ia);

    // Plugin requires 1 channel (we will mix after)
    if (!adapter->initialise(1, blocksize, blocksize)) {
        delete adapter;
        return 0;
    }
    return (uintptr_t) new Session{chordino, adapter};
}

void releasePlugin(uintptr_t lpSession) {
    auto *session = reinterpret_cast<Session*>(lpSession);
    // The adapters own the plugin they wrap
    delete session->adapter;
    delete session;
}

void resetProcess(uintptr_t lpSession) {
    reinterpret_cast<Session*>(lpSession)->adapter->reset();
}

void processBuffer(
    uintptr_t lpSession,
    uintptr_t buffer,
    long milliseconds
) {
    auto *adapter = reinterpret_cast<Session*>(lpSession)->adapter;
    float* mixbuf = reinterpret_cast<float*>(buffer);

    RealTime timestamp = RealTime::fromMilliseconds((int) milliseconds);

    // feed to plugin: can just take address of buffer, as only one channel
    adapter->process(&mixbuf, timestamp);
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

emscripten::val toChordList(Plugin::FeatureSet &fs, int chordFeatureNo) {
    emscripten::val result = emscripten::val::array();
    if (chordFeatureNo < 0)
    {
        return result;
    }
    Plugin::FeatureList& chordFeatures = fs[chordFeatureNo];
    for (int i = 0; i < (int)chordFeatures.size(); ++i)
    {
        // Construct objects directly in JS
        emscripten::val item = emscripten::val::object();
        item.set("chord", chordFeatures[i].label);
        // RealTime::msec() is only the sub-second part, so build the time from sec too
        const RealTime &timestamp = chordFeatures[i].timestamp;
        item.set("time", timestamp.sec + timestamp.nsec / 1e9);
        result.call<void>("push", item);
    }
    return result;
}

}

// Chords of everything fed, once all of it has been: the last few samples the
// adapter is still holding are padded with zeros to make up a whole frame.
emscripten::val getResult(uintptr_t lpSession) {
    auto *adapter = reinterpret_cast<Session*>(lpSession)->adapter;
    int chordFeatureNo = chordOutput(adapter);
    if (chordFeatureNo < 0)
    {
        return emscripten::val::array();
    }
    // features at end of processing (actually Chordino does all its work here)
    Plugin::FeatureSet fs = adapter->getRemainingFeatures();
    return toChordList(fs, chordFeatureNo);
}

// Chords of the frames analysed so far, for a caller that goes on feeding audio
// afterwards (the realtime worklet). It asks Chordino directly, which can decode
// its frames any number of times. Going through the adapter like getResult does
// would pad its buffered samples with zeros, and the audio fed next would then
// follow those zeros: a gap in the signal on every read.
emscripten::val getPartialResult(uintptr_t lpSession) {
    auto *chordino = reinterpret_cast<Session*>(lpSession)->chordino;
    Plugin::FeatureSet fs = chordino->getRemainingFeatures();
    return toChordList(fs, chordOutput(chordino));
}

// Forgets the frames from before the given time (in the clock processBuffer was
// given), so getPartialResult decodes a bounded window however long the audio
// goes on. The tuning estimate still counts them.
void forgetBefore(uintptr_t lpSession, long milliseconds) {
    reinterpret_cast<Session*>(lpSession)->chordino->forgetFramesBefore(milliseconds / 1000.0);
}

EMSCRIPTEN_BINDINGS(chordextract) {
    emscripten::function("initializePlugin", &initializePlugin);
    emscripten::function("releasePlugin", &releasePlugin);
    emscripten::function("resetProcess", &resetProcess);
    emscripten::function("processBuffer", &processBuffer);
    emscripten::function("getResult", &getResult);
    emscripten::function("getPartialResult", &getPartialResult);
    emscripten::function("forgetBefore", &forgetBefore);
    emscripten::function("setProgressCallback", &setProgressCallback);
}
