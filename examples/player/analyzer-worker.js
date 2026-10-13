// Runs the inspect build off the main thread: a few minutes of audio take
// seconds to analyze, which would otherwise freeze the page.
importScripts('./chordino-inspect.js');

const BUFFER_SIZE = 4096;
const OUTPUTS = ['chroma', 'smoothedchroma', 'chordsalience', 'semitonespectrum'];
// Share of the time spent feeding the audio in, as in src/js/chord-worker.ts
const FEED_SHARE = 0.25;

onmessage = ({ data: { samples, sampleRate } }) => {
  const pBuffer = Module._malloc(BUFFER_SIZE * Float32Array.BYTES_PER_ELEMENT);
  const instance = Module.initializePlugin(sampleRate, BUFFER_SIZE);
  // getAnalysis reports once per whole percent of its own part
  Module.setProgressCallback((done) => postMessage({ type: 'progress', value: FEED_SHARE + (1 - FEED_SHARE) * done }));
  try {
    // Same feeding loop as src/js/chord-worker.ts
    const block = new Float32Array(BUFFER_SIZE);
    const blocks = Math.ceil(samples.length / BUFFER_SIZE);
    const progressEvery = Math.max(1, Math.floor(blocks / 100));
    for (let i = 0; i < blocks; i++) {
      const offset = i * BUFFER_SIZE;
      block.fill(0);
      block.set(samples.subarray(offset, offset + BUFFER_SIZE));
      HEAPF32.set(block, pBuffer >> 2);
      Module.processBuffer(instance, pBuffer, Math.trunc((offset / sampleRate) * 1000));
      if (i % progressEvery === 0) {
        postMessage({ type: 'progress', value: FEED_SHARE * (i / blocks) });
      }
    }
    // Chordino does the NNLS, chord matching and Viterbi decoding all in here
    const analysis = Module.getAnalysis(instance);
    const transfer = [analysis.times.buffer, analysis.gate.buffer, ...OUTPUTS.map((output) => analysis[output].values.buffer)];
    postMessage({ type: 'done', analysis }, transfer);
  } catch (err) {
    postMessage({ type: 'error', message: String(err) });
  } finally {
    Module.setProgressCallback(null);
    Module.releasePlugin(instance);
    Module._free(pBuffer);
  }
};
