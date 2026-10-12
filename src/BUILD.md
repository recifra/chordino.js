## Requirements
- emsdk
- boost
- vamp-plugin-sdk

```sh
# export VAMP_PATH=$HOME/Development/github/vamp-plugin-sdk
```

```sh
emcc --bind -O3 \
    -s WASM=1 \
    -s ALLOW_MEMORY_GROWTH \
    -s WASM_ASYNC_COMPILATION=0 \
    -s EXPORTED_FUNCTIONS=['_malloc','_free'] \
    -s SINGLE_FILE=1 \
    -o chordino.js \
    nnls-chroma/Chordino.cpp \
    nnls-chroma/NNLSBase.cpp \
    nnls-chroma/chromamethods.cpp \
    nnls-chroma/viterbi.cpp \
    nnls-chroma/nnls.c \
    chordextract.cpp \
    $VAMP_PATH/src/vamp-hostsdk/*.c \
    $VAMP_PATH/src/vamp-hostsdk/*.cpp \
    $VAMP_PATH/src/vamp-sdk/RealTime.cpp
```

```sh
cp chordino.js ../examples/extract/chordino.js
cat chordino.js ../examples/extract/chord-worker.js > ../examples/extract/chordino-worker.js
cat chordino.js ../examples/realtime/chord-processor.js > ../examples/realtime/chordino-processor.js
```

`chordino-worker.js` detects the chords of a whole recording in a Web Worker,
posting its progress and then the chords in the `test/fixtures/training.json`
format; `examples/extract/index.html` shows how to drive it. `getResult` does
nearly all of the work in one call, so `setProgressCallback(fn)` has it call `fn`
with its progress (0 to 1) while it runs; `null` turns that off.

## Inspect build (examples/player)
The player in `examples/player` visualizes the per-frame bins Chordino computes
(chroma, smoothed chroma, chord salience, semitone spectrum). It uses its own
bundle so `chordino.js` keeps its size and API: `-DCHORDINO_INSPECT` turns on the
hooks in `nnls-chroma/ChordinoInspect.h` and `inspect.cpp` adds `getAnalysis`.

```sh
emcc --bind -O3 \
    -DCHORDINO_INSPECT \
    -s WASM=1 \
    -s ALLOW_MEMORY_GROWTH \
    -s WASM_ASYNC_COMPILATION=0 \
    -s EXPORTED_FUNCTIONS=['_malloc','_free'] \
    -s SINGLE_FILE=1 \
    -o chordino-inspect.js \
    nnls-chroma/Chordino.cpp \
    nnls-chroma/NNLSBase.cpp \
    nnls-chroma/chromamethods.cpp \
    nnls-chroma/viterbi.cpp \
    nnls-chroma/nnls.c \
    chordextract.cpp \
    inspect.cpp \
    $VAMP_PATH/src/vamp-hostsdk/*.c \
    $VAMP_PATH/src/vamp-hostsdk/*.cpp \
    $VAMP_PATH/src/vamp-sdk/RealTime.cpp
```

```sh
cp chordino-inspect.js ../examples/player/chordino-inspect.js
```

Serve `examples/` (e.g. `python3 -m http.server -d ../examples`), open
`/player/` and drop an audio file on the page.

## Tests
Run from the repository root. They load the bundles in `examples/`, so copy the
builds there first (see above).

```sh
npm install
npm test
```
