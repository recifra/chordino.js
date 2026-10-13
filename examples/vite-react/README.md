# chordino.js with React and Vite

A React 19 app built with Vite 8 that detects chords two ways:

- from an audio file, in a Web Worker, showing progress and then the list of
  chords;
- from the microphone, in an AudioWorklet, showing the chord being played.

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
import ChordWorker from 'chordino.js/chord-worker.mjs?worker';
import processorUrl from 'chordino.js/chord-processor.mjs?worker&url';
import wasmUrl from 'chordino.js/chordino.wasm?url';
```

- `?worker` has Vite bundle the worker together with the `chordino.mjs` it
  imports, and gives back a constructor. Vite also emits `chordino.wasm`, which
  that code loads, as an asset.
- `?worker&url` bundles the worklet the same way and gives back its URL, for
  `audioWorklet.addModule`. Plain `?url` would not do: it copies the file alone,
  and its import of `./chordino.mjs` would then fail in the build.
- `?url` gives the URL of `chordino.wasm`, whose bytes the page fetches and
  hands to the worklet, since worklets cannot fetch.

`vite.config.ts` needs one setting:

```ts
export default defineConfig({
  plugins: [react()],
  worker: { format: 'es' },
});
```

Vite bundles workers as `iife` by default, which swaps `import.meta.url` for
`self.location`. An AudioWorklet has neither `self` nor `location`, so with the
default the worklet fails with `self is not defined`; the worker would be fine.

`vite build` warns that `Module "node:module" has been externalized for browser
compatibility`. That import is in the branch of `chordino.mjs` that only runs
under Node, so the warning is harmless.
