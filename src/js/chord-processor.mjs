// Realtime chord detection in an AudioWorklet. Worklets cannot fetch, so the
// page hands over the bytes of chordino.wasm, which the worker in
// chord-worker.mjs shares:
//
//   const wasmBinary = await (await fetch(new URL('chordino.wasm', lib))).arrayBuffer();
//   await context.audioWorklet.addModule(new URL('chord-processor.mjs', lib));
//   const node = new AudioWorkletNode(context, 'chord-processor', { processorOptions: { wasmBinary } });
//
// It posts { type: 'ready' } once the wasm is up (audio before that is skipped),
// { type: 'chordResult', result: { chord, time } } on every chord change,
// { type: 'error', message } when the wasm cannot load, and { type: 'stopped' }
// after it is sent { type: 'stop' }.

import createChordino from './chordino.mjs';

// Every node in this AudioWorkletGlobalScope shares one wasm instance
let chordino = null;

function loadChordino(wasmBinary) {
  chordino ??= new Promise((resolve, reject) => {
    createChordino({
      // The glue would fetch chordino.wasm itself, which worklets cannot do. It
      // has no failure path for this hook, hence the outer promise.
      instantiateWasm(imports, receiveInstance) {
        WebAssembly.instantiate(wasmBinary, imports).then(({ instance }) => receiveInstance(instance), reject);
        return {};
      },
    }).then(resolve, reject);
  }).catch((err) => {
    // Let the next node try again, with other bytes
    chordino = null;
    throw err;
  });
  return chordino;
}

class ChordProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();
    this.stopped = false;
    this.module = undefined;
    this.instance = undefined;
    this.bufferIndex = 0;
    this.bufferSize = 4096;
    this.lastChord = undefined;
    this.lastChangeTimeMs = Math.trunc(currentTime * 1000);
    // Don't report while the input is near-silent, so the matcher's best fit to
    // the noise floor is never shown as a chord.
    this.silenceRmsThreshold = 0.01;
    // How much audio each read decodes: enough for the HMM/Viterbi smoothing to
    // settle, bounded so the cost per getPartialResult() call stays flat however
    // long the session runs.
    this.windowMs = 10000;
    this.bufferStartMs = 0;
    this.audioBuffer = new Float32Array(this.bufferSize);
    this.port.onmessage = this.handleMessage.bind(this);
    this.ready = loadChordino(options?.processorOptions?.wasmBinary).then(
      (module) => {
        if (this.stopped) {
          return;
        }
        this.module = module;
        this.pBuffer = module._malloc(this.bufferSize * Float32Array.BYTES_PER_ELEMENT);
        this.instance = module.initializePlugin(sampleRate, this.bufferSize);
        this.port.postMessage({ type: 'ready' });
      },
      (err) => {
        this.port.postMessage({ type: 'error', message: String(err?.message ?? err) });
      },
    );
  }

  async handleMessage(event) {
    if (event.data.type === 'stop') {
      this.stopped = true;
    }
  }

  process(inputs, _outputs, _parameters) {
    if (!this.instance) {
      if (this.stopped) {
        this.port.postMessage({ type: 'stopped' });
      }
      return !this.stopped;
    }
    const input = inputs[0];
    if (input && input.length > 0) {
      /** @type {Float32Array} */
      const channelData = input[0];
      const currentTimeMs = Math.trunc(currentTime * 1000);

      // Accumulate data
      if (this.bufferIndex === 0) {
        this.bufferStartMs = currentTime * 1000;
      }
      const toCopy = Math.min(channelData.length, this.bufferSize - this.bufferIndex);
      this.audioBuffer.set(channelData.subarray(0, toCopy), this.bufferIndex);
      this.bufferIndex += toCopy;

      if (this.bufferIndex >= this.bufferSize) {
        let sumSq = 0;
        for (let i = 0; i < this.bufferSize; i++) {
          sumSq += this.audioBuffer[i] * this.audioBuffer[i];
        }
        const rms = Math.sqrt(sumSq / this.bufferSize);

        // Every buffer goes in, quiet ones too, as in the offline extraction:
        // leaving one out would splice the audio around it and put Chordino's
        // clock behind currentTime.
        this.module.HEAPF32.set(this.audioBuffer, this.pBuffer >> 2);
        this.module.processBuffer(this.instance, this.pBuffer, Math.trunc(this.bufferStartMs));

        if (rms >= this.silenceRmsThreshold) {
          // Read back the results, re-decoding the last windowMs each call. Not
          // getResult: that one is for the end of the audio, and pads what is
          // still buffered with zeros the next buffers would then follow.
          this.module.forgetBefore(this.instance, currentTimeMs - this.windowMs);
          const chordList = this.module.getPartialResult(this.instance);

          // The last segment can still be revised as more frames arrive, so treat
          // the second-to-last one as the confirmed/stable reading. Ignore it when
          // it's 'N': the EMA smoothing and extension penalty already do the heavy
          // lifting on the C++ side, so a stray N here is just noise, not silence.
          if (chordList.length >= 2) {
            const chordItem = chordList[chordList.length - 2];

            if (chordItem.chord !== 'N' && this.lastChord !== chordItem.chord) {
              this.lastChord = chordItem.chord;
              this.lastChangeTimeMs = currentTimeMs;
              this.port.postMessage({ type: 'chordResult', result: chordItem });
            }
          }
        }
        // Reset buffer for next chunk (no overlap for simplicity)
        this.bufferIndex = 0;
      }
    }
    if (!this.stopped) {
      return true;
    }
    // Release memory
    this.module.releasePlugin(this.instance);
    this.instance = undefined;
    this.module._free(this.pBuffer);
    this.port.postMessage({ type: 'stopped' });
    return false;
  }
}

registerProcessor('chord-processor', ChordProcessor);
