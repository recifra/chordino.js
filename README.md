# chordino.js

Chord detection in the browser: Chordino, the chord estimation plugin from
Matthias Mauch's NNLS Chroma Vamp plugins, compiled to WebAssembly, with two
ways to run it:

- **Whole recordings**, in a Web Worker: hand it the samples, get back the
  chords with their start times, and progress while it works.
- **Live input**, in an AudioWorklet: feed it the microphone, get each chord
  change as it happens.

Both share one `chordino.wasm` and run with no server: everything happens in the
page.

## Install

```sh
npm install chordino.js
```

The package holds these files, in `node_modules/chordino.js/build/`:

| File | Size | What it is |
| --- | --- | --- |
| `chordino.wasm` | 273 KB (110 KB gzipped) | Chordino and the Vamp host SDK |
| `chordino.mjs` | 15 KB | ES module that loads the wasm; default export `createChordino()` |
| `chord-worker.mjs` | 1 KB | Module worker for whole recordings |
| `chord-processor.mjs` | 3 KB | AudioWorklet processor for live input |
| `chordino.d.mts` | 2 KB | Types of `chordino.mjs` |

The worker and the worklet are started from a URL, not imported, and each loads
`chordino.mjs` from next to itself, which loads `chordino.wasm` the same way. So
an app serves those four files together, from one directory; copy them into its
static assets as part of its build:

```sh
mkdir -p public/chordino
cp node_modules/chordino.js/build/*.mjs node_modules/chordino.js/build/*.wasm public/chordino/
```

Or load them from a CDN that mirrors npm, such as
`https://cdn.jsdelivr.net/npm/chordino.js@0.1.0/build/`. A worker from another
origin needs the shim shown below.

## Usage

### Whole recordings (Web Worker)

Decode and mix down to mono on the main thread (workers have no `AudioContext`),
then post the samples:

```js
const lib = new URL('/chordino/', location.href); // where the files above are served

const worker = new Worker(new URL('chord-worker.mjs', lib), { type: 'module' });
worker.onmessage = ({ data }) => {
  if (data.type === 'progress') {
    console.log(`${Math.round(data.value * 100)}%`);
  } else if (data.type === 'done') {
    console.log(data.result.timestamps); // [['G', 0], ['C', 1.486], ['F', 2.833], ['', 6.002]]
  } else if (data.type === 'error') {
    console.error(data.message);
  }
};
worker.postMessage({ samples, sampleRate }, [samples.buffer]); // samples: mono Float32Array
```

The worker posts, in order:

- `{ type: 'progress', value }`, with `value` going from 0 to 1, once per whole
  percent;
- `{ type: 'done', result }`, where `result.timestamps` is a list of
  `[chord, seconds]` in time order. Chords use Chordino's labels (`C`, `Am`,
  `G7`, `C7M`, `Bº`, `Dsus4`, `C/E`, ...); `''` means no chord, and the list
  always ends with `['', duration]`;
- `{ type: 'error', message }` in place of `done` when the analysis fails.

A worker can be posted any number of recordings, one after another.

Browsers only start a worker from the page's own origin. When the files live on
a CDN, start it from a same-origin blob that imports the real one; inside it,
`import.meta.url` still points at the CDN, so `chordino.mjs` and
`chordino.wasm` load from there (the CDN has to send CORS headers):

```js
function createWorker(url) {
  if (url.origin === location.origin) {
    return new Worker(url, { type: 'module' });
  }
  const shim = new Blob([`import ${JSON.stringify(url.href)};`], { type: 'text/javascript' });
  return new Worker(URL.createObjectURL(shim), { type: 'module' });
}
```

### Live input (AudioWorklet)

Worklets cannot fetch, so the page fetches `chordino.wasm` and hands its bytes
to the processor:

```js
const context = new AudioContext();
const stream = await navigator.mediaDevices.getUserMedia({ audio: { echoCancellation: false } });
const wasmBinary = await (await fetch(new URL('chordino.wasm', lib))).arrayBuffer();
await context.audioWorklet.addModule(new URL('chord-processor.mjs', lib));

const node = new AudioWorkletNode(context, 'chord-processor', {
  channelCount: 1,
  channelCountMode: 'explicit',
  channelInterpretation: 'discrete',
  processorOptions: { wasmBinary, delayMs: 1000 },
});
node.port.onmessage = ({ data }) => {
  if (data.type === 'chordResult') {
    console.log(data.result.chord, 'since', data.result.time); // time on the context.currentTime clock
  }
};
context.createMediaStreamSource(stream).connect(node);
node.connect(context.destination); // it outputs silence
```

The processor posts:

- `{ type: 'ready' }` once the wasm is up; audio before that is skipped;
- `{ type: 'chordResult', result: { chord, time } }` on every chord change.
  `time` is when the chord started, in seconds on the `AudioContext` clock. It
  arrives `delayMs` plus about 0.2 s after that. Stretches of near silence
  post nothing;
- `{ type: 'error', message }` when the wasm cannot load;
- `{ type: 'stopped' }` after it is sent `{ type: 'stop' }` and has released
  its memory. Wait for it before closing the context.

`processorOptions.delayMs` (default 1000) is how far behind the newest audio the
chord is read. The newest frames are the least settled, so a lower value shows
chords sooner and a higher one makes them agree with what the worker finds in
the same audio: about 95% of the time at 0 ms, 99% at 1000 ms.

### The module itself

`chordino.mjs` is the package's main entry, for use in Node or in a worker of
your own:

```js
import createChordino from 'chordino.js';

const Module = await createChordino(); // loads chordino.wasm from next to chordino.mjs
const instance = Module.initializePlugin(sampleRate, 4096);
```

The module has `initializePlugin`, `processBuffer`, `getResult`,
`getPartialResult`, `forgetBefore` and a few more; [BUILD.md](BUILD.md)
describes them and the package carries their types.
[src/js/chord-worker.ts](src/js/chord-worker.ts) is a short example of the
feeding loop.

## Examples

`examples/` has three pages that run on the build:

- `extract/`: detects the chords of an MP3 through the worker and offers them as
  JSON, the way an app would.
- `player/`: plays a recording and shows, frame by frame, the chroma, chord
  salience and semitone spectrum Chordino works from, next to the chords it
  finds.
- `realtime/`: the same from the microphone, through the production worklet.

Build first, then serve the directory and open one of them:

```sh
python3 -m http.server -d examples
```

## Building and testing

From a clone of this repository. You need emsdk, a checkout of the
[Vamp plugin SDK](https://github.com/vamp-plugins/vamp-plugin-sdk) and Node.js:

```sh
npm install
export VAMP_PATH=/path/to/vamp-plugin-sdk
npm run build  # or npm run build:js, for the TypeScript alone
npm test
```

[BUILD.md](BUILD.md) has the details.

## Layout

| Path | What it holds |
| --- | --- |
| `src/nnls-chroma/` | The NNLS Chroma / Chordino plugin source, adapted for this build |
| `src/chordextract.cpp`, `src/session.h` | The bindings `chordino.mjs` exposes |
| `src/inspect.cpp` | Extra bindings of the inspect build |
| `src/js/` | The worker and the worklet, in TypeScript |
| `src/build.sh` | The `emcc` commands |
| `examples/` | The example pages |
| `test/` | Tests, run with `node --test` against `build/` |

## Credits and license

NNLS Chroma and Chordino are by Matthias Mauch, Centre for Digital Music, Queen
Mary University of London, and are licensed under the GNU General Public License,
version 2 or later (see [src/nnls-chroma/COPYING](src/nnls-chroma/COPYING)). The
modules in `build/` are compiled from that code, so chordino.js is distributed
under the same license (see [LICENSE](LICENSE)). The Vamp plugin SDK is by
Chris Cannam and Queen Mary University of London, under a BSD-style license.

If you use Chordino in research, please cite:

> Matthias Mauch and Simon Dixon. *Approximate Note Transcription for the
> Improved Identification of Difficult Chords.* In Proceedings of the 11th
> International Society for Music Information Retrieval Conference (ISMIR
> 2010), 2010.
