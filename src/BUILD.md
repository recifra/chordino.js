## Requirements
- emsdk (`emcc` on the PATH)
- boost
- vamp-plugin-sdk

## Build
Run from the repository root; the commands and flags are in `src/build.sh`.

```sh
export VAMP_PATH=$HOME/Development/github/vamp-plugin-sdk
npm run build
```

Everything lands in `build/`:

- `chordino.mjs` + `chordino.wasm`: the production module, an ES module whose
  default export `createChordino()` resolves to the module with
  `initializePlugin`, `processBuffer`, `getResult`, `resetProcess`,
  `releasePlugin`, `_malloc`, `_free` and `HEAPF32`. `getResult` does nearly
  all of the work in one call, so `setProgressCallback(fn)` has it call `fn`
  with its progress (0 to 1) while it runs; `null` turns that off.
- `chord-worker.mjs` (from `src/js/`): module worker that detects the chords of
  a whole recording, posting its progress and then `{"timestamps": [[chord,
  seconds], ..., ["", end]]}`. `examples/extract/index.html` drives it,
  including from a CDN on another origin (workers must start from the page's
  origin).
- `chord-processor.mjs` (from `src/js/`): AudioWorklet for the microphone.
  Worklets cannot fetch, so the page passes the wasm bytes in
  `processorOptions.wasmBinary`; see `examples/realtime/index.html`.
- `inspect/chordino-inspect.js`: inspection build for `examples/player`, see
  below. Not for apps.

An app ships the four files at the top of `build/` together, in one directory.
The worker and the worklet share the one `.wasm` (15 KB + 271 KB; the old
single-file bundles inlined it in each, 372 KB apiece).

## Inspect build (examples/player)
The player in `examples/player` visualizes the per-frame bins Chordino computes
(chroma, smoothed chroma, chord salience, semitone spectrum). It uses its own
bundle so the production one keeps its size and API: `-DCHORDINO_INSPECT` turns
on the hooks in `nnls-chroma/ChordinoInspect.h` and `inspect.cpp` adds
`getAnalysis`.

## Examples
`examples/lib` and `examples/player/chordino-inspect.js` are symlinks into
`build/`, so build first. Serve `examples/` (e.g. `python3 -m http.server -d
examples`), open `/player/` and drop an audio file on the page; `/extract/` and
`/realtime/` try the production build.

## Tests
Run from the repository root. They load the files in `build/`, so build first.

```sh
npm install
npm test
```
