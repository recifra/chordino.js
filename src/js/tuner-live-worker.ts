// Live pitch detection, for a tuner: chord-processor.mjs, the AudioWorklet,
// gathers the microphone into buffers and hands them here over a MessagePort,
// as it does for chord-live-worker.mjs, and YIN (pitch.c, in chordino.wasm)
// reads the newest 4096 samples. A module worker; it loads chordino.wasm from
// next to chordino.mjs:
//
//   const worker = new Worker(new URL('tuner-live-worker.mjs', lib), { type: 'module' });
//   await context.audioWorklet.addModule(new URL('chord-processor.mjs', lib));
//   const node = new AudioWorkletNode(context, 'chord-processor', { processorOptions: { bufferSize: 2048 } });
//   const { port1, port2 } = new MessageChannel();
//   worker.postMessage({ type: 'start', sampleRate: context.sampleRate, port: port1 }, [port1]);
//   node.port.postMessage({ type: 'port', port: port2 }, [port2]);
//
// It posts { type: 'ready' } once the wasm is up (audio before that is skipped),
// { type: 'pitch', frequency, time } for every steady reading (frequency in Hz,
// time at the end of the window read, in seconds on the AudioContext clock),
// { type: 'error', message } when it cannot start, and { type: 'stopped' } once
// { type: 'stop' } has released the detector.
//
// It reads once per batch the worklet sends, on the newest window. With
// buffers of 2048 samples and the worker keeping up, that is a reading every
// 2048 samples, the windows overlapping by half; a worker that falls behind
// skips the windows it missed instead of reading every one of them late.
// Nothing is posted while the window is near-silent, nor until 3 readings in a
// row agree within 20 cents; what is posted is the median of the last 5 that
// did, so a lone window that lost samples, and reads sharp, stays off it. The
// band to listen to is the page's: filter the input before the worklet.
//
// clockReports: true has it post { type: 'clock', ratio } about every 5 s:
// the AudioContext time the buffers covered over the time it took them to
// arrive. Well under 1, the audio thread is falling behind, the case in which
// samples go missing and pitch reads sharp.

import createChordino, { type ChordinoModule } from './chordino.mjs';

/** A buffer chord-processor.mjs sends: the AudioContext time of its first sample, and the time it was sent at */
interface Block {
  samples: Float32Array;
  startMs: number;
  nowMs: number;
}

type WorkerMessage =
  | { type: 'ready' }
  | { type: 'pitch'; frequency: number; time: number }
  | { type: 'clock'; ratio: number }
  | { type: 'error'; message: string }
  | { type: 'stopped' };

// Lags up to half of it: A0 (27.5 Hz) at 48 kHz
const WINDOW_SIZE = 4096;
// Under this RMS the window is taken for silence
const MIN_RMS = 0.01;
const STABLE_CENTS = 20;
const STABLE_READINGS = 3;
// Steady readings the frequency posted is the median of
const MEDIAN_READINGS = 5;
const CLOCK_REPORT_MS = 5000;
// Longer than this between batches, the context was suspended rather than
// slow, and the clock interval starts over
const CLOCK_GAP_MS = 1000;

let chordino: Promise<ChordinoModule> | null = null;
let session: Session | null = null;

onmessage = async ({ data }: MessageEvent) => {
  if (data?.type === 'start') {
    await start(data.port, data.sampleRate, data.clockReports === true);
  } else if (data?.type === 'stop') {
    stop();
    post({ type: 'stopped' });
  }
};

function post(message: WorkerMessage) {
  postMessage(message);
}

async function start(port: MessagePort | undefined, sampleRate: unknown, clockReports: boolean) {
  stop();
  let current: Session | null = null;
  try {
    if (typeof port?.postMessage !== 'function' || typeof sampleRate !== 'number' || !(sampleRate > 0)) {
      throw new Error('expected { type: "start", sampleRate, port }');
    }
    current = new Session(port, sampleRate, clockReports);
    session = current;
    chordino ??= createChordino().catch((err: unknown) => {
      chordino = null;
      throw err;
    });
    const module = await chordino;
    // Stopped, or started again, meanwhile
    if (session !== current) {
      return;
    }
    current.open(module);
    post({ type: 'ready' });
  } catch (err) {
    if (session !== current) {
      return;
    }
    stop();
    post({ type: 'error', message: String((err as Error | null)?.message ?? err) });
  }
}

function stop() {
  session?.close();
  session = null;
}

const cents = (frequency: number, reference: number) => 1200 * Math.log2(frequency / reference);

class Session {
  private readonly port: MessagePort;
  private readonly sampleRate: number;
  private readonly clockReports: boolean;
  private module: ChordinoModule | undefined;
  private pitch = 0;
  private pBuffer = 0;
  // The newest WINDOW_SIZE samples, filled from the end
  private readonly window = new Float32Array(WINDOW_SIZE);
  private filled = 0;
  // The last reading, how many in a row agreed with the one before, and the
  // steady ones the median is taken over
  private lastFrequency = 0;
  private agreeing = 0;
  private steady: number[] = [];
  // Where the clock interval started: AudioContext time, and performance.now()
  private clockAudioMs = 0;
  private clockWallMs = 0;
  private lastWallMs = -Infinity;

  constructor(port: MessagePort, sampleRate: number, clockReports: boolean) {
    this.port = port;
    this.sampleRate = sampleRate;
    this.clockReports = clockReports;
    // Answered from the start, wasm or not, as the worklet waits for each reply
    // before it sends more
    port.onmessage = ({ data }: MessageEvent<{ blocks: Block[] }>) => {
      try {
        this.receive(data.blocks);
      } catch (err) {
        stop();
        post({ type: 'error', message: String((err as Error | null)?.message ?? err) });
        return;
      }
      port.postMessage({ type: 'idle' });
    };
  }

  open(module: ChordinoModule) {
    this.pBuffer = module._malloc(WINDOW_SIZE * Float32Array.BYTES_PER_ELEMENT);
    this.pitch = module.createPitch(this.sampleRate, WINDOW_SIZE);
    this.module = module;
  }

  close() {
    this.port.onmessage = null;
    this.port.close();
    const { module, pitch } = this;
    if (module) {
      if (pitch) {
        module.releasePitch(pitch);
      }
      module._free(this.pBuffer);
    }
    this.module = undefined;
    this.pitch = 0;
  }

  /** Slides every buffer that came in into the window, then reads it once. */
  private receive(blocks: Block[]) {
    const { module, pitch } = this;
    if (!module || !pitch || blocks.length === 0) {
      return;
    }
    for (const { samples } of blocks) {
      this.slide(samples);
    }
    const last = blocks[blocks.length - 1];
    const endMs = last.startMs + (last.samples.length / this.sampleRate) * 1000;
    if (this.clockReports) {
      this.clock(endMs);
    }
    if (this.filled < WINDOW_SIZE) {
      return;
    }
    const frequency = this.read(module, pitch);
    if (frequency) {
      post({ type: 'pitch', frequency, time: endMs / 1000 });
    }
  }

  private slide(samples: Float32Array) {
    const { window } = this;
    if (samples.length >= WINDOW_SIZE) {
      window.set(samples.subarray(samples.length - WINDOW_SIZE));
    } else {
      window.copyWithin(0, samples.length);
      window.set(samples, WINDOW_SIZE - samples.length);
    }
    this.filled = Math.min(WINDOW_SIZE, this.filled + samples.length);
  }

  /** The frequency to post for the window, or 0 */
  private read(module: ChordinoModule, pitch: number) {
    module.HEAPF32.set(this.window, this.pBuffer >> 2);
    if (module.pitchRms(pitch, this.pBuffer) < MIN_RMS) {
      // Near-silent: the next note starts over
      this.agreeing = 0;
      this.steady = [];
      return 0;
    }
    const frequency = module.detectPitch(pitch, this.pBuffer);
    if (!(frequency > 0) || !Number.isFinite(frequency)) {
      return 0;
    }
    const agrees = this.lastFrequency > 0 && Math.abs(cents(frequency, this.lastFrequency)) < STABLE_CENTS;
    this.agreeing = agrees ? this.agreeing + 1 : this.lastFrequency > 0 ? 0 : 1;
    this.lastFrequency = frequency;
    return this.agreeing >= STABLE_READINGS ? this.median(frequency) : 0;
  }

  /**
   * Median of the last steady readings. A device under load drops microphone
   * samples now and then, and the window that lost them reads sharp; the
   * median keeps those lone readings off. A note settling far from the median
   * is a new one, and starts over.
   */
  private median(frequency: number) {
    if (this.steady.length > 0 && Math.abs(cents(frequency, this.middle())) >= STABLE_CENTS) {
      this.steady = [];
    }
    this.steady.push(frequency);
    if (this.steady.length > MEDIAN_READINGS) {
      this.steady.shift();
    }
    return this.middle();
  }

  private middle() {
    const sorted = [...this.steady].sort((a, b) => a - b);
    const middle = sorted.length >> 1;
    return sorted.length % 2 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2;
  }

  /** Posts the clock ratio once CLOCK_REPORT_MS have gone by since the interval started. */
  private clock(endMs: number) {
    const wallMs = performance.now();
    if (wallMs - this.lastWallMs > CLOCK_GAP_MS) {
      this.clockAudioMs = endMs;
      this.clockWallMs = wallMs;
    } else if (wallMs - this.clockWallMs >= CLOCK_REPORT_MS) {
      post({ type: 'clock', ratio: (endMs - this.clockAudioMs) / (wallMs - this.clockWallMs) });
      this.clockAudioMs = endMs;
      this.clockWallMs = wallMs;
    }
    this.lastWallMs = wallMs;
  }
}
