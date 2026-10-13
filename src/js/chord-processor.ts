// Realtime chord detection, the AudioWorklet half: it gathers the microphone into
// 4096-sample buffers and hands them to chord-live-worker.mjs, which runs
// Chordino; see there for how a page connects the two. Nothing here does more
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

const BUFFER_SIZE = 4096;
// Buffers kept for a worker that does not reply, about 11 s at 48 kHz; the
// oldest go first.
const MAX_PENDING = 128;

class ChordProcessor extends AudioWorkletProcessor {
  private worker: MessagePort | undefined;
  private stopped = false;
  // Waiting for the worker to reply to the buffers sent last
  private busy = false;
  private pending: Block[] = [];
  private buffer = new Float32Array(BUFFER_SIZE);
  private bufferIndex = 0;
  private bufferStartMs = 0;

  constructor() {
    super();
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
    if (this.bufferIndex === 0) {
      this.bufferStartMs = currentTime * 1000;
    }
    const toCopy = Math.min(channelData.length, BUFFER_SIZE - this.bufferIndex);
    this.buffer.set(channelData.subarray(0, toCopy), this.bufferIndex);
    this.bufferIndex += toCopy;
    if (this.bufferIndex >= BUFFER_SIZE) {
      this.pending.push({ samples: this.buffer, startMs: this.bufferStartMs, nowMs: Math.trunc(currentTime * 1000) });
      if (this.pending.length > MAX_PENDING) {
        this.pending.shift();
      }
      // A new one, as the last goes to the worker
      this.buffer = new Float32Array(BUFFER_SIZE);
      this.bufferIndex = 0;
      this.send();
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
