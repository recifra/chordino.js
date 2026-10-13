// Hands the microphone audio on to inspect-worker.js, in the same 4096-sample
// buffers chord-processor.mjs feeds Chordino, each with the AudioContext time of
// its first sample. The page sends it the worker's end of a MessageChannel as
// { type: 'port', port }, so the audio never goes through the main thread, and
// { type: 'stop' } to end it.

const BUFFER_SIZE = 4096;

class TapProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.target = null;
    this.stopped = false;
    this.buffer = new Float32Array(BUFFER_SIZE);
    this.index = 0;
    this.startMs = 0;
    this.port.onmessage = ({ data }) => {
      if (data.type === 'port') {
        this.target = data.port;
      } else if (data.type === 'stop') {
        this.stopped = true;
      }
    };
  }

  process(inputs) {
    if (this.stopped) {
      return false;
    }
    const channel = inputs[0]?.[0];
    if (channel && this.target) {
      if (this.index === 0) {
        this.startMs = currentTime * 1000;
      }
      const count = Math.min(channel.length, BUFFER_SIZE - this.index);
      this.buffer.set(channel.subarray(0, count), this.index);
      this.index += count;
      if (this.index === BUFFER_SIZE) {
        this.target.postMessage({ samples: this.buffer, startMs: this.startMs }, [this.buffer.buffer]);
        this.buffer = new Float32Array(BUFFER_SIZE);
        this.index = 0;
      }
    }
    return true;
  }
}

registerProcessor('tap-processor', TapProcessor);
