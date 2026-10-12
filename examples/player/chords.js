// Chord label helpers shared by the player and the export.

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
