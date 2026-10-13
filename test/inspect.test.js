import assert from 'node:assert/strict';
import { before, describe, test } from 'node:test';

import { analyzeChords, decodeMp3, detectChords, feed, loadChordino, loadScript, paths, synthChord } from './helpers.js';

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

  before(async () => {
    chordino = await loadChordino();
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

  test('getPartialAnalysis keeps every frame its bins while feeding', async () => {
    const { Module } = inspect;
    const { samples, sampleRate } = await decodeMp3(paths.chordsMp3);
    const rows = (analysis, frame) => ['chroma', 'semitonespectrum'].map((output) => {
      const { binCount, values } = analysis[output];
      return Array.from(values.subarray(frame * binCount, (frame + 1) * binCount));
    });
    const firstSeen = new Map();
    let after = -1;
    const instance = Module.initializePlugin(sampleRate, 4096, true);
    try {
      const all = feed(inspect, samples, sampleRate, () => Module.getPartialAnalysis(instance, -1), {
        instance,
        afterEach: () => {
          const analysis = Module.getPartialAnalysis(instance, after);
          analysis.times.forEach((time, frame) => {
            assert.ok(time > after, `frame at ${time} is not after ${after}`);
            firstSeen.set(time, rows(analysis, frame));
          });
          after = analysis.times.at(-1) ?? after;
        },
      });

      assert.deepEqual(all.chords, Module.getPartialResult(instance));
      assert.equal(all.times.length, firstSeen.size);
      // Later calls have no NNLS output for these frames: the bins come from when they were new
      all.times.forEach((time, frame) => assert.deepEqual(rows(all, frame), firstSeen.get(time), `frame at ${time}`));
    } finally {
      Module.releasePlugin(instance);
    }
  });

  test('getPartialAnalysis has the bins of frames getPartialResult decoded first', async () => {
    const { Module } = inspect;
    const { samples, sampleRate } = await decodeMp3(paths.chordsMp3);
    // Reads every 5 blocks, then all the bins
    const analyze = (read) => {
      const instance = Module.initializePlugin(sampleRate, 4096, true);
      let block = 0;
      try {
        return feed(inspect, samples, sampleRate, () => Module.getPartialAnalysis(instance, -1), {
          instance,
          afterEach: () => {
            if (++block % 5 === 0) read(instance);
          },
        });
      } finally {
        Module.releasePlugin(instance);
      }
    };
    const afterResults = analyze((instance) => Module.getPartialResult(instance));
    const afterAnalyses = analyze((instance) => Module.getPartialAnalysis(instance, -1));

    assert.deepEqual(afterResults.chroma.values, afterAnalyses.chroma.values);
    assert.deepEqual(afterResults.semitonespectrum.values, afterAnalyses.semitonespectrum.values);
  });

  test('semitone bins start at A0', () => {
    const analysis = analyzeChords(inspect, synthChord(['C4'], 2, SAMPLE_RATE), SAMPLE_RATE);
    const peak = argmax(middleFrame(analysis, 'semitonespectrum'));

    assert.equal(peak, 60 - 21);
    assert.equal(analysis.semitonespectrum.binNames[peak], 'C4');
  });
});
