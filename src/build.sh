#!/bin/bash
# Builds the wasm side into build/ at the repository root (see BUILD.md):
#   chordino.mjs, chordino.wasm    production module, shared by the Web Worker
#                                  and the AudioWorklet (npm run build:js)
#   inspect/chordino-inspect.js    inspection build for examples/player
# Needs emcc on the PATH and VAMP_PATH pointing at a vamp-plugin-sdk checkout.
set -e
cd "$(dirname "$0")"
: "${VAMP_PATH:?set VAMP_PATH to the vamp-plugin-sdk checkout}"

OUT=../build
SOURCES="nnls-chroma/Chordino.cpp nnls-chroma/NNLSBase.cpp nnls-chroma/chromamethods.cpp
    nnls-chroma/viterbi.cpp nnls-chroma/nnls.c chordextract.cpp"
VAMP="$VAMP_PATH/src/vamp-hostsdk/*.c $VAMP_PATH/src/vamp-hostsdk/*.cpp $VAMP_PATH/src/vamp-sdk/RealTime.cpp"
mkdir -p "$OUT/inspect"

# One ES module plus one .wasm, so an app shipping both the worker and the
# worklet carries the wasm once. Closure, no filesystem and LTO only shrink the
# glue (78 KB -> 15 KB); -Oz would save 75 KB of wasm but doubles decode time.
emcc --bind -O3 --closure 1 -flto \
    -s FILESYSTEM=0 \
    -s ALLOW_MEMORY_GROWTH \
    -s MODULARIZE=1 -s EXPORT_ES6=1 -s EXPORT_NAME=createChordino \
    -s "EXPORTED_FUNCTIONS=['_malloc','_free']" \
    -s "EXPORTED_RUNTIME_METHODS=['HEAPF32']" \
    -o "$OUT/chordino.mjs" \
    $SOURCES $VAMP
chmod 644 "$OUT/chordino.wasm"

# Separate single-file bundle with the CHORDINO_INSPECT hooks and getAnalysis,
# so the production module keeps its size and API
emcc --bind -O3 \
    -DCHORDINO_INSPECT \
    -s WASM=1 \
    -s ALLOW_MEMORY_GROWTH \
    -s WASM_ASYNC_COMPILATION=0 \
    -s "EXPORTED_FUNCTIONS=['_malloc','_free']" \
    -s SINGLE_FILE=1 \
    -o "$OUT/inspect/chordino-inspect.js" \
    $SOURCES inspect.cpp $VAMP
