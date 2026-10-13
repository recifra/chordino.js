#ifndef CHORDINO_NOIOSTREAM_H
#define CHORDINO_NOIOSTREAM_H

// Force-included into the production build only (src/build.sh). A single use
// of std::cerr links libc++'s streams and locales, and printf and scanf behind
// them: more than half of the wasm (273 KB -> 127 KB without). Chordino and the
// Vamp SDK only write diagnostics there, and the SDK is not ours to edit, so
// every cerr becomes a stream that drops what it is given.
#ifdef __cplusplus
#include <iostream>

namespace std {
struct chordino_null_stream {
    template <class T> chordino_null_stream &operator<<(const T &) { return *this; }
    chordino_null_stream &operator<<(ostream &(*)(ostream &)) { return *this; }
};
inline chordino_null_stream chordino_null_cerr;
}

#define cerr chordino_null_cerr
#endif

#endif
