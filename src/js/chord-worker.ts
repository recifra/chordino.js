// Detects the chords of a whole recording off the main thread, reporting progress
// as it goes. A module worker; it loads chordino.wasm from next to chordino.mjs:
//
//   const worker = new Worker(new URL('chord-worker.mjs', lib), { type: 'module' });
//   worker.postMessage({ samples, sampleRate }, [samples.buffer]); // mono Float32Array
//
// (workers have to come from the page's origin; see examples/extract/index.html
// for scripts on a CDN) and gets back, in order:
//   { type: 'progress', value }  value rising from 0 to 1, once per whole percent
//   { type: 'done', result }     the chord JSON examples/player loads as expected,
//                                with the beats and the tempo:
//                                { timestamps: [[chord, seconds], ..., ['', end]],
//                                  beats: [[seconds, position], ...],
//                                  tempo: { bpm, meter: [numerator, denominator] } }
//   { type: 'error', message }   instead of done when the analysis failed

import createChordino, { type Beats, type Chord, type ChordinoModule } from './chordino.mjs';

interface ChordTimestamps {
  timestamps: [chord: string, seconds: number][];
}

interface BeatTimes {
  /** Every beat, with where it falls in its bar: 1 for the downbeat */
  beats: [seconds: number, position: number][];
  /** BPM of the note the meter counts (the eighth in 6/8); null with no beats */
  tempo: { bpm: number; meter: [numerator: number, denominator: number] } | null;
}

type WorkerMessage =
  | { type: 'progress'; value: number }
  | { type: 'done'; result: ChordTimestamps & BeatTimes }
  | { type: 'error'; message: string };

const BUFFER_SIZE = 4096;
// Share of the time spent feeding the audio in; getResult() takes the rest and
// reports its own progress (measured on a 5 minute song).
const FEED_SHARE = 0.3;

let chordino: Promise<ChordinoModule> | null = null;
let reportedPercent = -1;

onmessage = async ({ data }: MessageEvent) => {
  try {
    chordino ??= createChordino().catch((err: unknown) => {
      chordino = null;
      throw err;
    });
    const Module = await chordino;
    post({ type: 'done', result: detect(Module, data?.samples, data?.sampleRate) });
  } catch (err) {
    post({ type: 'error', message: String((err as Error | null)?.message ?? err) });
  }
};

function post(message: WorkerMessage) {
  postMessage(message);
}

function detect(Module: ChordinoModule, samples: unknown, sampleRate: unknown): ChordTimestamps & BeatTimes {
  if (!(samples instanceof Float32Array) || typeof sampleRate !== 'number' || !(sampleRate > 0)) {
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
    // Chordino does the NNLS, chord matching and Viterbi decoding all in here,
    // and then the beats are found with the chords
    const chords = Module.getResult(instance);
    const beats = Module.getBeats(instance);
    progress(1);
    return { ...toTimestamps(chords, samples.length / sampleRate), ...toBeatTimes(beats) };
  } finally {
    Module.setProgressCallback(null);
    Module.releasePlugin(instance);
    Module._free(pBuffer);
  }
}

function progress(value: number) {
  const percent = Math.floor(value * 100);
  if (percent !== reportedPercent) {
    reportedPercent = percent;
    post({ type: 'progress', value });
  }
}

const round = (seconds: number) => Math.round(seconds * 1000) / 1000;

/**
 * getResult()'s [{ chord, time }] as { timestamps: [[chord, seconds], ...] }, to
 * the millisecond: N becomes '' (no chord) and a leading one is dropped, and the N
 * Chordino closes with at the last frame becomes '' at the end of the audio,
 * unless the song already ends in no chord.
 */
function toTimestamps(chords: Chord[], duration: number): ChordTimestamps {
  const timestamps: ChordTimestamps['timestamps'] = [];
  for (const { chord, time } of chords.slice(0, -1)) {
    const label = chord === 'N' ? '' : chord;
    if (label !== (timestamps.at(-1)?.[0] ?? '')) {
      timestamps.push([label, round(time)]);
    }
  }
  if (timestamps.at(-1)?.[0] !== '') {
    timestamps.push(['', round(duration)]);
  }
  return { timestamps };
}

/** getBeats()'s beats as [[seconds, position], ...], to the millisecond, and the whole BPM */
function toBeatTimes(beats: Beats | null): BeatTimes {
  if (!beats) {
    return { beats: [], tempo: null };
  }
  return {
    beats: beats.beats.map(({ time, position }) => [round(time), position]),
    tempo: { bpm: Math.round(beats.bpm), meter: [beats.numerator, beats.denominator] },
  };
}
