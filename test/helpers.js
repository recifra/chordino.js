import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import vm from 'node:vm';

import { MPEGDecoder } from 'mpg123-decoder';

const require = createRequire(import.meta.url);

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

export const paths = {
  chordino: path.join(root, 'build/chordino.mjs'),
  wasm: path.join(root, 'build/chordino.wasm'),
  processor: path.join(root, 'build/chord-processor.mjs'),
  worker: path.join(root, 'build/chord-worker.mjs'),
  inspect: path.join(root, 'build/inspect/chordino-inspect.js'),
  chordsMp3: path.join(root, 'test/fixtures/chords.mp3'),
};

/**
 * Instantiates the production build (build/chordino.mjs, which reads
 * chordino.wasm from next to it under node). Same shape as loadScript.
 */
export async function loadChordino() {
  const { default: createChordino } = await import(pathToFileURL(paths.chordino).href);
  const Module = await createChordino();
  return { Module, heapF32: () => Module.HEAPF32 };
}

/**
 * Evaluates a classic Emscripten build (the inspect one) the way a <script> tag
 * would and hands back its Module. HEAPF32 is not exported on Module and is
 * replaced whenever ALLOW_MEMORY_GROWTH grows the heap, so it comes back as a
 * getter.
 */
export function loadScript(file) {
  const source = readFileSync(file, 'utf8');
  const wrapper = `(function (require, module, __filename, __dirname) {${source}\n` +
    'return { Module, heapF32: () => HEAPF32 };\n})';
  const factory = vm.runInThisContext(wrapper, { filename: file });
  return factory(require, { exports: {} }, file, path.dirname(file));
}

export async function decodeMp3(file) {
  const decoder = new MPEGDecoder();
  await decoder.ready;
  try {
    const { channelData, sampleRate } = decoder.decode(readFileSync(file));
    return { samples: mixDown(channelData), sampleRate };
  } finally {
    decoder.free();
  }
}

function mixDown(channels) {
  const mono = new Float32Array(channels[0].length);
  for (const channel of channels) {
    for (let i = 0; i < mono.length; i++) {
      mono[i] += channel[i] / channels.length;
    }
  }
  return mono;
}

/** Same feeding loop as src/js/chord-worker.mjs; `read` gets the instance once everything is fed. */
function feed({ Module, heapF32 }, samples, sampleRate, read, { bufferSize = 4096, instance } = {}) {
  const pBuffer = Module._malloc(bufferSize * Float32Array.BYTES_PER_ELEMENT);
  const ownInstance = instance === undefined;
  if (ownInstance) {
    instance = Module.initializePlugin(sampleRate, bufferSize);
  }
  try {
    const block = new Float32Array(bufferSize);
    for (let offset = 0; offset < samples.length; offset += bufferSize) {
      block.fill(0);
      block.set(samples.subarray(offset, offset + bufferSize));
      heapF32().set(block, pBuffer >> 2);
      Module.processBuffer(instance, pBuffer, Math.trunc((offset / sampleRate) * 1000));
    }
    return read(instance);
  } finally {
    if (ownInstance) {
      Module.releasePlugin(instance);
    }
    Module._free(pBuffer);
  }
}

export function detectChords(chordino, samples, sampleRate, options) {
  return feed(chordino, samples, sampleRate, (instance) => chordino.Module.getResult(instance), options);
}

/** Like detectChords, but through getAnalysis of the inspect build (examples/player). */
export function analyzeChords(inspect, samples, sampleRate, options) {
  return feed(inspect, samples, sampleRate, (instance) => inspect.Module.getAnalysis(instance), options);
}

const NOTE_NAMES = { C: 0, 'C#': 1, Db: 1, D: 2, 'D#': 3, Eb: 3, E: 4, F: 5, 'F#': 6, Gb: 6, G: 7, 'G#': 8, Ab: 8, A: 9, 'A#': 10, Bb: 10, B: 11 };

/** 'C3' -> 48 */
export function midi(note) {
  const [, name, octave] = note.match(/^([A-G][#b]?)(-?\d)$/);
  return NOTE_NAMES[name] + (Number(octave) + 1) * 12;
}

/**
 * Renders notes as harmonic tones (six partials with 1/h amplitude), which is
 * close enough to a real instrument for the NNLS note transcription to work on.
 */
export function synthChord(notes, seconds, sampleRate) {
  const out = new Float32Array(Math.round(seconds * sampleRate));
  for (const note of notes) {
    const frequency = 440 * 2 ** ((midi(note) - 69) / 12);
    for (let harmonic = 1; harmonic <= 6 && frequency * harmonic < sampleRate / 2; harmonic++) {
      const amplitude = 0.1 / harmonic;
      const step = (2 * Math.PI * frequency * harmonic) / sampleRate;
      for (let i = 0; i < out.length; i++) {
        out[i] += amplitude * Math.sin(step * i);
      }
    }
  }
  return out;
}

export function concat(...parts) {
  const out = new Float32Array(parts.reduce((length, part) => length + part.length, 0));
  let offset = 0;
  for (const part of parts) {
    out.set(part, offset);
    offset += part.length;
  }
  return out;
}

class FakePort {
  messages = [];
  onmessage = null;

  postMessage(data) {
    this.messages.push(data);
  }

  /** Delivers a message from the main thread, as AudioWorkletNode.port.postMessage would. */
  send(data) {
    this.onmessage?.({ data });
  }
}

/**
 * Loads the realtime worklet module with the AudioWorkletGlobalScope globals it
 * expects, and returns the registered processor class, the clock behind
 * `currentTime` and the wasm bytes a page hands it in processorOptions. The
 * globals are installed on globalThis, which is fine because node --test runs
 * every test file in its own process.
 */
export async function loadProcessor(file, sampleRate) {
  const clock = { currentTime: 0 };
  const registered = {};
  class AudioWorkletProcessor {
    constructor() {
      this.port = new FakePort();
    }
  }
  Object.assign(globalThis, {
    AudioWorkletProcessor,
    sampleRate,
    registerProcessor: (name, processorClass) => {
      registered[name] = processorClass;
    },
  });
  Object.defineProperty(globalThis, 'currentTime', { get: () => clock.currentTime, configurable: true });
  await import(pathToFileURL(file).href);
  return { registered, clock, wasmBinary: readFileSync(paths.wasm) };
}

/**
 * Loads a module worker with the worker globals it uses and returns what it posts
 * plus `send`, which delivers a message the way worker.postMessage would and
 * resolves once the handler is done. Like loadProcessor, the globals go on
 * globalThis; onmessage has to exist before the strict module assigns it.
 */
export async function loadWorker(file) {
  const messages = [];
  Object.assign(globalThis, {
    onmessage: null,
    postMessage: (data) => messages.push(data),
  });
  await import(pathToFileURL(file).href);
  return { messages, send: (data) => globalThis.onmessage({ data }) };
}

export const RENDER_QUANTUM = 128;

/** Feeds samples to a processor in render quanta, advancing the clock like the audio thread. */
export function stream(processor, clock, samples, sampleRate) {
  for (let offset = 0; offset < samples.length; offset += RENDER_QUANTUM) {
    const quantum = new Float32Array(RENDER_QUANTUM);
    quantum.set(samples.subarray(offset, offset + RENDER_QUANTUM));
    processor.process([[quantum]], [], {});
    clock.currentTime += RENDER_QUANTUM / sampleRate;
  }
}
