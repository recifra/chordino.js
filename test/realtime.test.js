import assert from 'node:assert/strict';
import { before, beforeEach, describe, test } from 'node:test';

import { concat, decodeMp3, loadLive, messageChannel, paths, RENDER_QUANTUM, stream, synthChord } from './helpers.js';

const SAMPLE_RATE = 44100;
const BUFFER_SIZE = 4096;

const root = (chord) => chord.match(/^[A-G][#b]?/)[0];

describe('realtime processor and worker', () => {
  let clock;
  let worker;
  let ChordProcessor;
  let start;

  before(async () => {
    ({ clock, worker, ChordProcessor, start } = await loadLive(SAMPLE_RATE));
    assert.ok(ChordProcessor, 'chord-processor was not registered');
  });

  beforeEach(() => {
    worker.messages.length = 0;
  });

  const detected = () => worker.messages
    .filter((message) => message.type === 'chordResult')
    .map((message) => message.result.chord);

  /** A processor connected to the worker, once the worker has its wasm up. */
  async function listen(options) {
    const live = await start(options);
    assert.deepEqual(worker.messages, [{ type: 'ready' }]);
    worker.messages.length = 0;
    return live;
  }

  test('the worklet only hands the audio on, in buffers sent once the worker is idle', () => {
    // No wasm and no Chordino on the audio thread: decoding there overran the
    // render quanta, and iOS then dropped microphone samples to catch up
    const processor = new ChordProcessor();
    const { port1, port2 } = messageChannel();
    const received = [];
    port1.onmessage = ({ data }) => received.push(data);
    processor.port.send({ type: 'port', port: port2 });

    const startTime = clock.currentTime;
    stream(processor, clock, synthChord(['C3', 'C4', 'E4', 'G4'], (3 * BUFFER_SIZE) / SAMPLE_RATE, SAMPLE_RATE), SAMPLE_RATE);

    assert.equal(received.length, 1, 'sent more before the worker was idle');
    const [first] = received[0].blocks;
    assert.equal(first.samples.length, BUFFER_SIZE);
    assert.ok(first.samples.some((sample) => sample !== 0), 'sent no audio');
    assert.ok(Math.abs(first.startMs - startTime * 1000) < 1e-6, `first buffer starts at ${first.startMs}`);

    port1.postMessage({ type: 'idle' });

    assert.equal(received.length, 2);
    const blocks = received[1].blocks;
    assert.equal(blocks.length, 2, 'the buffers gathered meanwhile go together');
    assert.ok(Math.abs(blocks[1].startMs - blocks[0].startMs - (BUFFER_SIZE / SAMPLE_RATE) * 1000) < 1e-6);
  });

  /** Every block a processor with these options sends for samples, the worker never replying until the end */
  function gather(options, samples) {
    const processor = new ChordProcessor(options);
    const { port1, port2 } = messageChannel();
    const blocks = [];
    port1.onmessage = ({ data }) => {
      blocks.push(...data.blocks);
      port1.postMessage({ type: 'idle' });
    };
    processor.port.send({ type: 'port', port: port2 });
    const startTime = clock.currentTime;
    stream(processor, clock, samples, SAMPLE_RATE);
    return { blocks, startTime };
  }

  for (const bufferSize of [2048, 1000]) {
    test(`processorOptions.bufferSize ${bufferSize} sends buffers of that size, every sample in order`, () => {
      // 1000 is not a multiple of the render quantum: buffers end partway
      // through one, and the rest of it starts the next
      const samples = Float32Array.from({ length: 10 * bufferSize + 300 }, (_, i) => i / 65536);
      const { blocks, startTime } = gather({ processorOptions: { bufferSize } }, samples);

      assert.equal(blocks.length, 10);
      blocks.forEach(({ samples: block, startMs }, n) => {
        assert.equal(block.length, bufferSize);
        assert.deepEqual(block, samples.subarray(n * bufferSize, (n + 1) * bufferSize), `block ${n}`);
        const expectedMs = (startTime + (n * bufferSize) / SAMPLE_RATE) * 1000;
        assert.ok(Math.abs(startMs - expectedMs) < 1e-6, `block ${n} starts at ${startMs}, not ${expectedMs}`);
      });
    });
  }

  test('rejects a bufferSize that is not a positive integer', () => {
    for (const bufferSize of [0, -2048, 2048.5, '2048']) {
      assert.throws(() => new ChordProcessor({ processorOptions: { bufferSize } }), RangeError);
    }
    assert.equal(gather({}, new Float32Array(BUFFER_SIZE)).blocks[0].samples.length, BUFFER_SIZE);
  });

  test('chords.mp3 goes through G, C, F', async () => {
    const { samples, sampleRate } = await decodeMp3(paths.chordsMp3);
    assert.equal(sampleRate, SAMPLE_RATE);
    const { processor } = await listen();
    stream(processor, clock, samples, sampleRate);

    const chords = detected();
    // The very first buffers have too little context to pin the chord type down
    // (e.g. G7M before settling on G), so only the root progression is asserted.
    const roots = chords.map(root).filter((r, i, all) => r !== all[i - 1]);
    assert.deepEqual(roots, ['G', 'C', 'F'], `detected ${chords.join(' ')}`);
    assert.equal(chords.at(-1), 'F');
  });

  test('follows a C Am F G progression', async () => {
    const voicings = [
      ['C3', 'C4', 'E4', 'G4'],
      ['A2', 'A3', 'C4', 'E4'],
      ['F2', 'A3', 'C4', 'F4'],
      ['G2', 'G3', 'B3', 'D4'],
    ];
    const { processor } = await listen();
    stream(processor, clock, concat(...voicings.map((notes) => synthChord(notes, 2, SAMPLE_RATE))), SAMPLE_RATE);

    assert.deepEqual(detected(), ['C', 'Am', 'F', 'G']);
  });

  test('keeps up when the worker falls behind, reading once for the buffers that came together', async () => {
    const samples = concat(synthChord(['C3', 'C4', 'E4', 'G4'], 3, SAMPLE_RATE), synthChord(['F2', 'A3', 'C4', 'F4'], 3, SAMPLE_RATE));
    const { processor, port } = await listen();
    port.hold();
    stream(processor, clock, samples, SAMPLE_RATE);

    const batches = [];
    const handler = port.onmessage;
    port.onmessage = (event) => {
      batches.push(event.data.blocks.length);
      handler(event);
    };
    while (port.flush() > 0);

    assert.deepEqual(batches, [1, Math.floor(samples.length / BUFFER_SIZE) - 1]);
    assert.equal(detected().at(-1), 'F');
  });

  test('posts a chord delayMs after its audio, with the time it started', async () => {
    // Frames come out about 0.2 s after their audio, on top of delayMs. With no
    // delay the newest frames also have to settle before the change shows.
    const samples = concat(synthChord(['A2', 'A3', 'C4', 'E4'], 2, SAMPLE_RATE), synthChord(['F2', 'A3', 'C4', 'F4'], 3, SAMPLE_RATE));
    const posted = {};
    const postMessage = globalThis.postMessage;
    try {
      for (const delayMs of [0, 1000]) {
        const { processor } = await listen({ delayMs });
        const startTime = clock.currentTime;
        globalThis.postMessage = (data) => {
          if (data.type === 'chordResult' && data.result.chord === 'F') {
            posted[delayMs] = clock.currentTime - startTime;
            assert.ok(Math.abs(data.result.time - startTime - 2) < 0.15, `F starts at ${data.result.time - startTime}`);
          }
        };
        stream(processor, clock, samples, SAMPLE_RATE);
        globalThis.postMessage = postMessage;
      }
    } finally {
      globalThis.postMessage = postMessage;
    }

    assert.ok(posted[0] - 2 < 0.8, `F posted ${posted[0] - 2}s after it started with delayMs 0`);
    assert.ok(Math.abs(posted[1000] - 2 - 1.2) < 0.15, `F posted ${posted[1000] - 2}s after it started with delayMs 1000`);
  });

  test('stays quiet on silence', async () => {
    const { processor } = await listen();
    stream(processor, clock, new Float32Array(3 * SAMPLE_RATE), SAMPLE_RATE);

    assert.deepEqual(worker.messages, []);
  });

  test('stop ends the worklet and releases the plugin', async () => {
    const { processor, port } = await listen();
    const quantum = [[new Float32Array(RENDER_QUANTUM)]];
    assert.equal(processor.process(quantum, [], {}), true);

    processor.port.send({ type: 'stop' });
    assert.equal(processor.process(quantum, [], {}), false);

    await worker.send({ type: 'stop' });
    assert.deepEqual(worker.messages, [{ type: 'stopped' }]);
    assert.ok(port.closed, 'the worker kept its end of the channel open');
  });

  test('skips the audio from before the wasm is up', async () => {
    const processor = new ChordProcessor();
    const { port1, port2 } = messageChannel();
    processor.port.send({ type: 'port', port: port2 });
    const starting = worker.send({ type: 'start', sampleRate: SAMPLE_RATE, port: port1 });
    // Answered at once, so the worklet goes on sending
    stream(processor, clock, synthChord(['C3', 'C4', 'E4', 'G4'], 3, SAMPLE_RATE), SAMPLE_RATE);
    await starting;

    assert.deepEqual(worker.messages, [{ type: 'ready' }]);
    worker.messages.length = 0;
    stream(processor, clock, synthChord(['F2', 'A3', 'C4', 'F4'], 3, SAMPLE_RATE), SAMPLE_RATE);
    assert.deepEqual(detected(), ['F']);
  });

  test('reports a start it cannot run', async () => {
    const { port1 } = messageChannel();
    await worker.send({ type: 'start', port: port1 });

    assert.equal(worker.messages.at(-1).type, 'error');
  });
});
