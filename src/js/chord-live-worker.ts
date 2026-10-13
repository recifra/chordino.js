// Realtime chord detection, the half that runs Chordino: chord-processor.mjs, the
// AudioWorklet, gathers the microphone into 4096-sample buffers and hands them
// here over a MessagePort. Decoding takes far longer than a render quantum, and
// an audio thread that overruns its quanta makes iOS drop microphone samples to
// keep up, which raises the pitch of everything heard after; hence the split.
// A module worker; it loads chordino.wasm from next to chordino.mjs:
//
//   const worker = new Worker(new URL('chord-live-worker.mjs', lib), { type: 'module' });
//   await context.audioWorklet.addModule(new URL('chord-processor.mjs', lib));
//   const node = new AudioWorkletNode(context, 'chord-processor');
//   const { port1, port2 } = new MessageChannel();
//   worker.postMessage({ type: 'start', sampleRate: context.sampleRate, delayMs, port: port1 }, [port1]);
//   node.port.postMessage({ type: 'port', port: port2 }, [port2]);
//
// It posts { type: 'ready' } once the wasm is up (audio before that is skipped),
// { type: 'chordResult', result: { chord, time } } on every chord change (time
// is when the chord started, on the AudioContext clock; it is posted delayMs
// plus about 0.2 s later), { type: 'error', message } when it cannot start, and
// { type: 'stopped' } once { type: 'stop' } has released the plugin.
//
// delayMs (default 1000) is how far behind the newest audio the chord posted is
// read: less shows chords sooner, more makes them agree with what
// chord-worker.mjs finds in the same audio (about 95% of the time at 0, 99% at
// 1000).

import createChordino, { type Chord, type ChordinoModule } from './chordino.mjs';

/** A buffer chord-processor.mjs sends: the AudioContext time of its first sample, and the time it was sent at */
interface Block {
  samples: Float32Array;
  startMs: number;
  nowMs: number;
}

type WorkerMessage =
  | { type: 'ready' }
  | { type: 'chordResult'; result: Chord }
  | { type: 'error'; message: string }
  | { type: 'stopped' };

// The size of the buffers chord-processor.mjs sends
const BUFFER_SIZE = 4096;

let chordino: Promise<ChordinoModule> | null = null;
let session: Session | null = null;

onmessage = async ({ data }: MessageEvent) => {
  if (data?.type === 'start') {
    await start(data.port, data.sampleRate, data.delayMs ?? 1000);
  } else if (data?.type === 'stop') {
    stop();
    post({ type: 'stopped' });
  }
};

function post(message: WorkerMessage) {
  postMessage(message);
}

async function start(port: MessagePort | undefined, sampleRate: unknown, delayMs: number) {
  stop();
  let current: Session | null = null;
  try {
    if (typeof port?.postMessage !== 'function' || typeof sampleRate !== 'number' || !(sampleRate > 0)) {
      throw new Error('expected { type: "start", sampleRate, port }');
    }
    current = new Session(port, sampleRate, delayMs);
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

class Session {
  private readonly port: MessagePort;
  private readonly sampleRate: number;
  private readonly delayMs: number;
  private module: ChordinoModule | undefined;
  private instance = 0;
  private pBuffer = 0;
  private lastChord: string | undefined;
  // Don't report what was read where the input is near-silent, so the
  // matcher's best fit to the noise floor is never shown as a chord.
  private readonly silenceRmsThreshold = 0.01;
  // How much audio each read decodes: enough for the HMM/Viterbi smoothing to
  // settle, bounded so the cost per getPartialResult() call stays flat however
  // long the session runs.
  private readonly windowMs = 10000;
  // Quiet stretches of the input still in the window, as [startMs, endMs], and
  // the end of the last buffer that was not quiet.
  private readonly quietStretches: [startMs: number, endMs: number][] = [];
  private loudUntilMs = -Infinity;

  constructor(port: MessagePort, sampleRate: number, delayMs: number) {
    this.port = port;
    this.sampleRate = sampleRate;
    // How far behind the newest frame the chord shown is read. The newest frames
    // are the least settled ones: the chroma smoothing runs both ways and the
    // Viterbi path can still change there, so they need a little of the audio
    // after them to come out as they would in the offline extraction.
    this.delayMs = delayMs;
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
    this.pBuffer = module._malloc(BUFFER_SIZE * Float32Array.BYTES_PER_ELEMENT);
    // realtime: each frame comes out as soon as its own audio is in
    this.instance = module.initializePlugin(this.sampleRate, BUFFER_SIZE, true);
    this.module = module;
    if (!this.instance) {
      throw new Error(`Chordino did not start at ${this.sampleRate} Hz`);
    }
  }

  close() {
    this.port.onmessage = null;
    this.port.close();
    const { module, instance } = this;
    if (module) {
      if (instance) {
        module.releasePlugin(instance);
      }
      module._free(this.pBuffer);
    }
    this.module = undefined;
    this.instance = 0;
  }

  /** Feeds every buffer that came in, then reads once: a worker that fell behind catches up at once. */
  private receive(blocks: Block[]) {
    const { module, instance } = this;
    if (!module || !instance || blocks.length === 0) {
      return;
    }
    for (const block of blocks) {
      this.feed(module, instance, block);
    }
    this.read(module, instance, blocks[blocks.length - 1].nowMs);
  }

  private feed(module: ChordinoModule, instance: number, { samples, startMs, nowMs }: Block) {
    let sumSq = 0;
    for (let i = 0; i < samples.length; i++) {
      sumSq += samples[i] * samples[i];
    }
    const rms = Math.sqrt(sumSq / samples.length);
    const endMs = startMs + (samples.length / this.sampleRate) * 1000;
    const lastQuiet = this.quietStretches.at(-1);
    if (rms >= this.silenceRmsThreshold) {
      this.loudUntilMs = endMs;
    } else if (lastQuiet && startMs - lastQuiet[1] < 1) {
      lastQuiet[1] = endMs;
    } else {
      this.quietStretches.push([startMs, endMs]);
    }
    while (this.quietStretches[0]?.[1] < nowMs - this.windowMs) {
      this.quietStretches.shift();
    }

    // Every buffer goes in, quiet ones too, as in the offline extraction:
    // leaving one out would splice the audio around it and put Chordino's
    // clock behind the AudioContext's.
    module.HEAPF32.set(samples, this.pBuffer >> 2);
    module.processBuffer(instance, this.pBuffer, Math.trunc(startMs));
  }

  private read(module: ChordinoModule, instance: number, nowMs: number) {
    // Nothing to read once the input has been quiet for longer than the point
    // read lags behind it (frames come out about 0.2 s after their audio).
    if (this.loudUntilMs <= nowMs - this.delayMs - 1000) {
      return;
    }
    // Read back the results, re-decoding the last windowMs each call. Not
    // getResult: that one is for the end of the audio, and pads what is still
    // buffered with zeros the next buffers would then follow.
    module.forgetBefore(instance, nowMs - this.windowMs);
    const chordList = module.getPartialResult(instance);

    // The closing N Chordino adds sits on the newest frame; the chord read is
    // the one sounding delayMs before that. Ignore an N there: the EMA
    // smoothing and extension penalty already do the heavy lifting on the
    // C++ side, so a stray N here is just noise, not silence.
    const readAtMs = (chordList.at(-1)?.time ?? 0) * 1000 - this.delayMs;
    const chordItem = chordList.slice(0, -1).findLast((item) => item.time * 1000 <= readAtMs);
    const quiet = this.quietStretches.some(([start, end]) => start <= readAtMs && readAtMs < end);
    if (chordItem && !quiet && chordItem.chord !== 'N' && this.lastChord !== chordItem.chord) {
      this.lastChord = chordItem.chord;
      post({ type: 'chordResult', result: chordItem });
    }
  }
}
