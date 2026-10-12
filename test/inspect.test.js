import assert from 'node:assert/strict';
import { before, describe, test } from 'node:test';

import { analyzeChords, decodeMp3, detectChords, loadScript, paths, synthChord } from './helpers.js';

const SAMPLE_RATE = 44100;
const OUTPUTS = { chroma: 24, smoothedchroma: 24, chordsalience: null, semitonespectrum: 84 };

/** Bins of one output at the frame closest to the middle of the analysis. */
function middleFrame(analysis, output) {
  const { binCount, values } = analysis[output];
  const frame = Math.floor(analysis.times.length / 2);
  return values.subarray(frame * binCount, (frame + 1) * binCount);
}

const argmax = (values) => values.indexOf(Math.max(...values));

describe('inspect build', () => {
  let chordino;
  let inspect;

  before(() => {
    chordino = loadScript(paths.chordino);
    inspect = loadScript(paths.inspect);
  });

  test('chords match the production build on chords.mp3', async () => {
    const { samples, sampleRate } = await decodeMp3(paths.chordsMp3);
    const analysis = analyzeChords(inspect, samples, sampleRate);

    assert.deepEqual(analysis.chords, detectChords(chordino, samples, sampleRate));
  });

  test('every output has one row of binCount values per frame', async () => {
    const { samples, sampleRate } = await decodeMp3(paths.chordsMp3);
    const analysis = analyzeChords(inspect, samples, sampleRate);
    const frames = analysis.times.length;

    assert.ok(frames > 100, `only ${frames} frames`);
    for (let i = 1; i < frames; i++) {
      assert.ok(analysis.times[i] > analysis.times[i - 1], `time goes back at ${i}`);
    }
    for (const [output, binCount] of Object.entries(OUTPUTS)) {
      const bins = analysis[output];
      if (binCount !== null) {
        assert.equal(bins.binCount, binCount, output);
      }
      assert.equal(bins.binNames.length, bins.binCount, output);
      assert.equal(bins.values.length, frames * bins.binCount, output);
    }
    assert.ok(analysis.gate instanceof Uint8Array);
    assert.equal(analysis.gate.length, frames);
    assert.ok(analysis.gate.every((reason) => reason <= 2), 'gate reasons are 0, 1 or 2');
    assert.equal(analysis.chordsalience.binNames.at(-1), 'N');
    assert.equal(analysis.chroma.binNames[0], 'A (bass)');
    assert.equal(analysis.chroma.binNames[12], 'A');
  });

  test('a C chord lights the C, E and G treble bins and wins the salience', () => {
    const analysis = analyzeChords(inspect, synthChord(['C3', 'C4', 'E4', 'G4'], 3, SAMPLE_RATE), SAMPLE_RATE);
    const treble = Array.from(middleFrame(analysis, 'chroma').subarray(12));
    const top3 = treble.map((value, bin) => [value, analysis.chroma.binNames[12 + bin]])
      .sort((a, b) => b[0] - a[0])
      .slice(0, 3)
      .map(([, name]) => name);

    assert.deepEqual(top3.sort(), ['C', 'E', 'G']);
    const salience = middleFrame(analysis, 'chordsalience');
    assert.equal(analysis.chordsalience.binNames[argmax(salience)], 'C');
  });

  test('semitone bins start at A0', () => {
    const analysis = analyzeChords(inspect, synthChord(['C4'], 2, SAMPLE_RATE), SAMPLE_RATE);
    const peak = argmax(middleFrame(analysis, 'semitonespectrum'));

    assert.equal(peak, 60 - 21);
    assert.equal(analysis.semitonespectrum.binNames[peak], 'C4');
  });
});
