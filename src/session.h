#ifndef CHORDINO_SESSION_H
#define CHORDINO_SESSION_H

#include "vamp-hostsdk/PluginBufferingAdapter.h"

#include "nnls-chroma/Chordino.h"

// What initializePlugin hands to JS as the instance: the adapter chain the audio
// goes through, and the Chordino at the end of it, which getPartialResult reads
// without going through the adapters.
struct Session {
    Chordino *chordino;
    Vamp::HostExt::PluginBufferingAdapter *adapter;
};

#endif
