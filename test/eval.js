// Scores chord detection on a recording against reference chords (a JSON shaped
// like training/training.json). Not part of npm test, which has to stay
// fast and self-contained: run it by hand while tuning the detector.
//
//   node test/eval.js <audio.mp3> <expected.json> [--inspect]
//
// --inspect runs the inspect build instead (build/inspect/chordino-inspect.js),
// which also reports how often the confidence gate rejected a frame.

import { readFileSync } from 'node:fs';

import { chordKey, compare, parseExpected } from '../examples/player/chords.js';
import { analyzeChords, decodeMp3, detectChords, loadChordino, loadScript, paths } from './helpers.js';

// A detected change counts for an expected one when it lands in this window
// around it, so a chord missed altogether is not paired with a far-away one.
const CHANGE_WINDOW = [-1, 2.5];
const GATE_REASONS = ['passed', 'lost to N', 'root margin'];

const files = process.argv.slice(2).filter((arg) => !arg.startsWith('--'));
const inspect = process.argv.includes('--inspect');
if (files.length !== 2) {
  console.error('usage: node test/eval.js <audio.mp3> <expected.json> [--inspect]');
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

const detected = chords.map((item, i) => ({
  chord: item.chord,
  start: item.time,
  end: i + 1 < chords.length ? chords[i + 1].time : duration,
}));
const wanted = parseExpected(JSON.parse(readFileSync(expectedFile, 'utf8')));
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

const percent = (value) => `${(value * 100).toFixed(1)}%`;
const time = (value) => `${value.toFixed(2)}s`;

console.log(`${audioFile}: ${time(duration)}, ${chords.length} chord marks, analyzed in ${time(seconds)}`);
console.log(`exact          ${percent(exact)}`);
console.log(`root           ${percent(root)}`);
console.log(`miss as N      ${percent(missN / annotated)}`);
console.log(`miss as chord  ${percent(missChord / annotated)}`);
console.log(`latency        p50 ${time(quantile(0.5))}  p90 ${time(quantile(0.9))}  (detected - expected)`);
console.log(`changes found  ${latencies.length}/${changes}`);
if (analysis?.gate) {
  const counts = [0, 0, 0];
  for (const reason of analysis.gate) {
    counts[reason]++;
  }
  console.log(`gate           ${counts.map((count, reason) => `${GATE_REASONS[reason]} ${percent(count / analysis.gate.length)}`).join(', ')}`);
}
console.log(`summary: exact ${percent(exact)} root ${percent(root)} missN ${percent(missN / annotated)} ` +
  `latency p50 ${time(quantile(0.5))} p90 ${time(quantile(0.9))} changes ${latencies.length}/${changes}`);
