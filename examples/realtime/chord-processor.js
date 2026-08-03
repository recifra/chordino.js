class ChordProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.stopped = false;
    this.module = Module;
    this.bufferIndex = 0;
    this.bufferSize = 4096;
    this.lastChord = undefined;
    this.lastChangeTimeMs = Math.trunc(currentTime * 1000);
    // Skip decoding near-silent/noise-floor buffers entirely instead of letting
    // the matcher try to fit noise to a chord template.
    this.silenceRmsThreshold = 0.01;
    // Keep enough history for the HMM/Viterbi smoothing to stabilize, but bounded
    // so cost per getResult() call doesn't grow for the whole session.
    this.resetIntervalMs = 3000;
    this.lastResetTimeMs = this.lastChangeTimeMs;
    this.pBuffer = this.module._malloc(this.bufferSize * Float32Array.BYTES_PER_ELEMENT);
    this.instance = this.module.initializePlugin(sampleRate, this.bufferSize);
    this.audioBuffer = new Float32Array(this.bufferSize);
    this.port.onmessage = this.handleMessage.bind(this);
  }

  async handleMessage(event) {
    if (event.data.type === 'stop') {
      this.stopped = true;
    }
  }

  process(inputs, _outputs, _parameters) {
    if (!this.instance) {
      return !this.stopped;
    }
    const input = inputs[0];
    if (input && input.length > 0) {
      /** @type {Float32Array} */
      const channelData = input[0];
      const currentTimeMs = Math.trunc(currentTime * 1000);

      // Accumulate data
      const toCopy = Math.min(channelData.length, this.bufferSize - this.bufferIndex);
      this.audioBuffer.set(channelData.subarray(0, toCopy), this.bufferIndex);
      this.bufferIndex += toCopy;

      if (this.bufferIndex >= this.bufferSize) {
        let sumSq = 0;
        for (let i = 0; i < this.bufferSize; i++) {
          sumSq += this.audioBuffer[i] * this.audioBuffer[i];
        }
        const rms = Math.sqrt(sumSq / this.bufferSize);

        if (rms < this.silenceRmsThreshold) {
          // Nothing worth analyzing: don't feed noise into the matcher.
        } else {
          // Process the full buffer
          HEAPF32.set(this.audioBuffer, this.pBuffer >> 2);

          // Process data
          this.module.processBuffer(this.instance, this.pBuffer, currentTimeMs);

          // Read back the results (re-decodes the whole buffered history each call,
          // so it must stay bounded by periodic resets below)
          const chordList = this.module.getResult(this.instance);

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

          // Bound how much history getResult() has to re-decode: reset periodically
          // instead of every chunk, so there's still enough context to smooth.
          if (currentTimeMs - this.lastResetTimeMs > this.resetIntervalMs) {
            this.module.resetProcess(this.instance);
            this.lastResetTimeMs = currentTimeMs;
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