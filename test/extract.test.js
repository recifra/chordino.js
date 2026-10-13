import assert from 'node:assert/strict';
import { before, describe, test } from 'node:test';

import { concat, decodeMp3, detectChords, loadChordino, paths, synthChord } from './helpers.js';

const SAMPLE_RATE = 44100;

const labels = (result) => result.map((item) => item.chord);

function assertWellFormed(result) {
  assert.ok(result.length >= 2, 'expected at least one chord plus the closing N');
  assert.equal(result[0].time, 0);
  assert.equal(result.at(-1).chord, 'N', 'result always ends with N at the last frame');
  for (let i = 1; i < result.length; i++) {
    assert.ok(result[i].time > result[i - 1].time, `time goes back at ${i}: ${JSON.stringify(result)}`);
    if (i < result.length - 1) {
      assert.notEqual(result[i].chord, result[i - 1].chord, `${result[i].chord} repeated at ${i}`);
    }
  }
}

describe('offline extraction', () => {
  let chordino;

  before(async () => {
    chordino = await loadChordino();
  });

  test('chords.mp3 is G, C, F', async () => {
    const { samples, sampleRate } = await decodeMp3(paths.chordsMp3);
    const result = detectChords(chordino, samples, sampleRate);

    assertWellFormed(result);
    assert.deepEqual(labels(result), ['G', 'C', 'F', 'N']);
    // Reference from upstream Chordino (src/nnls-chroma/chordextract): C at 1.49s,
    // F at 2.83s. The chroma smoothing runs both ways, so it must not lag them.
    const [, c, f, end] = result;
    assert.ok(Math.abs(c.time - 1.49) < 0.1, `C at ${c.time}`);
    assert.ok(Math.abs(f.time - 2.83) < 0.1, `F at ${f.time}`);
    assert.ok(end.time > 5 && end.time <= samples.length / sampleRate, `end at ${end.time}`);
  });

  const voicings = {
    C: ['C3', 'C4', 'E4', 'G4'],
    Cm: ['C3', 'C4', 'Eb4', 'G4'],
    Am: ['A2', 'A3', 'C4', 'E4'],
    Dm: ['D2', 'A3', 'D4', 'F4'],
    E: ['E2', 'G#3', 'B3', 'E4'],
    F: ['F2', 'A3', 'C4', 'F4'],
    G: ['G2', 'G3', 'B3', 'D4'],
    G7: ['G2', 'G3', 'B3', 'D4', 'F4'],
    'D7/C': ['C3', 'D4', 'F#4', 'A4', 'C5'],
    C7M: ['C3', 'C4', 'E4', 'G4', 'B4'],
    Csus4: ['C3', 'C4', 'F4', 'G4'],
    'C/E': ['E2', 'C4', 'E4', 'G4'],
    'C/G': ['G2', 'C4', 'E4', 'G4'],
    'Am/C': ['C3', 'A3', 'C4', 'E4'],
    'Am/E': ['E2', 'A3', 'C4', 'E4'],
  };

  for (const [chord, notes] of Object.entries(voicings)) {
    test(`detects ${chord} from ${notes.join(' ')}`, () => {
      const result = detectChords(chordino, synthChord(notes, 3, SAMPLE_RATE), SAMPLE_RATE);

      assertWellFormed(result);
      assert.deepEqual(labels(result), [chord, 'N']);
    });
  }

  test('follows a C Am F G progression', () => {
    const progression = ['C', 'Am', 'F', 'G'];
    const samples = concat(...progression.map((chord) => synthChord(voicings[chord], 2, SAMPLE_RATE)));
    const result = detectChords(chordino, samples, SAMPLE_RATE);

    assertWellFormed(result);
    assert.deepEqual(labels(result), [...progression, 'N']);
    for (let i = 1; i < progression.length; i++) {
      assert.ok(Math.abs(result[i].time - 2 * i) < 0.1, `${result[i].chord} at ${result[i].time}`);
    }
  });

  test('silence is N only', () => {
    const result = detectChords(chordino, new Float32Array(3 * SAMPLE_RATE), SAMPLE_RATE);

    assert.deepEqual(new Set(labels(result)), new Set(['N']));
  });

  test('setProgressCallback reports getResult from 0 to 1 without changing it', async () => {
    const { Module } = chordino;
    const { samples, sampleRate } = await decodeMp3(paths.chordsMp3);
    const plain = detectChords(chordino, samples, sampleRate);
    const reports = [];
    Module.setProgressCallback((done) => reports.push(done));
    try {
      assert.deepEqual(detectChords(chordino, samples, sampleRate), plain);
    } finally {
      Module.setProgressCallback(null);
    }

    assert.equal(reports[0], 0);
    assert.equal(reports.at(-1), 1);
    assert.ok(reports.length > 10, `only ${reports.length} reports`);
    for (let i = 1; i < reports.length; i++) {
      assert.ok(reports[i] > reports[i - 1], `progress goes back at ${i}: ${reports}`);
    }
    const count = reports.length;
    detectChords(chordino, samples, sampleRate);
    assert.equal(reports.length, count, 'null turns the reports off');
  });

  test('resetProcess drops the audio fed before it', () => {
    const { Module } = chordino;
    const instance = Module.initializePlugin(SAMPLE_RATE, 4096);
    try {
      detectChords(chordino, synthChord(voicings.C, 2, SAMPLE_RATE), SAMPLE_RATE, { instance });
      Module.resetProcess(instance);
      const result = detectChords(chordino, synthChord(voicings.Am, 2, SAMPLE_RATE), SAMPLE_RATE, { instance });

      assert.deepEqual(labels(result), ['Am', 'N']);
    } finally {
      Module.releasePlugin(instance);
    }
  });
});
