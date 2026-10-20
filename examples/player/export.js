// Exports one analysis as JSON laid out for an LLM to read: a readme first, chords
// as time ranges, then per-segment and per-window records (one per line) with
// the chord probabilities and the chroma by note name, sparse and rounded so
// the file stays small and needs no index bookkeeping.

import { chordKey, segmentAt } from './chords.js';

const TOP_CHORDS = 5;
const MAX_CONFUSIONS = 30;
// Pitch classes weaker than this share of the strongest one are left out
const CHROMA_FLOOR = 0.2;

const round = (value, digits) => Number(value.toFixed(digits));

/**
 * view: what the player built from getAnalysis (segments, aggregated salience,
 * raw chroma, comparison); expected: { name, segments } or null; step: timeline
 * window in seconds.
 */
export function buildExport({ view, expected, audioName, step }) {
  const { frames, frameRate, duration, segments, salience, chroma, comparison, labelByKey } = view;
  const pitchNames = chroma.binNames.slice(12);
  const energy = frameEnergy(chroma, frames);
  const typicalEnergy = median(energy.filter((value) => value > 0)) || 1;

  /** Mean of every bin over the frames in [start, end), at least one frame. */
  const mean = (bins, start, end) => {
    const first = Math.min(Math.max(Math.floor(start * frameRate), 0), frames - 1);
    const last = Math.max(first + 1, Math.min(Math.ceil(end * frameRate), frames));
    const sum = new Float64Array(bins.binCount);
    for (let frame = first; frame < last; frame++) {
      for (let bin = 0; bin < bins.binCount; bin++) {
        sum[bin] += bins.values[frame * bins.binCount + bin];
      }
    }
    return { values: sum.map((value) => value / (last - first)), first, last };
  };

  const describe = (start, end, wanted) => {
    const chromaMean = mean(chroma, start, end);
    let level = 0;
    for (let frame = chromaMean.first; frame < chromaMean.last; frame++) {
      level += energy[frame];
    }
    const probabilities = mean(salience, start, end).values;
    const ranked = [...probabilities.keys()].sort((a, b) => probabilities[b] - probabilities[a]);
    const record = {
      level: round(level / (chromaMean.last - chromaMean.first) / typicalEnergy, 2),
      ...gated(view.gate, chromaMean.first, chromaMean.last),
      bass: sparseChroma(chromaMean.values.subarray(0, 12), pitchNames),
      treble: sparseChroma(chromaMean.values.subarray(12, 24), pitchNames),
      top: ranked.slice(0, TOP_CHORDS).map((label) => [salience.binNames[label], round(probabilities[label], 4)]),
    };
    if (wanted !== undefined) {
      const label = labelByKey.get(chordKey(wanted).id);
      record.expectedRank = label === undefined ? null : ranked.indexOf(label) + 1;
      record.expectedP = label === undefined ? null : round(probabilities[label], 4);
    }
    return record;
  };

  const records = comparison
    ? comparison.intervals.map((interval) => ({
      start: round(interval.start, 2),
      end: round(interval.end, 2),
      expected: interval.expected,
      detected: interval.detected,
      result: interval.result,
      ...describe(interval.start, interval.end, interval.expected),
    }))
    : segments.map((segment) => ({
      start: round(segment.start, 2),
      end: round(segment.end, 2),
      detected: segment.chord,
      ...describe(segment.start, segment.end),
    }));

  const timeline = [];
  for (let i = 0; i * step < duration; i++) {
    const start = i * step;
    const end = Math.min(start + step, duration);
    const middle = (start + end) / 2;
    // Before the first expected mark there is nothing to compare against
    const wanted = expected ? segmentAt(expected.segments, middle)?.chord : undefined;
    timeline.push({
      t: round(start, 2),
      detected: segmentAt(segments, middle)?.chord ?? 'N',
      ...(expected ? { expected: wanted ?? null } : {}),
      ...describe(start, end, wanted),
    });
  }

  const data = {
    readme: readme({ audioName, expected, step, templates: view.templateCount, tempo: view.tempo }),
    audio: { file: audioName, duration: round(duration, 2), frameRate: round(frameRate, 3), frames },
  };
  if (view.tempo) {
    data.tempo = { bpm: round(view.tempo.bpm, 1), meter: view.tempo.meter };
    data.bars = view.beats.filter((beat) => beat.position === 1).map((beat) => round(beat.time, 2));
  }
  if (comparison) {
    data.expectedFile = expected.name;
    data.score = { exact: round(comparison.exact, 3), root: round(comparison.root, 3) };
    data.confusions = confusions(comparison.intervals);
  }
  data.detected = segments.map(timeRange);
  if (expected) {
    data.expected = expected.segments
      .filter((segment) => segment.start < duration)
      .map((segment) => timeRange({ ...segment, end: Math.min(segment.end, duration) }));
  }
  data.segments = records;
  data.timeline = timeline;
  return serialize(data);
}

/** Share of the frames the confidence gate rejected, by reason; nothing when none were. */
function gated(gate, first, last) {
  const counts = [0, 0, 0];
  for (let frame = first; frame < last; frame++) {
    counts[gate[frame]]++;
  }
  if (counts[1] + counts[2] === 0) {
    return {};
  }
  const share = (count) => round(count / (last - first), 2);
  return { gated: { ...(counts[1] ? { vsN: share(counts[1]) } : {}), ...(counts[2] ? { rootMargin: share(counts[2]) } : {}) } };
}

function readme({ audioName, expected, step, templates, tempo }) {
  return [
    `Chord analysis of ${audioName} by Chordino (NNLS chroma, chord template matching, HMM/Viterbi smoothing). Times are in seconds.`,
    ...(tempo ? [
      'tempo: the BPM and time signature the beat tracker found; the BPM counts the note of the denominator (eighths in 6/8 and 12/8).',
      'bars: when each bar starts, its downbeat, as the beat tracker placed them.',
    ] : []),
    'detected: the chord segments the Viterbi path settled on. N = no chord.',
    ...(expected ? [
      `expected: reference chords from ${expected.name}.`,
      'score: share of the annotated time where detected equals expected exactly, and where at least the root matches. Comparison ignores spelling (G#º = Abº, C/C = C).',
      'confusions: expected -> detected pairs that disagree, by total seconds, worst first; result is root (same root, other quality or bass) or miss.',
      'segments: one record per stretch where neither the detected nor the expected chord changes.',
    ] : [
      'segments: one record per detected chord.',
    ]),
    `timeline: fixed windows of ${step} s starting at t; detected/expected are the chords sounding at the window midpoint.`,
    `bass / treble: mean NNLS chroma over the record, the two halves the chord templates are matched against, each scaled to its own strongest pitch class (1), weaker than ${CHROMA_FLOOR} left out, strongest first. {} = silence.`,
    'level: chroma energy relative to the median frame of the song; near 0 = silence or very quiet.',
    'gated: share of frames the confidence gate rejected and pushed towards N, by reason: vsN = the best chord did not beat the no-chord template by the required margin, rootMargin = a chord on another root scored too close to it. Absent when no frame was rejected.',
    `top: the ${TOP_CHORDS} most probable chord labels with their mean probability over the record. This is the matcher's per-frame distribution over ${templates} templates (summed per label) after its confidence gate, i.e. what the Viterbi decoder receives. Similar chords (inversions, sus, add) share most of the weight, so read the values relative to each other; a frame that fails the gate puts nearly all weight on N.`,
    ...(expected ? [
      'expectedRank / expectedP: rank (1 = most probable) and mean probability of the expected chord; null when the dictionary has no template for it.',
    ] : []),
    'The matcher scores an exponential moving average of the chroma (fast for bass, ~300 ms for treble), so the probabilities lag the chroma shown here slightly.',
  ];
}

/** Total of the raw chroma per frame, the loudness proxy behind `level`. */
function frameEnergy(chroma, frames) {
  const energy = new Float64Array(frames);
  for (let frame = 0; frame < frames; frame++) {
    for (let bin = 0; bin < 24; bin++) {
      energy[frame] += chroma.values[frame * 24 + bin];
    }
  }
  return energy;
}

function median(values) {
  const sorted = Float64Array.from(values).sort();
  return sorted.length ? sorted[Math.floor(sorted.length / 2)] : 0;
}

function sparseChroma(values, names) {
  const peak = Math.max(...values);
  if (!(peak > 0)) {
    return {};
  }
  return Object.fromEntries([...values.keys()]
    .map((bin) => [names[bin], round(values[bin] / peak, 2)])
    .filter(([, value]) => value >= CHROMA_FLOOR)
    .sort((a, b) => b[1] - a[1]));
}

function confusions(intervals) {
  const totals = new Map();
  for (const interval of intervals) {
    if (interval.result === 'exact') {
      continue;
    }
    const key = `${interval.expected}\u0000${interval.detected}`;
    const entry = totals.get(key) ?? { expected: interval.expected, detected: interval.detected, result: interval.result, seconds: 0 };
    entry.seconds += interval.end - interval.start;
    totals.set(key, entry);
  }
  return [...totals.values()]
    .sort((a, b) => b.seconds - a.seconds)
    .slice(0, MAX_CONFUSIONS)
    .map((entry) => ({ ...entry, seconds: round(entry.seconds, 2) }));
}

function timeRange(segment) {
  return { start: round(segment.start, 2), end: round(segment.end, 2), chord: segment.chord };
}

/** Pretty at the top level, one line per list item, so records read like rows. */
function serialize(data) {
  const fields = Object.entries(data).map(([key, value]) => {
    const body = Array.isArray(value) && value.length > 0
      ? `[\n${value.map((item) => `    ${JSON.stringify(item)}`).join(',\n')}\n  ]`
      : JSON.stringify(value);
    return `  ${JSON.stringify(key)}: ${body}`;
  });
  return `{\n${fields.join(',\n')}\n}\n`;
}
