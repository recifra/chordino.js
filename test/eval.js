// Scores chord detection on a recording against reference chords (a JSON shaped
// like training/training.json). Not part of npm test, which has to stay
// fast and self-contained: run it by hand while tuning the detector.
//
//   node test/eval.js <audio.mp3> <expected.json> [--inspect | --realtime]
//
// --inspect runs the inspect build instead (build/inspect/chordino-inspect.js),
// which also reports how often the confidence gate rejected a frame.
//
// --realtime streams the recording through the AudioWorklet (chord-processor.mjs)
// in render quanta, the way the microphone would feed it, and scores the chords
// it posts twice: each from the time it reports for it (what it decided), and
// each from the moment it is posted (what is on screen, late by however long the
// worklet takes to make up its mind). Both are also compared with the offline
// extraction of the same audio, and it reports how long the audio thread spends
// in process().

import { readFileSync } from 'node:fs';

import { chordKey, compare, parseExpected } from '../examples/player/chords.js';
import {
  analyzeChords, decodeMp3, detectChords, loadChordino, loadProcessor, loadScript, paths, RENDER_QUANTUM,
} from './helpers.js';

// A detected change counts for an expected one when it lands in this window
// around it, so a chord missed altogether is not paired with a far-away one.
const CHANGE_WINDOW = [-1, 2.5];
const GATE_REASONS = ['passed', 'lost to N', 'root margin'];

const files = process.argv.slice(2).filter((arg) => !arg.startsWith('--'));
const inspect = process.argv.includes('--inspect');
const realtime = process.argv.includes('--realtime');
if (files.length !== 2 || (inspect && realtime)) {
  console.error('usage: node test/eval.js <audio.mp3> <expected.json> [--inspect | --realtime]');
  process.exit(1);
}
const [audioFile, expectedFile] = files;

const { samples, sampleRate } = await decodeMp3(audioFile);
const duration = samples.length / sampleRate;
const build = inspect ? loadScript(paths.inspect) : await loadChordino();
const started = performance.now();
const analysis = inspect ? analyzeChords(build, samples, sampleRate) : null;
const chords = analysis?.chords ?? detectChords(build, samples, sampleRate);
const seconds = (performance.now() - started) / 1000;

const segments = (marks) => marks.map((item, i) => ({
  chord: item.chord,
  start: item.time,
  end: i + 1 < marks.length ? marks[i + 1].time : duration,
}));
const extracted = segments(chords);
const wanted = parseExpected(JSON.parse(readFileSync(expectedFile, 'utf8')));

const percent = (value) => `${(value * 100).toFixed(1)}%`;
const time = (value) => `${value.toFixed(2)}s`;

console.log(`${audioFile}: ${time(duration)}, ${chords.length} chord marks, analyzed in ${time(seconds)}`);
if (realtime) {
  const { posted, costs } = await streamRealtime();
  console.log(`realtime: ${posted.length} chords posted`);
  // A chord can report a start before the previous one's, when the decoder moved
  // that change back after posting it; the earlier one is then cut short.
  let previous = 0;
  const marked = segments([{ chord: 'N', time: 0 }, ...posted.map(({ chord, time }) => {
    previous = Math.max(previous, time);
    return { chord, time: previous };
  })]);
  const onScreen = segments([{ chord: 'N', time: 0 }, ...posted.map(({ chord, postedAt }) => ({ chord, time: postedAt }))]);
  for (const [label, detected] of [['from the time it reports', marked], ['from when it is posted', onScreen]]) {
    console.log(`\n${label}:`);
    score(detected);
    const agreement = compare(detected, extracted, duration);
    console.log(`vs extraction  exact ${percent(agreement.exact)}  root ${percent(agreement.root)}`);
  }
  console.log();
  costs.sort((a, b) => a - b);
  const total = costs.reduce((sum, cost) => sum + cost, 0);
  console.log(`process()      max ${costs.at(-1).toFixed(1)}ms  p99.9 ${costs[Math.floor(0.999 * (costs.length - 1))].toFixed(1)}ms  ` +
    `total ${time(total / 1000)} (${percent(total / 1000 / duration)} of the audio)`);
} else {
  score(extracted);
}
if (analysis?.gate) {
  const counts = [0, 0, 0];
  for (const reason of analysis.gate) {
    counts[reason]++;
  }
  console.log(`gate           ${counts.map((count, reason) => `${GATE_REASONS[reason]} ${percent(count / analysis.gate.length)}`).join(', ')}`);
}

/** Feeds the recording to a chord-processor and collects what it posts, and when. */
async function streamRealtime() {
  const { registered, clock, wasmBinary } = await loadProcessor(paths.processor, sampleRate);
  const processor = new registered['chord-processor']({ processorOptions: { wasmBinary } });
  await processor.ready;
  const { messages } = processor.port;
  messages.length = 0;
  const posted = [];
  const costs = [];
  const quantum = new Float32Array(RENDER_QUANTUM);
  for (let offset = 0; offset < samples.length; offset += RENDER_QUANTUM) {
    quantum.fill(0);
    quantum.set(samples.subarray(offset, offset + RENDER_QUANTUM));
    const before = performance.now();
    processor.process([[quantum]], [], {});
    costs.push(performance.now() - before);
    clock.currentTime += RENDER_QUANTUM / sampleRate;
    for (const message of messages.splice(0)) {
      if (message.type === 'chordResult') {
        posted.push({ ...message.result, postedAt: clock.currentTime });
      }
    }
  }
  return { posted, costs };
}

function score(detected) {
  const { intervals, exact, root } = compare(detected, wanted, duration);

  let annotated = 0;
  let missN = 0;
  let missChord = 0;
  for (const interval of intervals) {
    const length = interval.end - interval.start;
    annotated += length;
    if (interval.result === 'miss') {
      if (interval.detected === 'N') {
        missN += length;
      } else {
        missChord += length;
      }
    }
  }

  // Latency of each expected root change: offset of the nearest detected segment
  // with that root starting inside CHANGE_WINDOW.
  const latencies = [];
  let changes = 0;
  for (let i = 1; i < wanted.length; i++) {
    const root = chordKey(wanted[i].chord).root;
    if (root === null || root === chordKey(wanted[i - 1].chord).root) {
      continue;
    }
    changes++;
    const offsets = detected
      .filter((segment) => chordKey(segment.chord).root === root)
      .map((segment) => segment.start - wanted[i].start)
      .filter((offset) => offset > CHANGE_WINDOW[0] && offset < CHANGE_WINDOW[1]);
    if (offsets.length > 0) {
      latencies.push(offsets.reduce((a, b) => (Math.abs(a) < Math.abs(b) ? a : b)));
    }
  }
  latencies.sort((a, b) => a - b);
  const quantile = (p) => latencies[Math.floor(p * (latencies.length - 1))] ?? NaN;

  console.log(`exact          ${percent(exact)}`);
  console.log(`root           ${percent(root)}`);
  console.log(`miss as N      ${percent(missN / annotated)}`);
  console.log(`miss as chord  ${percent(missChord / annotated)}`);
  console.log(`latency        p50 ${time(quantile(0.5))}  p90 ${time(quantile(0.9))}  (detected - expected)`);
  console.log(`changes found  ${latencies.length}/${changes}`);
  console.log(`summary: exact ${percent(exact)} root ${percent(root)} missN ${percent(missN / annotated)} ` +
    `latency p50 ${time(quantile(0.5))} p90 ${time(quantile(0.9))} changes ${latencies.length}/${changes}`);
}
