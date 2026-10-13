// Chord label helpers shared by the player, the export and test/eval.js. No DOM here.

const NATURALS = { C: 0, D: 2, E: 4, F: 5, G: 7, A: 9, B: 11 };

function pitchClass(name) {
  let pitch = NATURALS[name[0]];
  for (const accidental of name.slice(1)) {
    pitch += accidental === '#' ? 1 : -1;
  }
  return (pitch + 12) % 12;
}

/** Spelling-independent identity of a label: G#º = Abº, C/C = C, "" = N. */
export function chordKey(label) {
  const match = /^([A-G][#b]*)([^/]*)(?:\/([A-G][#b]*))?$/.exec(label);
  if (!match) {
    return { root: null, id: label || 'N' };
  }
  const root = pitchClass(match[1]);
  const bass = match[3] ? pitchClass(match[3]) : root;
  return { root, id: `${root}${match[2]}/${bass}` };
}

/** exact: same chord; root: same root, other quality or bass; miss: anything else. */
export function grade(detected, wanted) {
  const got = chordKey(detected);
  const want = chordKey(wanted);
  if (got.id === want.id) {
    return 'exact';
  }
  return got.root !== null && got.root === want.root ? 'root' : 'miss';
}

/** The segment sounding at `time`, if any. */
export function segmentAt(segments, time) {
  return segments.find((segment) => segment.start <= time && time < segment.end);
}

// {"timestamps": [[chord, seconds], ...]} as in training/training.json, where
// "" is no chord (it closes the last one); getResult()'s [{chord, time}] works too.
export function parseExpected(data) {
  const entries = Array.isArray(data) ? data : data?.timestamps;
  if (!Array.isArray(entries)) {
    throw new Error('expected {"timestamps": [[chord, seconds], ...]}');
  }
  const marks = entries
    .map((entry) => (Array.isArray(entry) ? { chord: entry[0], time: entry[1] } : entry))
    .sort((a, b) => a.time - b.time);
  for (const mark of marks) {
    if (typeof mark?.chord !== 'string' || !Number.isFinite(mark.time)) {
      throw new Error(`bad entry ${JSON.stringify(mark)}`);
    }
  }
  return marks.map((mark, i) => ({
    chord: mark.chord || 'N',
    start: mark.time,
    end: i + 1 < marks.length ? marks[i + 1].time : Infinity,
  }));
}

/** Overlaps of detected and expected segments, graded, plus time-weighted scores. */
export function compare(detected, wanted, duration) {
  const intervals = [];
  const totals = { exact: 0, root: 0, miss: 0 };
  for (const want of wanted) {
    for (const got of detected) {
      const start = Math.max(want.start, got.start);
      const end = Math.min(want.end, got.end, duration);
      if (start < end) {
        const result = grade(got.chord, want.chord);
        intervals.push({ start, end, result, expected: want.chord, detected: got.chord });
        totals[result] += end - start;
      }
    }
  }
  const total = totals.exact + totals.root + totals.miss;
  return {
    intervals,
    exact: total > 0 ? totals.exact / total : 0,
    root: total > 0 ? (totals.exact + totals.root) / total : 0,
  };
}
