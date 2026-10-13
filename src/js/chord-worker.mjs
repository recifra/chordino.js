// Detects the chords of a whole recording off the main thread, reporting progress
// as it goes. A module worker; it loads chordino.wasm from next to chordino.mjs:
//
//   const worker = new Worker(new URL('chord-worker.mjs', lib), { type: 'module' });
//   worker.postMessage({ samples, sampleRate }, [samples.buffer]); // mono Float32Array
//
// (workers have to come from the page's origin; see examples/extract/index.html
// for scripts on a CDN) and gets back, in order:
//   { type: 'progress', value }  value rising from 0 to 1, once per whole percent
//   { type: 'done', result }     the chord JSON examples/player loads as expected:
//                                { timestamps: [[chord, seconds], ..., ['', end]] }
//   { type: 'error', message }   instead of done when the analysis failed

import createChordino from './chordino.mjs';

const BUFFER_SIZE = 4096;
// Share of the time spent feeding the audio in; getResult() takes the rest and
// reports its own progress (measured on a 5 minute song).
const FEED_SHARE = 0.25;

let chordino = null;
let reportedPercent = -1;

onmessage = async ({ data }) => {
  try {
    chordino ??= createChordino().catch((err) => {
      chordino = null;
      throw err;
    });
    const Module = await chordino;
    postMessage({ type: 'done', result: detect(Module, data?.samples, data?.sampleRate) });
  } catch (err) {
    postMessage({ type: 'error', message: String(err?.message ?? err) });
  }
};

function detect(Module, samples, sampleRate) {
  if (!(samples instanceof Float32Array) || !(sampleRate > 0)) {
    throw new Error('expected { samples: Float32Array, sampleRate }');
  }
  reportedPercent = -1;
  progress(0);
  const pBuffer = Module._malloc(BUFFER_SIZE * Float32Array.BYTES_PER_ELEMENT);
  const instance = Module.initializePlugin(sampleRate, BUFFER_SIZE);
  Module.setProgressCallback((done) => progress(FEED_SHARE + (1 - FEED_SHARE) * done));
  try {
    const block = new Float32Array(BUFFER_SIZE);
    for (let offset = 0; offset < samples.length; offset += BUFFER_SIZE) {
      block.fill(0);
      block.set(samples.subarray(offset, offset + BUFFER_SIZE));
      Module.HEAPF32.set(block, pBuffer >> 2);
      Module.processBuffer(instance, pBuffer, Math.trunc((offset / sampleRate) * 1000));
      progress(FEED_SHARE * Math.min((offset + BUFFER_SIZE) / samples.length, 1));
    }
    // Chordino does the NNLS, chord matching and Viterbi decoding all in here
    const chords = Module.getResult(instance);
    progress(1);
    return toTimestamps(chords, samples.length / sampleRate);
  } finally {
    Module.setProgressCallback(null);
    Module.releasePlugin(instance);
    Module._free(pBuffer);
  }
}

function progress(value) {
  const percent = Math.floor(value * 100);
  if (percent !== reportedPercent) {
    reportedPercent = percent;
    postMessage({ type: 'progress', value });
  }
}

/**
 * getResult()'s [{ chord, time }] as { timestamps: [[chord, seconds], ...] }, to
 * the millisecond: N becomes '' (no chord) and a leading one is dropped, and the N
 * Chordino closes with at the last frame becomes '' at the end of the audio,
 * unless the song already ends in no chord.
 */
function toTimestamps(chords, duration) {
  const round = (seconds) => Math.round(seconds * 1000) / 1000;
  const timestamps = [];
  for (const { chord, time } of chords.slice(0, -1)) {
    const label = chord === 'N' ? '' : chord;
    if (label !== (timestamps.at(-1)?.[0] ?? '')) {
      timestamps.push([label, round(time)]);
    }
  }
  if (timestamps.length === 0 || timestamps.at(-1)[0] !== '') {
    timestamps.push(['', round(duration)]);
  }
  return { timestamps };
}
