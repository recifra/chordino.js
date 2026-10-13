# chordino.js

Chord detection in the browser: Chordino, the chord estimation plugin from
Matthias Mauch's NNLS Chroma Vamp plugins, compiled to WebAssembly, with two
ways to run it:

- **Whole recordings**, in a Web Worker: hand it the samples, get back the
  chords with their start times, and progress while it works.
- **Live input**, in an AudioWorklet that hands the microphone to a Web
  Worker: get each chord change as it happens.

Both share one `chordino.wasm` and run with no server: everything happens in the
page.

## Install

```sh
npm install chordino.js
```

The package holds these files, in `node_modules/chordino.js/build/`:

| File | Size | What it is |
| --- | --- | --- |
| `chordino.wasm` | 127 KB (58 KB gzipped) | Chordino and the Vamp host SDK |
| `chordino.mjs` | 15 KB | ES module that loads the wasm; default export `createChordino()` |
| `chord-worker.mjs` | 1 KB | Module worker for whole recordings |
| `chord-live-worker.mjs` | 2 KB | Module worker that runs Chordino on live input |
| `chord-processor.mjs` | 1 KB | AudioWorklet processor that hands live input to it |
| `chordino.d.mts` | 2 KB | Types of `chordino.mjs` |

The workers and the worklet are started from a URL, not imported. Each worker
loads `chordino.mjs` from next to itself, which loads `chordino.wasm` the same
way. So an app serves those five files together, from one directory; copy them
into its static assets as part of its build:

```sh
mkdir -p public/chordino
cp node_modules/chordino.js/build/*.mjs node_modules/chordino.js/build/*.wasm public/chordino/
```

Or load them from a CDN that mirrors npm, such as
`https://cdn.jsdelivr.net/npm/chordino.js@0.2.0/build/`. A worker from another
origin needs the shim shown below. With Vite there is nothing to copy: see
[With Vite](#with-vite).

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

### Live input (AudioWorklet and Web Worker)

The AudioWorklet only gathers the microphone into buffers and hands them to a
module worker, which runs Chordino. Decoding a buffer takes several
milliseconds, against 2.7 ms for a whole render quantum at 48 kHz, and an audio
thread that keeps overrunning its quanta makes iOS drop microphone samples to
catch up, which reads everything about a semitone sharp. The two talk over a
`MessageChannel`, so the audio never goes through the main thread:

```js
const context = new AudioContext();
const stream = await navigator.mediaDevices.getUserMedia({ audio: { echoCancellation: false } });
const worker = new Worker(new URL('chord-live-worker.mjs', lib), { type: 'module' });
await context.audioWorklet.addModule(new URL('chord-processor.mjs', lib));

const node = new AudioWorkletNode(context, 'chord-processor', {
  channelCount: 1,
  channelCountMode: 'explicit',
  channelInterpretation: 'discrete',
});
worker.onmessage = ({ data }) => {
  if (data.type === 'chordResult') {
    console.log(data.result.chord, 'since', data.result.time); // time on the context.currentTime clock
  }
};
const { port1, port2 } = new MessageChannel();
worker.postMessage({ type: 'start', sampleRate: context.sampleRate, delayMs: 1000, port: port1 }, [port1]);
node.port.postMessage({ type: 'port', port: port2 }, [port2]);
context.createMediaStreamSource(stream).connect(node);
node.connect(context.destination); // it outputs silence
```

The worker posts:

- `{ type: 'ready' }` once the wasm is up; audio before that is skipped;
- `{ type: 'chordResult', result: { chord, time } }` on every chord change.
  `time` is when the chord started, in seconds on the `AudioContext` clock. It
  arrives `delayMs` plus about 0.2 s after that. Stretches of near silence
  post nothing;
- `{ type: 'error', message }` when it cannot start, as when the wasm cannot
  load;
- `{ type: 'stopped' }` after it is sent `{ type: 'stop' }` and has released
  its memory; it can then be sent another `start`.

To stop listening, send the worklet `{ type: 'stop' }`, stop or terminate the
worker (the plugin goes with it) and close the context.

`delayMs` (default 1000) is how far behind the newest audio the chord is read.
The newest frames are the least settled, so a lower value shows chords sooner
and a higher one makes them agree with what `chord-worker.mjs` finds in the
same audio: about 95% of the time at 0 ms, 99% at 1000 ms.

The worklet sends the worker every buffer gathered since the worker last
replied, so a worker that falls behind, on a slow device, reads once for
several buffers instead of lagging a little further with each one.

Up to 0.1, the worklet ran Chordino itself and took the wasm bytes in
`processorOptions.wasmBinary`; it now takes no options and posts nothing.


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

### With Vite

Vite can bundle the workers and the worklet straight from the package,
emitting `chordino.wasm` as a hashed asset, so nothing needs copying:

```ts
import ChordLiveWorker from 'chordino.js/chord-live-worker.mjs?worker';
import ChordWorker from 'chordino.js/chord-worker.mjs?worker';
import processorUrl from 'chordino.js/chord-processor.mjs?worker&url';

const worker = new ChordWorker(); // in place of new Worker(new URL('chord-worker.mjs', lib), ...)
const liveWorker = new ChordLiveWorker(); // in place of new Worker(new URL('chord-live-worker.mjs', lib), ...)
await context.audioWorklet.addModule(processorUrl);
```

The examples set `worker: { format: 'es' }` in `vite.config.ts`, which keeps
the bundled workers ES modules like the files in the package.
[examples/vite-react](examples/vite-react) is a React app built this way, with
a hook for each.

## Examples

`examples/` has three pages that run on the build, and a React app:

- `extract/`: detects the chords of an MP3 through the worker and offers them as
  JSON, the way an app would.
- `player/`: plays a recording and shows, frame by frame, the chroma, chord
  salience and semitone spectrum Chordino works from, next to the chords it
  finds.
- `realtime/`: the same from the microphone, through the production worklet
  and worker.

Build first, then serve the directory and open one of them:

```sh
python3 -m http.server -d examples
```

- `vite-react/`: a React and Vite app with a hook for whole recordings and one
  for live input. It runs on its own; [its README](examples/vite-react/README.md) has
  the steps.

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
| `src/js/` | The workers and the worklet, in TypeScript |
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
