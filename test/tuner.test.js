import assert from 'node:assert/strict';
import { after, before, describe, test } from 'node:test';

import { loadChordino } from './helpers.js';

const SIZE = 4096;
const HALF = SIZE / 2;

const cents = (frequency, expected) => 1200 * Math.log2(frequency / expected);

/** size samples of a tone with partials harmonics at 1/k, starting offset samples in */
function tone(frequency, sampleRate, { partials = 4, noise = 0, offset = 0, random = Math.random } = {}) {
  const samples = new Float32Array(SIZE);
  for (let i = 0; i < SIZE; i++) {
    const t = (i + offset) / sampleRate;
    let value = 0;
    for (let k = 1; k <= partials; k++) {
      value += Math.sin(2 * Math.PI * frequency * k * t) / k;
    }
    samples[i] = 0.3 * value + noise * (random() * 2 - 1);
  }
  return samples;
}

/** The same YIN as pitch.c, over the direct sum: the lag it settles on, or 0 */
function directLag(samples) {
  const d = new Float64Array(HALF);
  for (let tau = 1; tau < HALF; tau++) {
    let sum = 0;
    for (let i = 0; i < HALF; i++) {
      const delta = samples[i] - samples[i + tau];
      sum += delta * delta;
    }
    d[tau] = sum;
  }
  let runningSum = 0;
  for (let tau = 1; tau < HALF; tau++) {
    runningSum += d[tau];
    d[tau] = runningSum > 0 ? (d[tau] * tau) / runningSum : 1;
  }
  let tau = 2;
  while (tau < HALF && d[tau] >= 0.1) tau++;
  if (tau === HALF) return 0;
  while (tau + 1 < HALF && d[tau + 1] < d[tau]) tau++;
  if (tau + 1 === HALF) return tau;
  const [s0, s1, s2] = [d[tau - 1], d[tau], d[tau + 1]];
  return tau + (s2 - s0) / (2 * (2 * s1 - s2 - s0));
}

/** Deterministic [0, 1) numbers (mulberry32) */
function seeded(seed) {
  return () => {
    seed = (seed + 0x6d2b79f5) | 0;
    let t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

describe('pitch detector (pitch.c)', () => {
  let Module;
  let pBuffer;
  const detectors = new Map();

  before(async () => {
    ({ Module } = await loadChordino());
    pBuffer = Module._malloc(SIZE * Float32Array.BYTES_PER_ELEMENT);
  });

  after(() => {
    for (const pitch of detectors.values()) {
      Module.releasePitch(pitch);
    }
    Module._free(pBuffer);
  });

  function detector(sampleRate) {
    if (!detectors.has(sampleRate)) {
      detectors.set(sampleRate, Module.createPitch(sampleRate, SIZE));
    }
    return detectors.get(sampleRate);
  }

  function detect(samples, sampleRate) {
    Module.HEAPF32.set(samples, pBuffer >> 2);
    return Module.detectPitch(detector(sampleRate), pBuffer);
  }

  test('tones with 4 partials come out within 5 cents', () => {
    for (const [frequency, sampleRate] of [[82.41, 44100], [440, 48000], [1046.5, 48000]]) {
      const found = detect(tone(frequency, sampleRate), sampleRate);
      assert.ok(Math.abs(cents(found, frequency)) < 5, `${frequency} Hz read as ${found}`);
    }
  });

  test('A0 to C8 within 10 cents', () => {
    // C8 reads about 5 cents sharp: its period is 11.5 samples at 48 kHz, short
    // for the parabola through three lags
    for (const [frequency, sampleRate] of [[27.5, 44100], [27.5, 48000], [2093, 48000], [4186, 48000]]) {
      const found = detect(tone(frequency, sampleRate), sampleRate);
      assert.ok(Math.abs(cents(found, frequency)) < 10, `${frequency} Hz read as ${found}`);
    }
  });

  test('settles on the lag a direct-sum YIN does', () => {
    const random = seeded(1);
    for (let n = 0; n < 40; n++) {
      const sampleRate = n % 2 ? 48000 : 44100;
      const samples = n % 8 === 7
        ? Float32Array.from({ length: SIZE }, () => random() * 2 - 1)
        : tone(27.5 * 2 ** (random() * 7), sampleRate, {
          partials: 1 + (n % 6),
          noise: 0.05 * (n % 4),
          offset: random() * 1000,
          random,
        });
      const found = detect(samples, sampleRate);
      const lag = directLag(samples);
      if (lag === 0) {
        assert.equal(found, 0, `window ${n}`);
      } else {
        assert.ok(Math.abs(sampleRate / found - lag) < 1e-6, `window ${n}: lag ${sampleRate / found}, direct ${lag}`);
      }
    }
  });

  test('silence has no pitch', () => {
    assert.equal(detect(new Float32Array(SIZE), 48000), 0);
  });

  test('pitchRms is the RMS of the window', () => {
    const samples = tone(440, 48000);
    let sumSq = 0;
    for (const value of samples) {
      sumSq += value * value;
    }
    Module.HEAPF32.set(samples, pBuffer >> 2);
    assert.ok(Math.abs(Module.pitchRms(detector(48000), pBuffer) - Math.sqrt(sumSq / SIZE)) < 1e-12);
  });

  test('createPitch takes even sizes only', () => {
    assert.equal(Module.createPitch(48000, 4095), 0);
  });
});
