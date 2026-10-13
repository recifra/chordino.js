# Building chordino-wasm

## Requirements
- emsdk (`emcc` on the PATH)
- vamp-plugin-sdk
- Node.js, for TypeScript, esbuild and the tests (`npm install`)

## Build
Run from the repository root.

```sh
npm install
export VAMP_PATH=$HOME/Development/github/vamp-plugin-sdk
npm run build
```

`npm run build` runs both halves, which can also run on their own:

- `npm run build:wasm` compiles the C++ with `emcc`; the commands and flags are
  in `src/build.sh`. Needs `VAMP_PATH`.
- `npm run build:js` type-checks the TypeScript in `src/js/` (`tsc -b src/js`)
  and has esbuild minify it into ES modules. It needs no `emcc`, so after a
  change to the worker or the worklet alone this is the one to run.

Everything lands in `build/`:

- `chordino.mjs` + `chordino.wasm`: the production module, an ES module whose
  default export `createChordino()` resolves to the module with
  `initializePlugin`, `processBuffer`, `getResult`, `resetProcess`,
  `releasePlugin`, `_malloc`, `_free` and `HEAPF32`. `getResult` does nearly
  all of the work in one call, so `setProgressCallback(fn)` has it call `fn`
  with its progress (0 to 1) while it runs; `null` turns that off. It is for
  the end of the audio: it pads the samples still buffered with zeros. To read
  while still feeding, as the worklet does, `getPartialResult` decodes the
  frames so far and leaves the buffered samples alone; each frame's chroma is
  worked out once and kept, and `forgetBefore(instance, milliseconds)` drops
  the frames before that time so every read decodes a bounded window.
  `initializePlugin(sampleRate, blockSize, true)` gets each frame out as soon
  as its audio is in, half a block (186 ms at 44.1 kHz) sooner, at the cost of
  the first few frames, so the chords start that much after 0.
- `chord-worker.mjs` (from `src/js/chord-worker.ts`): module worker that
  detects the chords of a whole recording, posting its progress and then
  `{"timestamps": [[chord, seconds], ..., ["", end]]}`.
  `examples/extract/index.html` drives it, including from a CDN on another
  origin (workers must start from the page's origin).
- `chord-processor.mjs` (from `src/js/chord-processor.ts`): AudioWorklet for
  the microphone. Worklets cannot fetch, so the page passes the wasm bytes in
  `processorOptions.wasmBinary`; see `examples/realtime/index.html`.
- `inspect/chordino-inspect.js`: inspection build for `examples/player`, see
  below. Not for apps.

An app ships the four files at the top of `build/` together, in one directory.
The worker and the worklet share the one `.wasm` (15 KB + 273 KB; the old
single-file bundles inlined it in each, 372 KB apiece); they add 1 KB and 3 KB
of their own.

## TypeScript
The worker and the worklet run in different global scopes, a
`DedicatedWorkerGlobalScope` and an `AudioWorkletGlobalScope`, whose typings
clash. So `src/js/tsconfig.json` only references one project per scope:
`tsconfig.worker.json` (lib `webworker`) and `tsconfig.worklet.json`
(`@types/audioworklet`). Both import `./chordino.mjs`, typed by
`src/js/chordino.d.mts` after the bindings at the end of
`src/chordextract.cpp`; keep the two in step. esbuild transpiles each file on
its own and leaves that import as it is, so `chordino.mjs` must sit next to
them.

## Inspect build (examples/player, examples/realtime)
The player in `examples/player` visualizes the per-frame bins Chordino computes
(chroma, smoothed chroma, chord salience, semitone spectrum) for a recording;
`examples/realtime` does the same live from the microphone, next to the chords
the production worklet posts. They use their own bundle so the production one
keeps its size and API: `-DCHORDINO_INSPECT` turns on the hooks in
`src/nnls-chroma/ChordinoInspect.h` and `src/inspect.cpp` adds `getAnalysis`,
and `getPartialAnalysis` for reading while still feeding audio.

## Examples
`examples/lib` and `examples/player/chordino-inspect.js` are symlinks into
`build/`, so build first. Serve `examples/` (e.g. `python3 -m http.server -d
examples`), open `/player/` and drop an audio file on the page, or `/realtime/`
and start listening; `/extract/` tries the production build alone.

## Tests
Run from the repository root. They load the files in `build/`, so build first.

```sh
npm test
```
