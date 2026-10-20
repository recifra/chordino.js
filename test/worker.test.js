import assert from 'node:assert/strict';
import { before, beforeEach, describe, test } from 'node:test';

import { concat, decodeMp3, loadWorker, paths, synthBand, synthChord } from './helpers.js';

const SAMPLE_RATE = 44100;
// Share of the progress src/js/chord-worker.ts gives to feeding the audio
const FEED_SHARE = 0.3;

/**
 * Reproducible white noise. Chordino only reports N where something fails its
 * confidence gate: in digital silence the smoothed chroma still holds the fading
 * neighbouring chords, so a gap needs noise to come out as no chord.
 */
function noise(seconds, amplitude) {
  const out = new Float32Array(Math.round(seconds * SAMPLE_RATE));
  let state = 1;
  for (let i = 0; i < out.length; i++) {
    state = (state * 1103515245 + 12345) % 2147483648;
    out[i] = amplitude * (state / 1073741824 - 1);
  }
  return out;
}

describe('offline worker', () => {
  let worker;

  before(async () => {
    worker = await loadWorker(paths.worker);
  });

  beforeEach(() => {
    worker.messages.length = 0;
  });

  /** Runs one job: the progress values posted and the message that ended it. */
  async function run(data) {
    await worker.send(data);
    return {
      progress: worker.messages.filter((message) => message.type === 'progress').map((message) => message.value),
      last: worker.messages.at(-1),
    };
  }

  test('chords.mp3 comes back as training.json timestamps', async () => {
    const { samples, sampleRate } = await decodeMp3(paths.chordsMp3);
    const duration = samples.length / sampleRate;
    const { last } = await run({ samples, sampleRate });

    assert.equal(last.type, 'done', JSON.stringify(last));
    const { timestamps } = last.result;
    assert.deepEqual(timestamps.map(([chord]) => chord), ['G', 'C', 'F', '']);
    const [g, c, f, end] = timestamps;
    assert.equal(g[1], 0);
    assert.ok(Math.abs(c[1] - 1.49) < 0.1, `C at ${c[1]}`);
    assert.ok(Math.abs(f[1] - 2.83) < 0.1, `F at ${f[1]}`);
    assert.equal(end[1], Math.round(duration * 1000) / 1000, 'closes at the end of the audio');
  });

  test('reports progress from 0 to 1, through getResult too', async () => {
    const { samples, sampleRate } = await decodeMp3(paths.chordsMp3);
    const { progress, last } = await run({ samples, sampleRate });

    assert.equal(last.type, 'done');
    assert.equal(progress[0], 0);
    assert.equal(progress.at(-1), 1);
    for (let i = 1; i < progress.length; i++) {
      assert.ok(progress[i] > progress[i - 1], `progress goes back at ${i}: ${progress}`);
    }
    // Feeding stops at FEED_SHARE; anything between that and 1 came from getResult
    assert.ok(progress.filter((value) => value > FEED_SHARE && value < 1).length > 10, `${progress}`);
  });

  test('silence is a single closing mark', async () => {
    const { last } = await run({ samples: new Float32Array(3 * SAMPLE_RATE), sampleRate: SAMPLE_RATE });

    assert.deepEqual(last, { type: 'done', result: { timestamps: [['', 3]], beats: [], tempo: null } });
  });

  test('comes back with the beats, the tempo and the time signature', async () => {
    const C = ['C3', 'C4', 'E4', 'G4'];
    const G = ['G2', 'G3', 'B3', 'D4'];
    const samples = synthBand({ bpm: 120, numerator: 4, chords: [C, G, C, G, C, G, C, G], sampleRate: SAMPLE_RATE });
    const { last } = await run({ samples, sampleRate: SAMPLE_RATE });

    assert.equal(last.type, 'done', JSON.stringify(last));
    const { beats, tempo } = last.result;
    assert.deepEqual(tempo, { bpm: 120, meter: [4, 4] });
    // 8 bars of 2 s from 0.5 s, to the millisecond
    const bars = beats.filter(([seconds, position]) => position === 1 && seconds > 0.4 && seconds < 16.4);
    assert.equal(bars.length, 8, JSON.stringify(beats));
    bars.forEach(([seconds], bar) => assert.ok(Math.abs(seconds - 0.5 - bar * 2) < 0.04, `bar ${bar} at ${seconds}`));
    for (const [seconds] of beats) {
      assert.equal(seconds, Math.round(seconds * 1000) / 1000);
    }
  });

  test('a gap with no chord comes back as an empty label', async () => {
    const samples = concat(
      synthChord(['C3', 'C4', 'E4', 'G4'], 2, SAMPLE_RATE),
      noise(2, 0.05),
      synthChord(['G2', 'G3', 'B3', 'D4'], 2, SAMPLE_RATE),
    );
    const { last } = await run({ samples, sampleRate: SAMPLE_RATE });

    assert.deepEqual(last.result.timestamps.map(([chord]) => chord), ['C', '', 'G', '']);
  });

  test('no chord before the first one is left out', async () => {
    const samples = concat(noise(2, 0.05), synthChord(['C3', 'C4', 'E4', 'G4'], 2, SAMPLE_RATE));
    const { last } = await run({ samples, sampleRate: SAMPLE_RATE });

    const { timestamps } = last.result;
    assert.deepEqual(timestamps.map(([chord]) => chord), ['C', '']);
    assert.ok(Math.abs(timestamps[0][1] - 2) < 0.6, `C at ${timestamps[0][1]}`);
    assert.equal(timestamps[1][1], 4);
  });

  test('rejects a message without samples', async () => {
    const { progress, last } = await run({});

    assert.deepEqual(progress, []);
    assert.equal(last.type, 'error');
    assert.match(last.message, /Float32Array/);
  });
});
