# Plan: a tuner in chordino.js

Live pitch detection next to live chord detection, so that apps such as
Recifra (app-recifra) take both from this package instead of carrying their
own worklet, worker and detector.

## Status (2026-10-19)

Steps 1 to 5 are done: `src/pitch.c`/`.h` and its exports,
`processorOptions.bufferSize` in the worklet, `tuner-live-worker.mjs` with the
clock report, `test/tuner.test.js` and `examples/tuner/`. The page was checked
in desktop Chrome with an oscillator standing in for the microphone (in tune,
19.6 cents sharp at 445 Hz, E1, silence, clock ratio 1.000); it still has to
be tried on the tablet that stutters. Step 6 (README, BUILD.md, 0.3.0) is
still to do.

From 0.2.1, `build/chordino.wasm` went from 69,712 to 72,648 bytes (+2.9 KB)
and `chordino.mjs` from 5,138 to 5,307. `detectPitch` takes 0.061 ms per
4096-sample window under node on a desktop, against 0.41 ms for app-recifra's
TypeScript, and settles on the lag of a direct-sum YIN to within 1e-10.

The detector to port is app-recifra's as of its commit 351be3c9 ("lighten the
tuner"), the one that brought the FFT difference function and the median; the
constants below were checked against it.

Nothing already here does the job. The closest is Chordino's semitone spectrum
(84 NNLS activations, A0 upwards, three log-frequency bins per semitone) and
its tuning estimate, but they come from 16384-sample blocks, the tuning is one
offset averaged over everything fed, not the pitch of the note being played,
and the spectrum only leaves the wasm in the inspect build. A tuner needs a
cents-accurate fundamental every few tens of milliseconds, which is YIN's job.

## Where it comes from

app-recifra's tuner has the same shape as the live chords here: an AudioWorklet
that only copies the microphone (`src/worklets/tuner-processor.ts`) and a module
worker running YIN on it (`src/workers/tuner-pitch.ts`,
`src/utils/pitch-detector.ts`). The detector there:

- 4096-sample window, 50% overlap (a reading every 2048 samples, ~23/s at
  48 kHz), enough for A0 (27.5 Hz, tau up to ~1750);
- YIN with threshold 0.1 and parabolic interpolation; the difference function
  comes from an FFT cross-correlation, `e(0) + e(tau) - 2 r(tau)` (0.41 ms per
  window on a desktop, against 3.7 ms for the direct sum);
- RMS gate at 0.01, under which the stability count starts over;
- stability filter: a frequency is reported once 3 readings in a row agree
  within 20 cents;
- what is reported is the median of the last 5 stable readings; a stable
  reading 20 cents or more from that median starts a new note.

On an Android tablet the tuner still stutters while the app plays a reference
note through spessasynth (in another AudioContext), and readings came out up to
~30 cents sharp before a reboot: the same symptom as the iOS sample drops this
package's worklet was split for.

## What this does and does not fix

What moving the tuner here gives:

- one microphone pipeline for chords and pitch, tested in one place;
- the worklet's backpressure (`chord-processor.ts` batches what piles up while
  the worker is busy), which app-recifra's worklet lacks: a worker that falls
  behind analyses only the newest window instead of every one it missed, so
  load drops exactly when the device is struggling;
- the detector in plain C, which a native shell can compile as is (see the
  last section);
- a clock diagnostic that tells whether samples are being lost.

What it does not do by itself: if the stutter comes from the audio threads
themselves (the synth and the microphone fighting for the CPU), a faster
detector in a worker changes little. JS already takes 0.41 ms per window, and
the wasm takes 0.061 ms (about 7 times faster, on a desktop). The diagnostic below is what tells
the two cases apart.

## Design

### C: `src/pitch.c`, `src/pitch.h`

`pitch_t`, the per-window YIN, stateless between calls (a plain struct, as
`spectrum_t`; its buffers are the only thing it keeps):

```c
// A detector for windows of size samples (4096 for the piano's full range);
// NULL unless size is even
pitch_t *pitch_create(float sampleRate, int size);
void pitch_destroy(pitch_t *pitch);
// RMS of the size samples at block
double pitch_rms(const pitch_t *pitch, const float *block);
// Fundamental of the size samples at block, in Hz; 0 when YIN finds none
double pitch_detect(pitch_t *pitch, const float *block);
```

- Difference function by cross-correlation over `kiss_fftr`/`kiss_fftri`
  (size `size`): the first half zero-padded against the whole window, so
  `tau < size / 2` never wraps; the energy of the shifted window is slid along.
  Clamp at 0, since rounding can take it just under.
- CMND, the first local minimum under 0.1, parabolic interpolation: as in
  app-recifra's `pitch-detector.ts`.
- `kiss_fft_scalar` is double for the whole build (`src/build.sh`), which
  suits this too.
- No `printf` (see BUILD.md on the wasm size).

The stateful part (overlap, RMS gate, stability, median) goes in the worker,
as `Session` does for the chords. That keeps the C a pure function of one
window, the part worth sharing with a native build.

### Export layer

Keep the three in step, as BUILD.md asks:

- `src/chordextract.c`: `EMSCRIPTEN_KEEPALIVE` wrappers `createPitch`,
  `releasePitch`, `detectPitch`, `pitchRms`;
- `src/chordextract.js`: `Module['createPitch']`, and the others;
- `src/js/chordino.d.mts`: their types.

`src/build.sh`: add `pitch.c` to `SOURCES` (both builds).

### Worklet: `chord-processor.ts`, reused

It already does what the tuner needs. Add one option:

- `processorOptions.bufferSize`, default 4096 (unchanged for the chords). The
  tuner passes 2048, the hop, so readings come every 2048 samples as today.
  `MAX_PENDING` stays a count of buffers (about 5.5 s at 2048 and 48 kHz);
  the tuner worker only uses the newest two anyway.

Keep the registered name `chord-processor`, and document it as the
microphone feed of both workers. Renaming it would break current users.

### Worker: `src/js/tuner-live-worker.ts` → `build/tuner-live-worker.mjs`

Same protocol as `chord-live-worker.ts`:

- in: `{ type: 'start', sampleRate, port, clockReports? }`, `{ type: 'stop' }`;
- out: `{ type: 'ready' }`, `{ type: 'pitch', frequency, time }` (`time` in
  seconds on the AudioContext clock, at the end of the window read),
  `{ type: 'error', message }`, `{ type: 'stopped' }`;
- answers `{ type: 'idle' }` on the port after every batch, from the start
  (before the wasm is up, the audio is skipped), so the worklet keeps
  batching.

Per batch:

1. Append every block to a 4096-sample sliding window.
2. Analyse **once**, on the newest window, however many blocks came in. With
   `bufferSize` 2048 and the worker keeping up, that gives the 50% overlap.
   When the worker falls behind, the windows it missed are dropped and the
   reading stays current.
3. RMS under 0.01: reset the stability count and the median, post nothing.
4. `pitch_detect`, then stability (3 within 20 cents) and median (last 5,
   restart 20 cents away), then post.

Constants as in app-recifra. Expose none as options until an app needs one;
the band per instrument stays with the app (biquad filters before the
worklet).

### Clock diagnostic (`clockReports: true`)

Every ~5 s, the worker posts `{ type: 'clock', ratio }`. `ratio` is the
AudioContext time covered by the blocks (from `startMs`/`nowMs`) over the
`performance.now()` time it took to receive them, measured over the whole
interval, so the batching jitter averages out.

- `ratio` well under 1: the audio thread is falling behind, the case where
  samples get spliced and pitch reads sharp. Moving work off the audio threads
  is the fix (fewer AudioContexts, larger buffers, or native capture).
- `ratio` ~1 while readings stay sharp: the samples are lost before the
  AudioContext, in the capture itself, and nothing in the WebView fixes that.
  Go native.

This is a lead, not a proof: how each browser advances `currentTime` when it
overruns differs. Check it on the tablet first, against a stutter you can
hear.

### Example: `examples/tuner/`

A page of its own, `index.html` and `tuner.js`, with the tuner only. It uses
the production files from `../lib/` (the `build/` symlink) the way an app
does: `chord-processor.mjs` with `processorOptions: { bufferSize: 2048 }`,
`tuner-live-worker.mjs` with `clockReports: true`, nothing from the inspect
build. Styles from `../player/style.css`, as `realtime/` does.

It shows:

- the nearest note with its octave (A4), large, and how far off it is in
  cents (-50 to +50), as a needle or a bar, plus the frequency to 0.1 Hz.
  The note and cents are worked out in the page from A4 = 440 Hz; the worker
  posts only the frequency;
- the reading greyed out once no `pitch` has come for about a second (under
  the RMS gate the worker posts nothing);
- a strip of the last 10 s of readings, cents against the nearest note, so a
  jump sharp shows when it happened;
- the clock ratio, updated every report.

Controls:

- start/stop;
- band: off, or the highpass/lowpass pair app-recifra sets per instrument, as
  biquads between the microphone and the worklet;
- a reference tone (an oscillator at the nearest note) in a second
  AudioContext, to load the device as app-recifra's reference note does, and
  check whether the readings or the ratio change while it plays.

It has to fit a phone or tablet held upright. The microphone needs a secure
context, which `http://<computer's IP>:8000` is not (the page says so instead
of failing on `navigator.mediaDevices`). On the Android tablet, either connect
it by USB and open `http://localhost:8000/tuner/` after
`adb reverse tcp:8000 tcp:8000`, or add `http://<computer's IP>:8000` to
`chrome://flags/#unsafely-treat-insecure-origin-as-secure` and relaunch
Chrome.

## Steps

1. `src/pitch.c`/`.h` and the export layer; `npm run build:wasm`. Note the
   wasm size before and after in the PR, which should only grow by a few KB.
2. `processorOptions.bufferSize` in `chord-processor.ts`.
3. `src/js/tuner-live-worker.ts`. Add it to the esbuild entry points of
   `build:js` and to `exports` and `files` in `package.json`.
4. Tests (below); `npm test`.
5. `examples/tuner/` (above); try it on the desktop and on the tablet that
   stutters.
6. README (a "Tuner (live input)" section next to "Live input", `tuner/` in
   the Examples list), BUILD.md (the file list, the C API, `/tuner/` in
   Examples), the Layout table; version 0.3.0.

## Tests (`test/tuner.test.js`, `node --test` against `build/`)

All of the below are in, plus silence, `pitchRms` and odd sizes on
`detectPitch` itself, a `bufferSize` that is not a multiple of the render
quantum, and no `clock` without `clockReports`.

From app-recifra's `src/utils/pitch-detector.test.ts`, ported:

- tones with 4 partials at 82.41 Hz/44.1 kHz, 440 and 1046.5 Hz/48 kHz: null
  for the first readings, then within 5 cents;
- under the RMS gate nothing is posted, and after it the 3 readings are
  waited for again;
- a short burst ~15 cents sharp inside a held note stays off the reading
  (the median).

New:

- range: 27.5 Hz to 4186 Hz (C8) within 10 cents. C8 reads about 5 cents
  sharp at 48 kHz, its period being 11.5 samples, short for the parabola;
- the C against a direct-sum YIN on random windows: same tau to 1e-6;
- backpressure: a batch of 5 blocks produces one analysis, on the newest
  window;
- `bufferSize` 2048 in the worklet: blocks of 2048, the 4096 default
  unchanged (the existing realtime tests keep passing);
- protocol: `ready`, `pitch`, `stopped`, and `idle` answered before `ready`;
- `clock`: with the fake clock of `helpers.js` running at 0.98 of
  `performance.now()`, the ratio reported is within 0.005 of 0.98.

Extend `helpers.js`'s `loadLive` to load the tuner worker as it loads
`chord-live-worker.mjs`.

## Migration in app-recifra (after 0.3.0)

- `chordino.js` `^0.3.0`.
- `src/contexts/tuner-context.tsx`: import
  `chordino.js/chord-processor.mjs?worker&url` (as
  `chord-detector-context.tsx` does) and
  `chordino.js/tuner-live-worker.mjs?worker&url`, the worker started through
  `createModuleWorker` (a blob of the page, for the CDN); the node with
  `processorOptions: { bufferSize: 2048 }`; `pitch` → `handlePitch`, `error` →
  `setErrored`. Keep the biquad band and shelves, `latencyHint: "playback"`
  and the 0.5-cent dedupe in `handlePitch`.
- Delete `src/worklets/tuner-processor.ts`, `src/workers/tuner-pitch.ts`,
  `src/utils/pitch-detector.ts` and its test (now covered here).
  `src/utils/worker.test.ts` only uses a `tuner-pitch-*.js` name as a URL
  fixture, so it can stay.
- `vite.config.ts`: fix the comment that names the tuner worklet. The output
  already sends workers and `.wasm` to `js/`, which the mobile shell packages.
- Verify in the mobile build that both load from `js/`, with `clockReports`
  on, on the tablet that stutters.

## If it still stutters: native

The decision rests on the clock diagnostic above. If the capture loses
samples, or the WebView's audio threads cannot keep up, move the tuner's
capture and detection out of the WebView. In mobile-recifra, record natively
(AudioRecord/Oboe on Android, AVAudioEngine on iOS) and run `pitch.c` through
`dart:ffi`; it has no emscripten dependencies by design. Post readings to the
SPA over the bridge, so the WebView does no audio for the tuner at all. That
is a separate plan, in mobile-recifra.

## Later: several notes at once

Live detection of several notes at once (what is sounding, not only the
fundamental of one note) comes after the tuner, in a plan of its own. YIN
cannot do it: it finds one period. It will reuse what this plan builds (the
worklet with `bufferSize`, the worker's backpressure and clock report, a
page in `examples/`), and could start from the 84 semitone activations
Chordino already works out per frame, which today leave the wasm only in the
inspect build and come from 16384-sample blocks, too slow as they are.

## Open decisions

- C or TypeScript for the detector. TS is already fast enough (0.41 ms). C
  is chosen for the native path, and because the library's core is C.
- One worklet name for both workers (above), or a `mic-processor` alias
  registered alongside it.
