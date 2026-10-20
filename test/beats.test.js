import assert from 'node:assert/strict';
import { before, describe, test } from 'node:test';

import { feed, loadChordino, synthBand } from './helpers.js';

const SAMPLE_RATE = 44100;
const LEAD = 0.5;

const C = ['C3', 'C4', 'E4', 'G4'];
const G = ['G2', 'G3', 'B3', 'D4'];
const Am = ['A2', 'A3', 'C4', 'E4'];
const F = ['F2', 'A3', 'C4', 'F4'];
const SONG = [C, G, Am, F, C, G, F, C, C, G, Am, F, C, G, F, C];

describe('beats', () => {
  let chordino;

  before(async () => {
    chordino = await loadChordino();
  });

  const beatsOf = (samples, sampleRate = SAMPLE_RATE) => feed(chordino, samples, sampleRate, (instance) => {
    chordino.Module.getResult(instance);
    return chordino.Module.getBeats(instance);
  });

  /** Every beat on the band's grid, within tolerance, numbered from 1 at each bar */
  function assertOnGrid(result, { bpm, numerator }, tolerance = 0.04) {
    const beat = 60 / bpm;
    const inMusic = result.beats.filter(({ time }) => time > LEAD - tolerance && time < LEAD + beat * numerator * SONG.length);
    assert.ok(inMusic.length >= numerator * SONG.length - 2, `${inMusic.length} beats in the music`);
    for (const { time, position } of inMusic) {
      const index = Math.round((time - LEAD) / beat);
      assert.ok(Math.abs(time - LEAD - index * beat) < tolerance, `beat at ${time} off the grid`);
      assert.equal(position, (index % numerator) + 1, `beat at ${time}`);
    }
  }

  for (const band of [
    { bpm: 120, numerator: 4, meter: [4, 4] },
    { bpm: 72, numerator: 4, meter: [4, 4] },
    { bpm: 90, numerator: 3, meter: [3, 4] },
    { bpm: 150, numerator: 6, meter: [6, 8] },
    { bpm: 180, numerator: 12, meter: [12, 8] },
  ]) {
    test(`${band.meter.join('/')} at ${band.bpm} BPM`, () => {
      const result = beatsOf(synthBand({ ...band, chords: SONG, sampleRate: SAMPLE_RATE, lead: LEAD }));

      assert.ok(Math.abs(result.bpm - band.bpm) < band.bpm * 0.02, `${result.bpm} BPM`);
      assert.deepEqual([result.numerator, result.denominator], band.meter);
      assertOnGrid(result, band);
    });
  }

  test('beats at 48 kHz are timed as at 44.1', () => {
    const band = { bpm: 120, numerator: 4 };
    const result = beatsOf(synthBand({ ...band, chords: SONG, sampleRate: 48000, lead: LEAD }), 48000);

    assert.deepEqual([result.numerator, result.denominator], [4, 4]);
    assertOnGrid(result, band);
  });

  test('none in silence', () => {
    assert.equal(beatsOf(new Float32Array(3 * SAMPLE_RATE)), null);
  });

  test('none before getResult, nor for a realtime instance', () => {
    const { Module } = chordino;
    const samples = synthBand({ bpm: 120, numerator: 4, chords: SONG.slice(0, 4), sampleRate: SAMPLE_RATE });
    assert.equal(feed(chordino, samples, SAMPLE_RATE, (instance) => Module.getBeats(instance)), null);

    const instance = Module.initializePlugin(SAMPLE_RATE, 4096, true);
    try {
      const beats = feed(chordino, samples, SAMPLE_RATE, () => {
        Module.getResult(instance);
        return Module.getBeats(instance);
      }, { instance });
      assert.equal(beats, null);
    } finally {
      Module.releasePlugin(instance);
    }
  });
});
