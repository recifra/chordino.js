#include <string>

#include "vamp-hostsdk/PluginInputDomainAdapter.h"
#include "vamp-hostsdk/PluginBufferingAdapter.h"

#include "nnls-chroma/Chordino.h"

#include <iostream>
#include <string>
#include <emscripten/emscripten.h>
#include <emscripten/bind.h>
#include <stdlib.h>

using namespace std;
using namespace Vamp;
using namespace Vamp::HostExt;

int main() {
    return 0;
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
        return 0;
    }
    return (uintptr_t) adapter;
}

void releasePlugin(uintptr_t lpAdapter) {
    auto *adapter = reinterpret_cast<PluginBufferingAdapter*>(lpAdapter);
    delete adapter;
}

void resetProcess(uintptr_t lpAdapter) {
    auto *adapter = reinterpret_cast<PluginBufferingAdapter*>(lpAdapter);
    adapter->reset();
}

void processBuffer(
    uintptr_t lpAdapter,
    uintptr_t buffer,
    long milliseconds
) {
    auto *adapter = reinterpret_cast<PluginBufferingAdapter*>(lpAdapter);
    float* mixbuf = reinterpret_cast<float*>(buffer);

    RealTime timestamp = RealTime::fromMilliseconds((int) milliseconds);

    // feed to plugin: can just take address of buffer, as only one channel
    adapter->process(&mixbuf, timestamp);
}

emscripten::val getResult(uintptr_t lpAdapter) {
    auto *adapter = reinterpret_cast<PluginBufferingAdapter*>(lpAdapter);
    int chordFeatureNo = -1;
    Plugin::OutputList outputs = adapter->getOutputDescriptors();
    for (int i = 0; i < int(outputs.size()); ++i)
    {
        if (outputs[i].identifier == "simplechord")
        {
            chordFeatureNo = i;
            break;
        }
    }
    emscripten::val result = emscripten::val::array();
    if (chordFeatureNo < 0)
    {
        return result;
    }
    // features at end of processing (actually Chordino does all its work here)
    Plugin::FeatureSet fs = adapter->getRemainingFeatures();
    Plugin::FeatureList& chordFeatures = fs[chordFeatureNo];
    for (int i = 0; i < (int)chordFeatures.size(); ++i)
    {
        // Construct objects directly in JS
        emscripten::val item = emscripten::val::object();
        item.set("chord", chordFeatures[i].label);
        item.set("time", chordFeatures[i].timestamp.msec() / 1000.f);
        result.call<void>("push", item);
    }
    return result;
}

EMSCRIPTEN_BINDINGS(chordextract) {
    emscripten::function("initializePlugin", &initializePlugin);
    emscripten::function("releasePlugin", &releasePlugin);
    emscripten::function("resetProcess", &resetProcess);
    emscripten::function("processBuffer", &processBuffer);
    emscripten::function("getResult", &getResult);
}
