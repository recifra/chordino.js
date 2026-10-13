import assert from 'node:assert/strict';
import { before, describe, test } from 'node:test';

import { pathToFileURL } from 'node:url';

import { concat, decodeMp3, loadProcessor, paths, RENDER_QUANTUM, stream, synthChord } from './helpers.js';

const SAMPLE_RATE = 44100;

const detected = (processor) => processor.port.messages
  .filter((message) => message.type === 'chordResult')
  .map((message) => message.result.chord);

const root = (chord) => chord.match(/^[A-G][#b]?/)[0];

describe('realtime processor', () => {
  let ChordProcessor;
  let registered;
  let clock;
  let wasmBinary;

  before(async () => {
    ({ registered, clock, wasmBinary } = await loadProcessor(paths.processor, SAMPLE_RATE));
    ChordProcessor = registered['chord-processor'];
    assert.ok(ChordProcessor, 'chord-processor was not registered');
  });

  /** A processor as an AudioWorkletNode would create it, once its wasm is up. */
  async function createProcessor() {
    const processor = new ChordProcessor({ processorOptions: { wasmBinary } });
    await processor.ready;
    assert.deepEqual(processor.port.messages, [{ type: 'ready' }]);
    processor.port.messages.length = 0;
    return processor;
  }

  test('chords.mp3 goes through G, C, F', async () => {
    const { samples, sampleRate } = await decodeMp3(paths.chordsMp3);
    assert.equal(sampleRate, SAMPLE_RATE);
    const processor = await createProcessor();
    stream(processor, clock, samples, sampleRate);

    const chords = detected(processor);
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
    const processor = await createProcessor();
    stream(processor, clock, concat(...voicings.map((notes) => synthChord(notes, 2, SAMPLE_RATE))), SAMPLE_RATE);

    assert.deepEqual(detected(processor), ['C', 'Am', 'F', 'G']);
  });

  test('stays quiet on silence', async () => {
    const processor = await createProcessor();
    stream(processor, clock, new Float32Array(3 * SAMPLE_RATE), SAMPLE_RATE);

    assert.deepEqual(processor.port.messages, []);
  });

  test('stop releases the plugin and ends processing', async () => {
    const processor = await createProcessor();
    const quantum = [[new Float32Array(RENDER_QUANTUM)]];
    assert.equal(processor.process(quantum, [], {}), true);

    processor.port.send({ type: 'stop' });

    assert.equal(processor.process(quantum, [], {}), false);
    assert.equal(processor.instance, undefined);
    assert.deepEqual(processor.port.messages.at(-1), { type: 'stopped' });
  });

  test('ignores audio until the wasm is up', async () => {
    const processor = new ChordProcessor({ processorOptions: { wasmBinary } });
    const quantum = [[new Float32Array(RENDER_QUANTUM)]];
    assert.equal(processor.process(quantum, [], {}), true);

    await processor.ready;
    assert.ok(processor.instance, 'no plugin instance once ready');
  });

  test('reports wasm bytes it cannot load', async () => {
    // A fresh copy of the module: once one loads, its scope keeps that wasm
    await import(`${pathToFileURL(paths.processor).href}?fresh`);
    const FreshProcessor = registered['chord-processor'];
    assert.notEqual(FreshProcessor, ChordProcessor);
    const processor = new FreshProcessor({ processorOptions: { wasmBinary: new Uint8Array([0, 97, 115, 109]) } });
    await processor.ready;

    assert.equal(processor.port.messages.at(-1).type, 'error');
    assert.equal(processor.instance, undefined);
  });
});
