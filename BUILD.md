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

- `npm run build:wasm` compiles the C with `emcc`; the commands and flags are
  in `src/build.sh`. It is all C, so no libc++ comes in (its streams alone
  were once more than half of the wasm). Anything new that formats text in the
  production module should do it without `printf`, or the size comes back. The Vamp SDK is no longer
  needed: Chordino, a Vamp plugin upstream, is the `chordino_t` of
  `src/chordino.h`, with no descriptors or parameters (they are constants in
  `src/nnls-chroma/chordino.c`); `src/session.c` and `src/spectrum.c` do
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
  `initializePlugin`, `processBuffer`, `getResult`, `getBeats`, `resetProcess`,
  `releasePlugin`, the tuner's `createPitch`, `detectPitch`, `pitchRms` and
  `releasePitch`, `_malloc`, `_free` and `HEAPF32`. `getResult` does nearly
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
  Offline (not realtime), the instance also hands every sample to the
  `rhythm_t` of `src/rhythm.c`, and `getResult` finds the beats along with
  the chords, which `getBeats(instance)` then reads: `{ bpm, numerator,
  denominator, beats: [{ time, position }] }`, or `null`. `src/onset.c` makes
  the onset strength they come from, the spectral flux of madmom's SuperFlux
  at 100 frames a second; the tempo is the period it repeats at most, weighed
  towards 110 BPM, and the beats are Ellis's dynamic programming over it,
  with the period of every few seconds free to drift up to a quarter off it,
  as a band playing live does.
  The bars are where Chordino's chords change and the bass comes in: four
  beats a bar (2/4 comes out as 4/4), or three, or six eighths of 6/8,
  whichever stands out most, and threes only where the onset strength repeats
  more every three beats than every two as well, with a Viterbi pass that lets a bar start out of turn where the
  accents move for good. Beats that split in three, a third and two thirds
  into them more than halfway, make the meter compound, 6/8 or 12/8, with the
  thirds as its beats. None of it changes a bit of the chords.
  `createPitch(sampleRate, size)` is the `pitch_t` of `src/pitch.c`: YIN over
  windows of `size` samples (even), with the difference function from a
  cross-correlation through KISS FFT, as `pitch_create`, `pitch_detect`,
  `pitch_rms` and `pitch_destroy` in C. It keeps nothing between windows and
  needs nothing of emscripten, so a native build can compile it as is.
- `chord-worker.mjs` (from `src/js/chord-worker.ts`): module worker that
  detects the chords of a whole recording, posting its progress and then
  `{"timestamps": [[chord, seconds], ..., ["", end]], "beats": [[seconds,
  position], ...], "tempo": {"bpm": 76, "meter": [4, 4]}}`.
  `examples/extract/index.html` drives it, including from a CDN on another
  origin (workers must start from the page's origin).
- `chord-live-worker.mjs` (from `src/js/chord-live-worker.ts`): module worker
  that runs Chordino on the microphone, posting each chord change.
- `chord-processor.mjs` (from `src/js/chord-processor.ts`): AudioWorklet that
  gathers the microphone into buffers and hands them to `chord-live-worker.mjs`
  over a `MessageChannel`. It only copies: decoding on the audio thread overran
  its render quanta, and iOS then dropped microphone samples to catch up, which
  read everything about a semitone sharp. See `examples/realtime/index.html`.
  `processorOptions.bufferSize` sets the buffer size, 4096 unless given.
- `tuner-live-worker.mjs` (from `src/js/tuner-live-worker.ts`): module worker
  that reads the pitch of the microphone from `chord-processor.mjs` in
  buffers of 2048, for a tuner: YIN on the newest 4096 samples once per batch,
  an RMS gate, 3 readings within 20 cents and the median of the last 5, a
  gate over the background with `ignoreBackground`, and the clock ratio with
  `clockReports`. See `examples/tuner/index.html`.
- `chordino.d.mts`: the types of `chordino.mjs`, for the package.
- `inspect/chordino-inspect.js`: inspection build for `examples/player`, see
  below. Not for apps, and not in the package.

An app ships the six files at the top of `build/` together, in one directory.
The three workers share the one `.wasm` (5 KB + 87 KB; the old single-file
bundles inlined it in each, 372 KB apiece); the workers and the worklet add 1,
2, 3 and 1 KB of their own.

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
`src/nnls-chroma/chordino_inspect.h` and `src/inspect.c` adds `getAnalysis`,
and `getPartialAnalysis` for reading while still feeding audio, which
`src/inspect.js` wraps.

## Examples
`examples/lib` and `examples/player/chordino-inspect.js` are symlinks into
`build/`, so build first. Serve `examples/` (e.g. `python3 -m http.server -d
examples`), open `/player/` and drop an audio file on the page, or `/realtime/`
and start listening, or `/tuner/`; `/extract/` tries the production build
alone. To listen
from another device, `npm run examples` (`examples/serve.mjs`, no
dependencies) serves them over https with a self-signed certificate kept in
`.cert/`.
`examples/vite-react` is a separate Vite app that installs the repository as
its `chordino.js` dependency; see its README.

## Tests
Run from the repository root. They load the files in `build/`, so build first.

```sh
npm test
```

`test/eval.js` and `test/eval-beats.js` are not part of it: they score a
recording against reference chords, and against reference beats (a beat per
line, its time and its position in the bar, as the Ballroom and GTZAN
annotations have them) or chord changes, while tuning the detectors.

```sh
node test/eval-beats.js training/training.mp3 training/training.json
```

## Lint and typecheck

```sh
npm run lint        # Biome over the JS and TS, biome.json
npm run lint:c      # the C through emcc with -Wall -Wextra -Werror
npm run typecheck   # tsc -b src/js, which npm run build:js runs first too
```

`biome.json` keeps the recommended rules but those against this code's own
ways: assigning in an expression (`const current = worker = new Worker(...)`),
a `forEach` arrow that returns, `onmessage =` in a worker, and the quoted
names of `src/chordextract.js` and `src/inspect.js`, which Closure leaves as
they are.

## CI
`.github/workflows/ci.yml` runs the lint, the typecheck and the tests (after
`lint:c` and a full build, with the emsdk of
`.github/actions/setup-emsdk`, 6.0.3) on every push to `main` and every pull
request. A tag `vX.Y.Z` runs them too and then publishes the package to npm,
with provenance, once the tag is the version in `package.json`:

Bump the version in `package.json`, `package-lock.json` and the CDN URL of
`README.md`, commit, then tag that commit and push both:

```sh
git tag v0.5.1 && git push origin main v0.5.1
```

Publishing needs an npm automation token in the repository's `NPM_TOKEN`
secret, or chordino.js set up on npmjs.com for trusted publishing from this
repository's `ci.yml`.

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
