# Building chordino.js

## Requirements
- emsdk (`emcc` on the PATH)
- Node.js, for TypeScript, esbuild and the tests (`npm install`)

## Build
Run from the repository root.

```sh
npm install
npm run build
```

`npm run build` runs both halves, which can also run on their own:

- `npm run build:wasm` compiles the C++ with `emcc`; the commands and flags are
  in `src/build.sh`. The production module force-includes
  `src/noiostream.h`, which drops every `std::cerr` write so that libc++'s
  streams stay out of the wasm (they were more than half of it); the inspect
  build keeps them. Anything new that formats text should do it without
  streams or `printf`, or the size comes back. The Vamp SDK is no longer
  needed: Chordino, a Vamp plugin upstream, is the `chordino_t` of
  `src/chordino.h`, with no descriptors or parameters (they are constants in
  `src/nnls-chroma/Chordino.cpp`); `src/session.c` and `src/spectrum.c` do
  what the SDK's `PluginBufferingAdapter` and `PluginInputDomainAdapter` did,
  with the same results, over KISS FFT 131.2.0 in `src/kissfft/` (the files
  the build uses, unchanged), with the Hann window of its `Window` in
  `src/window.c`, and `src/realtime.c` does its `RealTime` arithmetic step for
  step. `src/kissfft.c` compiles KISS FFT with `NDEBUG`, for that file only,
  as its error messages would bring `fprintf` back.
- `npm run build:js` type-checks the TypeScript in `src/js/` (`tsc -b src/js`),
  has esbuild minify it into ES modules and copies `src/js/chordino.d.mts`
  next to them. It needs no `emcc`, so after a change to the workers or the
  worklet alone this is the one to run.

Everything lands in `build/`:

- `chordino.mjs` + `chordino.wasm`: the production module, an ES module whose
  default export `createChordino()` resolves to the module with
  `initializePlugin`, `processBuffer`, `getResult`, `resetProcess`,
  `releasePlugin`, `_malloc`, `_free` and `HEAPF32`. `getResult` does nearly
  all of the work in one call, so `setProgressCallback(fn)` has it call `fn`
  with its progress (0 to 1) while it runs; `null` turns that off. It is for
  the end of the audio: it pads the samples still buffered with zeros. To read
  while still feeding, as the live worker does, `getPartialResult` decodes the
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
- `chord-live-worker.mjs` (from `src/js/chord-live-worker.ts`): module worker
  that runs Chordino on the microphone, posting each chord change.
- `chord-processor.mjs` (from `src/js/chord-processor.ts`): AudioWorklet that
  gathers the microphone into buffers and hands them to `chord-live-worker.mjs`
  over a `MessageChannel`. It only copies: decoding on the audio thread overran
  its render quanta, and iOS then dropped microphone samples to catch up, which
  read everything about a semitone sharp. See `examples/realtime/index.html`.
- `chordino.d.mts`: the types of `chordino.mjs`, for the package.
- `inspect/chordino-inspect.js`: inspection build for `examples/player`, see
  below. Not for apps, and not in the package.

An app ships the five files at the top of `build/` together, in one directory.
The two workers share the one `.wasm` (5 KB + 84 KB; the old single-file
bundles inlined it in each, 372 KB apiece); the workers and the worklet add 1,
2 and 1 KB of their own.

## TypeScript
The workers and the worklet run in different global scopes,
`DedicatedWorkerGlobalScope` and `AudioWorkletGlobalScope`, whose typings
clash. So `src/js/tsconfig.json` only references one project per scope:
`tsconfig.worker.json` (lib `webworker`) and `tsconfig.worklet.json`
(`@types/audioworklet`). The workers import `./chordino.mjs`, typed by
`src/js/chordino.d.mts` after `src/chordextract.js`, the API it puts over the
C functions `src/chordextract.c` exports; keep the three in step. esbuild
transpiles each file on
its own and leaves that import as it is, so `chordino.mjs` must sit next to
them.

## Inspect build (examples/player, examples/realtime)
The player in `examples/player` visualizes the per-frame bins Chordino computes
(chroma, smoothed chroma, chord salience, semitone spectrum) for a recording;
`examples/realtime` does the same live from the microphone, next to the chords
the production worker posts. They use their own bundle so the production one
keeps its size and API: `-DCHORDINO_INSPECT` turns on the hooks in
`src/nnls-chroma/ChordinoInspect.h` and `src/inspect.cpp` adds `getAnalysis`,
and `getPartialAnalysis` for reading while still feeding audio.

## Examples
`examples/lib` and `examples/player/chordino-inspect.js` are symlinks into
`build/`, so build first. Serve `examples/` (e.g. `python3 -m http.server -d
examples`), open `/player/` and drop an audio file on the page, or `/realtime/`
and start listening; `/extract/` tries the production build alone.
`examples/vite-react` is a separate Vite app that installs the repository as
its `chordino.js` dependency; see its README.

## Tests
Run from the repository root. They load the files in `build/`, so build first.

```sh
npm test
```

## Publishing
The npm package is `chordino.js`. Its contents are the `files` listed in
`package.json` (the five files an app ships, plus `chordino.d.mts` and the KISS
FFT and Vamp SDK licenses, as the wasm carries code of both) along with
`README.md` and `LICENSE`; `exports` maps `chordino.js` to `chordino.mjs`, and
`chordino.js/chord-worker.mjs`, `chordino.js/chord-live-worker.mjs`,
`chordino.js/chord-processor.mjs` and `chordino.js/chordino.wasm` to the files
in `build/`. `npm publish` first runs
`prepublishOnly`, a full build and the tests, so it needs everything `npm run
build` does. To see what would go out:

```sh
npm pack --dry-run
```
