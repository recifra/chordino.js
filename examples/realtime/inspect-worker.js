// Runs the inspect build on the microphone audio tap-processor.js hands over, the
// way chord-processor.mjs runs the production one: a realtime session, decoding
// the last WINDOW_MS on every buffer, with the chord read delayMs behind the
// newest frame. Messages to it:
//   { type: 'start', sampleRate, delayMs, port }  port: where the audio comes in
//   { type: 'stop' }
// and from it:
//   { type: 'ready', labels, templates, chromaNames, semitoneNames } on the first frames
//   { type: 'frames', settled, pending, readAt, newest, path } after every buffer:
//     settled: frames that just reached the read point, with the values the
//              worklet's decision there was made on; each only comes once
//     pending: the frames after it, whose values still move with every buffer
//     path:    the Viterbi path from the read point on, as [{ chord, start, end }]
//   { type: 'error', message }
// A frame is { time, chroma, smoothed, salience, semitones, gate }, with the
// salience summed per label (the dictionary has several templates per label).

importScripts('../lib/inspect/chordino-inspect.js');

const BUFFER_SIZE = 4096;
// As in chord-processor.mjs
const WINDOW_MS = 10000;

let instance = 0;
let pBuffer = 0;
let source = null;
let delayMs = 1000;
let bufferMs = 0;
// Time of the last frame posted as settled, as the Float32 times come out
let settledUntil = -1;
let labelOfBin = null;
let labelCount = 0;

onmessage = ({ data }) => {
  try {
    if (data.type === 'start') {
      start(data);
    } else if (data.type === 'stop') {
      stop();
    }
  } catch (err) {
    postMessage({ type: 'error', message: String(err?.message ?? err) });
  }
};

function start({ sampleRate, delayMs: delay, port }) {
  stop();
  delayMs = delay;
  bufferMs = (BUFFER_SIZE / sampleRate) * 1000;
  settledUntil = -1;
  labelOfBin = null;
  pBuffer = Module._malloc(BUFFER_SIZE * Float32Array.BYTES_PER_ELEMENT);
  instance = Module.initializePlugin(sampleRate, BUFFER_SIZE, true);
  source = port;
  source.onmessage = ({ data: buffer }) => {
    try {
      analyze(buffer);
    } catch (err) {
      postMessage({ type: 'error', message: String(err?.message ?? err) });
    }
  };
}

function stop() {
  if (source) {
    source.onmessage = null;
    source.close();
    source = null;
  }
  if (instance) {
    Module.releasePlugin(instance);
    Module._free(pBuffer);
    instance = 0;
  }
}

function analyze({ samples, startMs }) {
  if (!instance) {
    return;
  }
  HEAPF32.set(samples, pBuffer >> 2);
  Module.processBuffer(instance, pBuffer, Math.trunc(startMs));
  Module.forgetBefore(instance, Math.trunc(startMs + bufferMs - WINDOW_MS));
  const analysis = Module.getPartialAnalysis(instance, settledUntil);
  const { chords } = analysis;
  if (chords.length === 0) {
    return;
  }
  if (!labelOfBin) {
    announceLabels(analysis);
  }

  // As in chord-processor.mjs: the closing N sits on the newest frame
  const newest = chords.at(-1).time;
  const readAt = newest - delayMs / 1000;
  const settled = [];
  const pending = [];
  analysis.times.forEach((time, frame) => {
    if (time > settledUntil) {
      (time <= readAt ? settled : pending).push(frameAt(analysis, frame));
    }
  });
  if (settled.length > 0) {
    settledUntil = settled.at(-1).time;
  }
  const path = [];
  for (let i = 0; i < chords.length - 1; i++) {
    if (chords[i + 1].time > readAt) {
      path.push({ chord: chords[i].chord, start: chords[i].time, end: chords[i + 1].time });
    }
  }
  postMessage({ type: 'frames', settled, pending, readAt, newest, path });
}

function announceLabels(analysis) {
  const names = analysis.chordsalience.binNames;
  const labels = [...new Set(names)];
  const indexOf = new Map(labels.map((label, i) => [label, i]));
  labelOfBin = names.map((name) => indexOf.get(name));
  labelCount = labels.length;
  postMessage({
    type: 'ready',
    labels,
    templates: names.length,
    chromaNames: analysis.chroma.binNames,
    semitoneNames: analysis.semitonespectrum.binNames,
  });
}

function frameAt(analysis, frame) {
  const row = (bins) => bins.values.slice(frame * bins.binCount, (frame + 1) * bins.binCount);
  const templates = row(analysis.chordsalience);
  const salience = new Float32Array(labelCount);
  templates.forEach((value, bin) => {
    salience[labelOfBin[bin]] += value;
  });
  return {
    time: analysis.times[frame],
    chroma: row(analysis.chroma),
    smoothed: row(analysis.smoothedchroma),
    salience,
    semitones: row(analysis.semitonespectrum),
    gate: analysis.gate[frame],
  };
}
