class ChordProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.stopped = false;
    this.module = Module;
    this.lastChangeTimeMs = 0;
    this.bufferSize = 4096;
    this.lastChord = undefined;
    this.pBuffer = this.module._malloc(this.bufferSize * Float32Array.BYTES_PER_ELEMENT);
    this.instance = this.module.initializePlugin(44000, this.bufferSize);
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
      const currentTimeMs = Math.trunc(_parameters.currentTime * 1000);

      // Slice to bufferSize
      const paddedData = channelData.slice(0, this.bufferSize);
      HEAPF32.set(paddedData, this.pBuffer >> 2);

      // Process data
      this.module.processBuffer(this.instance, this.pBuffer, currentTimeMs);

      // Read back the results
      const chordList = this.module.getResult(this.instance);

      const ellapsedTime = currentTimeMs - this.lastChangeTimeMs;

      if (chordList.length <= 2 && ellapsedTime > 250) {
        this.lastChangeTimeMs = currentTimeMs;
      }
      if (chordList.length > 2) {
        this.module.resetProcess(this.instance);
        this.lastChangeTimeMs = currentTimeMs;
      }
      if (chordList.length > 2 && this.lastChord !== chordList[1].chord) {
        const chordItem = chordList[1];
        this.lastChord = chordItem.chord;
        this.port.postMessage({ type: 'chordResult', result: chordItem });
      }
    }
    if (!this.stopped) {
      return true;
    }
    this.port.postMessage({ type: 'stopping' });
    // Release memory
    this.module.releasePlugin(this.instance);
    this.instance = undefined;
    this.module._free(this.pBuffer);
    return false;
  }
}

registerProcessor('chord-processor', ChordProcessor);