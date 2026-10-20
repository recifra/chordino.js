// Scores the beats found in a recording against reference beats: a text file
// with a beat per line, its time in seconds and its position in the bar (1 for
// the downbeat), as the Ballroom and GTZAN beat annotations are. Or, with a
// chord JSON shaped like training/training.json instead, how many of its chord
// changes land on a beat, and on which. Not part of npm test: run it by hand
// while tuning src/rhythm.c.
//
//   node test/eval-beats.js <audio.mp3> [<reference.beats> | <chords.json>]
//
// Beats count as found within 70 ms of a reference one, the tolerance of the
// MIREX beat tracking evaluation (which also leaves out the first 5 s).

import { readFileSync } from 'node:fs';

import { decodeMp3, feed, loadChordino } from './helpers.js';

const TOLERANCE = 0.07;
const SKIP = 5;

const [audioFile, referenceFile] = process.argv.slice(2);
if (!audioFile) {
  console.error('usage: node test/eval-beats.js <audio.mp3> [<reference.beats> | <chords.json>]');
  process.exit(1);
}

const { samples, sampleRate } = await decodeMp3(audioFile);
const chordino = await loadChordino();
const started = performance.now();
const result = feed(chordino, samples, sampleRate, (instance) => {
  chordino.Module.getResult(instance);
  return chordino.Module.getBeats(instance);
});
const seconds = (performance.now() - started) / 1000;

const duration = samples.length / sampleRate;
const percent = (value) => `${(value * 100).toFixed(1)}%`;
console.log(`${audioFile}: ${duration.toFixed(2)}s, chords and beats in ${seconds.toFixed(2)}s`);
if (!result) {
  console.log('no beats');
  process.exit(0);
}
const { bpm, numerator, denominator, beats } = result;
console.log(`${bpm.toFixed(1)} BPM in ${numerator}/${denominator}, ${beats.length} beats`);
if (!referenceFile) {
  process.exit(0);
}

const text = readFileSync(referenceFile, 'utf8');
if (text.trimStart().startsWith('{')) {
  scoreChanges(JSON.parse(text).timestamps);
} else {
  scoreBeats(text.split('\n').filter((line) => line.trim()).map((line) => line.trim().split(/\s+/).map(Number)));
}

/** F-measure of found against wanted times, each matched once, within TOLERANCE */
function fMeasure(found, wanted) {
  found = found.filter((time) => time >= SKIP);
  wanted = wanted.filter((time) => time >= SKIP);
  if (found.length === 0 || wanted.length === 0) {
    return 0;
  }
  const used = new Set();
  let hits = 0;
  for (const time of found) {
    let best = -1;
    for (let i = 0; i < wanted.length; i++) {
      if (!used.has(i) && Math.abs(wanted[i] - time) <= TOLERANCE && (best < 0 || Math.abs(wanted[i] - time) < Math.abs(wanted[best] - time))) {
        best = i;
      }
    }
    if (best >= 0) {
      used.add(best);
      hits++;
    }
  }
  const precision = hits / found.length;
  const recall = hits / wanted.length;
  return hits === 0 ? 0 : (2 * precision * recall) / (precision + recall);
}

function median(values) {
  const sorted = [...values].sort((a, b) => a - b);
  return sorted[Math.floor(sorted.length / 2)];
}

function scoreBeats(reference) {
  // A compound meter's thirds are counted too, as its beats
  const times = beats.map(({ time }) => time);
  const wanted = reference.map(([time]) => time);
  const intervals = (list) => list.slice(1).map((time, i) => time - list[i]);
  const wantedBpm = 60 / median(intervals(wanted));
  const downbeats = (list, positions) => list.filter((_, i) => positions[i] === 1);
  console.log(`reference      ${wantedBpm.toFixed(1)} BPM, ${Math.max(...reference.map(([, position]) => position))} a bar`);
  console.log(`beats          F ${percent(fMeasure(times, wanted))}`);
  console.log(`downbeats      F ${percent(fMeasure(downbeats(times, beats.map(({ position }) => position)),
    downbeats(wanted, reference.map(([, position]) => position))))}`);
}

function scoreChanges(timestamps) {
  const changes = timestamps.map(([, time]) => time).filter((time) => time > 0 && time < duration - 1);
  const interval = 60 / bpm;
  let near = 0;
  const positions = new Map();
  for (const time of changes) {
    const closest = beats.reduce((best, beat) => (Math.abs(beat.time - time) < Math.abs(best.time - time) ? beat : best));
    if (Math.abs(closest.time - time) < 0.2 * interval) {
      near++;
    }
    positions.set(closest.position, (positions.get(closest.position) ?? 0) + 1);
  }
  console.log(`chord changes  ${changes.length}, within a fifth of a beat of one ${percent(near / changes.length)}`);
  console.log(`on positions   ${[...positions].sort(([a], [b]) => a - b).map(([position, count]) => `${position}: ${percent(count / changes.length)}`).join('  ')}`);
}
