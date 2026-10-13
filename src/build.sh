#!/bin/bash
# Builds the wasm side into build/ at the repository root (see BUILD.md):
#   chordino.mjs, chordino.wasm    production module, shared by the Web Worker
#                                  and the AudioWorklet (npm run build:js)
#   inspect/chordino-inspect.js    inspection build for examples/player
# Needs emcc on the PATH.
set -e
cd "$(dirname "$0")"

OUT=../build
SOURCES="nnls-chroma/Chordino.cpp nnls-chroma/chromamethods.cpp nnls-chroma/viterbi.cpp
    nnls-chroma/nnls.c session.c spectrum.c window.c kissfft.c realtime.c
    chordextract.c"
# Nothing of the Vamp SDK: session.c and spectrum.c do what its host adapters
# did, window.c what its Window did and realtime.c what its RealTime did. The
# FFT in double precision, as the Vamp SDK ran it, in every file that includes
# kissfft.
FLAGS="-Dkiss_fft_scalar=double"
mkdir -p "$OUT/inspect"

# One ES module plus one .wasm, so an app shipping both the worker and the
# worklet carries the wasm once. Closure, no filesystem and LTO only shrink the
# glue (78 KB -> 15 KB), and chordextract.js in place of embind takes it to
# 7 KB; EXPORT_KEEPALIVE=0 leaves the C functions it wraps off Module.
# noiostream.h keeps libc++'s streams out of the wasm (273 KB -> 127 KB);
# -Oz would save 48 KB more but doubles decode time.
emcc -O3 --closure 1 -flto $FLAGS \
    -include noiostream.h \
    --post-js chordextract.js \
    -s FILESYSTEM=0 \
    -s EXPORT_KEEPALIVE=0 \
    -s ALLOW_MEMORY_GROWTH \
    -s MODULARIZE=1 -s EXPORT_ES6=1 -s EXPORT_NAME=createChordino \
    -s "EXPORTED_FUNCTIONS=['_malloc','_free']" \
    -s "EXPORTED_RUNTIME_METHODS=['HEAPF32']" \
    -o "$OUT/chordino.mjs" \
    $SOURCES
chmod 644 "$OUT/chordino.wasm"

# Separate single-file bundle with the CHORDINO_INSPECT hooks and getAnalysis,
# so the production module keeps its size and API. inspect.cpp still binds
# getAnalysis with embind.
emcc --bind -O3 $FLAGS \
    -DCHORDINO_INSPECT \
    --post-js chordextract.js \
    -s WASM=1 \
    -s ALLOW_MEMORY_GROWTH \
    -s WASM_ASYNC_COMPILATION=0 \
    -s "EXPORTED_FUNCTIONS=['_malloc','_free']" \
    -s SINGLE_FILE=1 \
    -o "$OUT/inspect/chordino-inspect.js" \
    $SOURCES inspect.cpp
