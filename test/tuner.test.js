import assert from 'node:assert/strict';
import { after, before, beforeEach, describe, test } from 'node:test';

import { loadChordino, loadLive, messageChannel, paths, stream } from './helpers.js';

const SIZE = 4096;
const HALF = SIZE / 2;

const cents = (frequency, expected) => 1200 * Math.log2(frequency / expected);

/** length samples of a tone with partials harmonics at 1/k, starting offset samples in */
function tone(frequency, sampleRate, { length = SIZE, amplitude = 0.3, partials = 4, noise = 0, offset = 0, random = Math.random } = {}) {
  const samples = new Float32Array(length);
  for (let i = 0; i < length; i++) {
    const t = (i + offset) / sampleRate;
    let value = 0;
    for (let k = 1; k <= partials; k++) {
      value += Math.sin(2 * Math.PI * frequency * k * t) / k;
    }
    samples[i] = amplitude * value + noise * (random() * 2 - 1);
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

describe('tuner worker', () => {
  // The worklet's own rate only places a buffer that ends inside a render
  // quantum; 2048 never does, so 44.1 kHz tones go through it too
  const SAMPLE_RATE = 48000;
  const HOP = 2048;
  let clock;
  let worker;
  let ChordProcessor;
  let start;

  before(async () => {
    ({ clock, worker, ChordProcessor, start } = await loadLive(SAMPLE_RATE, paths.tunerWorker));
  });

  beforeEach(() => {
    worker.messages.length = 0;
  });

  const pitches = () => worker.messages.filter((message) => message.type === 'pitch');

  /** A worklet sending buffers of HOP samples to the worker, once its wasm is up */
  async function listen({ sampleRate = SAMPLE_RATE, ...options } = {}) {
    const live = await start({ sampleRate, processorOptions: { bufferSize: HOP }, ...options });
    assert.deepEqual(worker.messages, [{ type: 'ready' }]);
    worker.messages.length = 0;
    return { ...live, sampleRate };
  }

  /** What the worker posts for each HOP samples, as app-recifra's detector returned it: a frequency or null */
  function readings({ processor, sampleRate }, samples) {
    const found = [];
    for (let offset = 0; offset + HOP <= samples.length; offset += HOP) {
      const before = pitches().length;
      stream(processor, clock, samples.subarray(offset, offset + HOP), sampleRate);
      const posted = pitches().slice(before);
      assert.ok(posted.length <= 1, 'more than one reading for a buffer');
      found.push(posted[0]?.frequency ?? null);
    }
    return found;
  }

  const seconds = (frequency, sampleRate, length, options) =>
    tone(frequency, sampleRate, { length: Math.round(length * sampleRate), amplitude: 0.15, ...options });

  for (const [frequency, sampleRate] of [[440, 48000], [82.41, 44100], [1046.5, 48000]]) {
    test(`posts ${frequency} Hz at ${sampleRate} Hz once it holds steady`, async () => {
      const live = await listen({ sampleRate });
      const startTime = clock.currentTime;
      const found = readings(live, seconds(frequency, sampleRate, 1));

      // The second buffer fills the window, then three readings have to agree
      assert.deepEqual(found.slice(0, 3), [null, null, null]);
      assert.ok(!found.slice(3).includes(null), `readings ${found}`);
      for (const value of found.slice(3)) {
        assert.ok(Math.abs(cents(value, frequency)) < 5, `${frequency} Hz read as ${value}`);
      }
      // time is the end of the window read, on the AudioContext clock
      const first = pitches()[0];
      assert.ok(Math.abs(first.time - startTime - (4 * HOP) / sampleRate) < 1e-6, `first reading at ${first.time - startTime}`);
    });
  }

  test('stays quiet under the RMS gate, then waits for three readings again', async () => {
    const live = await listen();
    assert.ok(Math.abs(readings(live, seconds(440, SAMPLE_RATE, 0.5)).at(-1) - 440) < 0.5);

    // The first buffer still has the end of the tone in the other half of the window
    assert.ok(readings(live, new Float32Array(SAMPLE_RATE)).slice(1).every((value) => value === null));
    assert.ok(readings(live, seconds(440, SAMPLE_RATE, 0.5, { amplitude: 0.0025 })).every((value) => value === null));

    const found = readings(live, seconds(440, SAMPLE_RATE, 0.5));
    assert.deepEqual(found.slice(0, 2), [null, null]);
    assert.ok(Math.abs(found.at(-1) - 440) < 0.5);
  });

  test('keeps a lone sharp window off the reading', async () => {
    const live = await listen();
    readings(live, seconds(261.63, SAMPLE_RATE, 0.5));
    // ~15 cents sharp, as a window that lost microphone samples reads: still steady
    const found = readings(live, seconds(263.9, SAMPLE_RATE, 0.1));
    for (const value of found.filter((value) => value !== null)) {
      assert.ok(Math.abs(cents(value, 261.63)) < 5, `read ${value}`);
    }
  });

  test('reads once per batch, on the newest window, when it falls behind', async () => {
    const live = await listen();
    readings(live, seconds(440, SAMPLE_RATE, 0.5));
    const before = pitches().length;
    live.port.hold();
    // Every window read in the middle would be at 330 Hz and break the run
    const blocks = [440, 330, 330, 330, 440, 440].map((frequency) => seconds(frequency, SAMPLE_RATE, HOP / SAMPLE_RATE));
    for (const block of blocks) {
      stream(live.processor, clock, block, SAMPLE_RATE);
    }
    const batches = [];
    const handler = live.port.onmessage;
    live.port.onmessage = (event) => {
      batches.push(event.data.blocks.length);
      handler(event);
    };
    while (live.port.flush() > 0);

    assert.deepEqual(batches, [1, 5]);
    const posted = pitches().slice(before);
    assert.equal(posted.length, 2, 'one reading per batch');
    for (const { frequency } of posted) {
      assert.ok(Math.abs(cents(frequency, 440)) < 5, `read ${frequency}`);
    }
    assert.ok(Math.abs(posted[1].time - clock.currentTime) < 1e-6, 'the second reading is not of the newest window');
  });

  test('answers the worklet before the wasm is up, then posts ready, pitch and stopped', async () => {
    const processor = new ChordProcessor({ processorOptions: { bufferSize: HOP } });
    const { port1, port2 } = messageChannel();
    let replies = 0;
    const postMessage = port1.postMessage.bind(port1);
    port1.postMessage = (data) => {
      replies += data.type === 'idle';
      postMessage(data);
    };
    processor.port.send({ type: 'port', port: port2 });
    const starting = worker.send({ type: 'start', sampleRate: SAMPLE_RATE, port: port1 });
    stream(processor, clock, seconds(440, SAMPLE_RATE, (3 * HOP) / SAMPLE_RATE), SAMPLE_RATE);
    assert.equal(replies, 3, 'the worklet waited on a worker without its wasm');
    await starting;
    assert.deepEqual(worker.messages, [{ type: 'ready' }]);

    stream(processor, clock, seconds(440, SAMPLE_RATE, 0.5), SAMPLE_RATE);
    assert.ok(pitches().length > 0);

    processor.port.send({ type: 'stop' });
    await worker.send({ type: 'stop' });
    assert.equal(worker.messages.at(-1).type, 'stopped');
    assert.ok(port1.closed, 'the worker kept its end of the channel open');
  });

  test('reports a start it cannot run', async () => {
    const { port1 } = messageChannel();
    await worker.send({ type: 'start', port: port1 });

    assert.equal(worker.messages.at(-1).type, 'error');
  });

  test('clockReports: the AudioContext time over the time it took, every 5 s', async () => {
    const descriptor = Object.getOwnPropertyDescriptor(globalThis, 'performance');
    let wallMs = 0;
    Object.defineProperty(globalThis, 'performance', { value: { now: () => wallMs }, configurable: true });
    try {
      const live = await listen({ clockReports: true });
      const hopMs = (HOP / SAMPLE_RATE) * 1000;
      const feed = (count) => {
        for (let n = 0; n < count; n++) {
          // The audio thread runs at 0.98 of real time
          wallMs += hopMs / 0.98;
          stream(live.processor, clock, new Float32Array(HOP), SAMPLE_RATE);
        }
      };
      const ratios = () => worker.messages.filter((message) => message.type === 'clock').map((message) => message.ratio);

      feed(Math.ceil(12000 / hopMs));
      assert.equal(ratios().length, 2);
      // A suspended context is not a slow one: the interval starts over
      wallMs += 3000;
      feed(Math.ceil(6000 / hopMs));
      assert.equal(ratios().length, 3);
      for (const ratio of ratios()) {
        assert.ok(Math.abs(ratio - 0.98) < 0.005, `ratio ${ratio}`);
      }
    } finally {
      Object.defineProperty(globalThis, 'performance', descriptor);
    }
  });

  test('posts no clock without clockReports', async () => {
    const live = await listen();
    stream(live.processor, clock, new Float32Array(6 * SAMPLE_RATE), SAMPLE_RATE);
    assert.ok(!worker.messages.some((message) => message.type === 'clock'));
  });
});
