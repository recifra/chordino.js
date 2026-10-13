# chordino.js with React and Vite

A React 19 app built with Vite 8 that detects chords two ways:

- from an audio file, in a Web Worker, showing progress and then the list of
  chords;
- from the microphone, through an AudioWorklet that hands it to a second Web
  Worker, showing the chord being played.

All of the chordino.js code is in two hooks in [src/chordino.ts](src/chordino.ts),
`useChordDetection` and `useLiveChords`; [src/App.tsx](src/App.tsx) only renders
what they return.

## Run it

The example installs chordino.js from this repository (`file:../..`), so build
it first:

```sh
# at the repository root
npm install
npm run build

cd examples/vite-react
npm install
npm run dev
```

Open the page, pick an audio file (`examples/extract/chords.mp3` will do), or
press Listen and play something.

## In your own app

```sh
npm install chordino.js
```

Copy [src/chordino.ts](src/chordino.ts) and set `worker.format` in
`vite.config.ts` as below. Vite then takes care of the files: no copying to
`public/`, and they get hashed names like the rest of the build.

### How it fits together

```ts
import ChordLiveWorker from 'chordino.js/chord-live-worker.mjs?worker';
import ChordWorker from 'chordino.js/chord-worker.mjs?worker';
import processorUrl from 'chordino.js/chord-processor.mjs?worker&url';
```

- `?worker` has Vite bundle each worker together with the `chordino.mjs` it
  imports, and gives back a constructor. Vite also emits `chordino.wasm`, which
  that code loads, as an asset, once for both.
- `?worker&url` bundles the worklet the same way and gives back its URL, for
  `audioWorklet.addModule`. It only hands the microphone to the live worker over
  a `MessageChannel`, so it imports nothing.

`vite.config.ts` needs one setting:

```ts
export default defineConfig({
  plugins: [react()],
  worker: { format: 'es' },
});
```

Vite bundles workers as `iife` by default, which swaps `import.meta.url` for
`self.location`. Up to chordino.js 0.1 the worklet imported `chordino.mjs`, and
as an AudioWorklet has neither `self` nor `location`, it failed with `self is
not defined`. It imports nothing now; the setting keeps the workers ES modules
like the files in the package.

`vite build` warns that `Module "node:module" has been externalized for browser
compatibility`. That import is in the branch of `chordino.mjs` that only runs
under Node, so the warning is harmless.
