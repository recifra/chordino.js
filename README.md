# chordino.js

Chord detection in the browser: Chordino, the chord estimation plugin from
Matthias Mauch's NNLS Chroma Vamp plugins, ported to C and compiled to
WebAssembly, with two ways to run it:

- **Whole recordings**, in a Web Worker: hand it the samples, get back the
  chords with their start times, and progress while it works.
- **Live input**, in an AudioWorklet that hands the microphone to a Web
  Worker: get each chord change as it happens.

Plus a **tuner**: the same AudioWorklet hands the microphone to another
worker, which posts the pitch of the note being played about 23 times a
second, with YIN.

All of it shares one `chordino.wasm` and runs with no server: everything
happens in the page.

## Install

```sh
npm install chordino.js
```

The package holds these files, in `node_modules/chordino.js/build/`:

| File | Size | What it is |
| --- | --- | --- |
| `chordino.wasm` | 71 KB (37 KB gzipped) | Chordino and the tuner's YIN, with KISS FFT |
| `chordino.mjs` | 5 KB | ES module that loads the wasm; default export `createChordino()` |
| `chord-worker.mjs` | 1 KB | Module worker for whole recordings |
| `chord-live-worker.mjs` | 2 KB | Module worker that runs Chordino on live input |
| `chord-processor.mjs` | 1 KB | AudioWorklet processor that hands live input to it |
| `tuner-live-worker.mjs` | 3 KB | Module worker that reads the pitch of live input, for a tuner |
| `chordino.d.mts` | 2 KB | Types of `chordino.mjs` |

The workers and the worklet are started from a URL, not imported. Each worker
loads `chordino.mjs` from next to itself, which loads `chordino.wasm` the same
way. So an app serves those six files together, from one directory; copy them
into its static assets as part of its build:

```sh
mkdir -p public/chordino
cp node_modules/chordino.js/build/*.mjs node_modules/chordino.js/build/*.wasm public/chordino/
```

Or load them from a CDN that mirrors npm, such as
`https://cdn.jsdelivr.net/npm/chordino.js@0.3.1/build/`. A worker from another
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
// Voice processing is for speech: noise suppression takes sustained notes for
// noise and cuts them, and gain control lifts the noise floor between them
const stream = await navigator.mediaDevices.getUserMedia({
  audio: { echoCancellation: false, noiseSuppression: false, autoGainControl: false },
});
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
  arrives `delayMs` plus about 0.2 s after that. Nothing is posted where the
  sustained level, the lower quartile of the RMS of 10 ms slices within 200 ms,
  is under 0.008 (−42 dB): near silence, and sounds that do not ring on, such
  as typing, steps or knocks, however loud each is. What stays in the spectrum
  for a long while, as the hum of a speaker left on near the microphone, is
  taken off before the chords are matched, so playing over it reads as if it
  were not there; whatever sounds from the very first frames counts as such
  background until it changes, so start listening before playing;
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

The worklet gathers buffers of 4096 samples; `processorOptions.bufferSize`
sets another size, as the tuner does. Up to 0.1, it ran Chordino itself and
took the wasm bytes in `processorOptions.wasmBinary`; it posts nothing now.

### Tuner (live input)

The same worklet, in buffers of 2048 samples, feeds `tuner-live-worker.mjs`,
which reads the pitch of the newest 4096 samples with YIN (`pitch.c`, in
`chordino.wasm`):

```js
const context = new AudioContext({ latencyHint: 'playback' });
const stream = await navigator.mediaDevices.getUserMedia({
  audio: { echoCancellation: false, noiseSuppression: false, autoGainControl: false },
});
const worker = new Worker(new URL('tuner-live-worker.mjs', lib), { type: 'module' });
await context.audioWorklet.addModule(new URL('chord-processor.mjs', lib));

const node = new AudioWorkletNode(context, 'chord-processor', {
  channelCount: 1,
  channelCountMode: 'explicit',
  channelInterpretation: 'discrete',
  processorOptions: { bufferSize: 2048 },
});
worker.onmessage = ({ data }) => {
  if (data.type === 'pitch') {
    console.log(data.frequency, 'Hz at', data.time); // time on the context.currentTime clock
  }
};
const { port1, port2 } = new MessageChannel();
worker.postMessage({ type: 'start', sampleRate: context.sampleRate, port: port1 }, [port1]);
node.port.postMessage({ type: 'port', port: port2 }, [port2]);
context.createMediaStreamSource(stream).connect(node);
node.connect(context.destination); // it outputs silence
```

The worker posts `{ type: 'ready' }`, `{ type: 'error', message }` and
`{ type: 'stopped' }` as the chord worker does, and
`{ type: 'pitch', frequency, time }` for every steady reading: `frequency` in
Hz, `time` at the end of the window read, in seconds on the `AudioContext`
clock. The note and how many cents off it is are left to the page.

- It reads once per batch of buffers, on the newest window: a reading every
  2048 samples, the windows overlapping by half, while it keeps up, and only
  the newest when it falls behind.
- Nothing is posted while the input is near-silent (RMS under 0.01), nor until
  3 readings in a row agree within 20 cents. What is posted is the median of
  the last 5 that did, so a lone window that lost samples, and reads sharp,
  stays off it.
- It reaches A0 (27.5 Hz) to C8 (4186 Hz) at 48 kHz, C8 about 5 cents sharp.
  It listens to everything: to keep one instrument's band, put biquad filters
  between the microphone and the worklet (the tuner example has some).
- With `ignoreBackground: true` in `start`, it also skips what is not clearly
  louder than the background: a steady hum, as from a speaker left on near the
  microphone, is periodic enough to read as a note otherwise. The background
  follows the quietest windows, dropping at once and rising 1 dB a second, so
  a held note is read for a long while; a window has to be 6 dB over it.
- With `clockReports: true` in `start`, it also posts `{ type: 'clock', ratio }`
  about every 5 s: the `AudioContext` time the buffers covered over the time
  they took to come. Well under 1, the audio thread is falling behind, the
  case in which samples go missing and pitch reads sharp.


### The module itself

`chordino.mjs` is the package's main entry, for use in Node or in a worker of
your own:

```js
import createChordino from 'chordino.js';

const Module = await createChordino(); // loads chordino.wasm from next to chordino.mjs
const instance = Module.initializePlugin(sampleRate, 4096);
```

The module has `initializePlugin`, `processBuffer`, `getResult`,
`getPartialResult`, `forgetBefore` and a few more, and the tuner's
`createPitch`, `detectPitch`, `pitchRms` and `releasePitch`; [BUILD.md](BUILD.md)
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
import TunerLiveWorker from 'chordino.js/tuner-live-worker.mjs?worker';

const worker = new ChordWorker(); // in place of new Worker(new URL('chord-worker.mjs', lib), ...)
const liveWorker = new ChordLiveWorker(); // in place of new Worker(new URL('chord-live-worker.mjs', lib), ...)
const tunerWorker = new TunerLiveWorker(); // in place of new Worker(new URL('tuner-live-worker.mjs', lib), ...)
await context.audioWorklet.addModule(processorUrl);
```

The examples set `worker: { format: 'es' }` in `vite.config.ts`, which keeps
the bundled workers ES modules like the files in the package.
[examples/vite-react](examples/vite-react) is a React app built this way, with
a hook for each.

## Examples

`examples/` has four pages that run on the build, and a React app:

- `extract/`: detects the chords of an MP3 through the worker and offers them as
  JSON, the way an app would.
- `player/`: plays a recording and shows, frame by frame, the chroma, chord
  salience and semitone spectrum Chordino works from, next to the chords it
  finds.
- `realtime/`: the same from the microphone, through the production worklet
  and worker, with the input and sustained levels against the silence gate,
  and the audio heard to download as a WAV, to analyze it again elsewhere.
- `tuner/`: a tuner on the microphone, through the worklet and the tuner
  worker, with the clock ratio and a reference tone to read back.

Build first, then serve the directory and open one of them:

```sh
python3 -m http.server -d examples
```

From a phone or tablet, the microphone needs https (browsers only give it to
https and localhost pages). `npm run examples` serves the same files over
https on port 8000 (`PORT=` picks another), with a self-signed certificate it
makes on the first run (in `.cert/`), and prints the addresses to open; each
device asks once to go on past the warning.

- `vite-react/`: a React and Vite app with a hook for whole recordings and one
  for live input. It runs on its own; [its README](examples/vite-react/README.md) has
  the steps.

## Building and testing

From a clone of this repository. You need emsdk and Node.js:

```sh
npm install
npm run build  # or npm run build:js, for the TypeScript alone
npm test
```

[BUILD.md](BUILD.md) has the details.

## Layout

| Path | What it holds |
| --- | --- |
| `src/nnls-chroma/` | The NNLS Chroma / Chordino plugin source, adapted for this build |
| `src/chordino.h` | `chordino_t`: Chordino, as functions instead of a Vamp plugin |
| `src/pitch.c`, `src/pitch.h` | `pitch_t`: the tuner's YIN, one window at a time |
| `src/chordextract.c` | The C functions `chordino.mjs` calls |
| `src/chordextract.js` | The API `chordino.mjs` exposes over them |
| `src/session.c`, `src/session.h` | `session_t`: what feeds Chordino, buffering and frame times |
| `src/spectrum.c`, `src/spectrum.h` | `spectrum_t`: the spectra Chordino takes, windowed, through KISS FFT |
| `src/window.c`, `src/window.h` | `window_t`: the Hann window, as Vamp's `Window` |
| `src/realtime.c`, `src/realtime.h` | `realtime_t`: times in seconds and nanoseconds, as Vamp's `RealTime` |
| `src/vector.c`, `src/vector.h` | `vector_t`: a growing array |
| `src/kissfft/` | KISS FFT 131.2.0 |
| `src/inspect.c`, `src/inspect.js` | The extra functions of the inspect build |
| `src/js/` | The workers and the worklet, in TypeScript |
| `src/build.sh` | The `emcc` commands |
| `examples/` | The example pages |
| `test/` | Tests, run with `node --test` against `build/` |

## Credits and license

NNLS Chroma and Chordino are by Matthias Mauch, Centre for Digital Music, Queen
Mary University of London, and are licensed under the GNU General Public License,
version 2 or later (see [src/nnls-chroma/COPYING](src/nnls-chroma/COPYING)). The
modules in `build/` are compiled from that code, so chordino.js is distributed
under the same license (see [LICENSE](LICENSE)). `src/session.c`,
`src/spectrum.c`, `src/window.c` and `src/realtime.c` are adapted from the Vamp
plugin SDK, by Chris Cannam and Queen Mary University of London, under a
BSD-style license (see [src/vamp-sdk-COPYING](src/vamp-sdk-COPYING), which the
package includes).
KISS FFT is by Mark Borgerding, under the BSD 3-clause license (see
[src/kissfft/COPYING](src/kissfft/COPYING), which the package includes).

If you use Chordino in research, please cite:

> Matthias Mauch and Simon Dixon. *Approximate Note Transcription for the
> Improved Identification of Difficult Chords.* In Proceedings of the 11th
> International Society for Music Information Retrieval Conference (ISMIR
> 2010), 2010.
