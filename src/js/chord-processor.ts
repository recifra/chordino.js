// Realtime chord detection, the AudioWorklet half: it gathers the microphone into
// buffers and hands them to chord-live-worker.mjs, which runs Chordino; see
// there for how a page connects the two. The buffers are 4096 samples long
// unless processorOptions.bufferSize says otherwise. It feeds the tuner,
// tuner-live-worker.mjs, the same way, in buffers of 2048. Nothing here does more
// than copy. Decoding takes far longer than a render quantum, and an audio thread
// that overruns its quanta makes iOS drop microphone samples to keep up, which
// raises the pitch of everything heard after.
//
// The page sends it { type: 'port', port }, the worker's end of a MessageChannel,
// so the audio never goes through the main thread, and { type: 'stop' } to end
// it. It sends the worker every buffer gathered since the worker's last reply,
// so a worker that falls behind gets them together and reads once for all of
// them, instead of lagging a little further with every buffer.

/** A buffer as chord-live-worker.mjs takes it: the AudioContext time of its first sample, and the time it was sent at */
interface Block {
  samples: Float32Array<ArrayBuffer>;
  startMs: number;
  nowMs: number;
}

const DEFAULT_BUFFER_SIZE = 4096;
// Buffers kept for a worker that does not reply, about 11 s of 4096 samples at
// 48 kHz; the oldest go first.
const MAX_PENDING = 128;

class ChordProcessor extends AudioWorkletProcessor {
  private worker: MessagePort | undefined;
  private stopped = false;
  // Waiting for the worker to reply to the buffers sent last
  private busy = false;
  private pending: Block[] = [];
  private readonly bufferSize: number;
  private buffer: Float32Array<ArrayBuffer>;
  private bufferIndex = 0;
  private bufferStartMs = 0;

  constructor(options?: { processorOptions?: { bufferSize?: number } }) {
    super();
    const bufferSize = options?.processorOptions?.bufferSize ?? DEFAULT_BUFFER_SIZE;
    if (!Number.isInteger(bufferSize) || bufferSize <= 0) {
      throw new RangeError(`bufferSize must be a positive integer, not ${bufferSize}`);
    }
    this.bufferSize = bufferSize;
    this.buffer = new Float32Array(bufferSize);
    this.port.onmessage = ({ data }: MessageEvent) => {
      if (data.type === 'port') {
        this.worker = data.port as MessagePort;
        this.worker.onmessage = () => {
          this.busy = false;
          this.send();
        };
      } else if (data.type === 'stop') {
        this.stopped = true;
      }
    };
  }

  process(inputs: Float32Array[][], _outputs: Float32Array[][], _parameters: Record<string, Float32Array>): boolean {
    if (this.stopped) {
      this.worker?.close();
      return false;
    }
    const channelData = inputs[0]?.[0];
    if (!this.worker || !channelData) {
      return true;
    }
    // A buffer can end partway through the quantum, when bufferSize is not a
    // multiple of it; the rest starts the next one.
    for (let offset = 0; offset < channelData.length;) {
      if (this.bufferIndex === 0) {
        this.bufferStartMs = (currentTime + offset / sampleRate) * 1000;
      }
      const toCopy = Math.min(channelData.length - offset, this.bufferSize - this.bufferIndex);
      this.buffer.set(channelData.subarray(offset, offset + toCopy), this.bufferIndex);
      this.bufferIndex += toCopy;
      offset += toCopy;
      if (this.bufferIndex >= this.bufferSize) {
        this.pending.push({ samples: this.buffer, startMs: this.bufferStartMs, nowMs: Math.trunc(currentTime * 1000) });
        if (this.pending.length > MAX_PENDING) {
          this.pending.shift();
        }
        // A new one, as the last goes to the worker
        this.buffer = new Float32Array(this.bufferSize);
        this.bufferIndex = 0;
        this.send();
      }
    }
    return true;
  }

  private send() {
    if (this.busy || !this.worker || this.pending.length === 0) {
      return;
    }
    const blocks = this.pending;
    this.pending = [];
    this.busy = true;
    this.worker.postMessage({ blocks }, blocks.map((block) => block.samples.buffer));
  }
}

registerProcessor('chord-processor', ChordProcessor);
