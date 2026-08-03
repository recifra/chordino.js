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
cat chordino.js ../examples/realtime/chord-processor.js > ../examples/realtime/chordino-processor.js
```
