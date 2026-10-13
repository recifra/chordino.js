// Realtime chord detection in an AudioWorklet. Worklets cannot fetch, so the
// page hands over the bytes of chordino.wasm, which the worker in
// chord-worker.mjs shares:
//
//   const wasmBinary = await (await fetch(new URL('chordino.wasm', lib))).arrayBuffer();
//   await context.audioWorklet.addModule(new URL('chord-processor.mjs', lib));
//   const node = new AudioWorkletNode(context, 'chord-processor', { processorOptions: { wasmBinary } });
//
// It posts { type: 'ready' } once the wasm is up (audio before that is skipped),
// { type: 'chordResult', result: { chord, time } } on every chord change (time
// is when the chord started, on the AudioContext clock; it is posted delayMs
// plus about 0.2 s later),
// { type: 'error', message } when the wasm cannot load, and { type: 'stopped' }
// after it is sent { type: 'stop' }.
//
// processorOptions.delayMs (default 1000) is how far behind the newest audio the
// chord posted is read: less shows chords sooner, more makes them agree with
// what chord-worker.mjs finds in the same audio (about 95% of the time at 0, 99%
// at 1000).

import createChordino, { type Chord, type ChordinoModule } from './chordino.mjs';

interface ChordProcessorOptions {
  wasmBinary?: BufferSource;
  delayMs?: number;
}

type ProcessorMessage =
  | { type: 'ready' }
  | { type: 'chordResult'; result: Chord }
  | { type: 'error'; message: string }
  | { type: 'stopped' };

// Every node in this AudioWorkletGlobalScope shares one wasm instance
let chordino: Promise<ChordinoModule> | null = null;

function loadChordino(wasmBinary: BufferSource | undefined): Promise<ChordinoModule> {
  chordino ??= new Promise<ChordinoModule>((resolve, reject) => {
    if (!wasmBinary) {
      throw new Error('expected the bytes of chordino.wasm in processorOptions.wasmBinary');
    }
    createChordino({
      // The glue would fetch chordino.wasm itself, which worklets cannot do. It
      // has no failure path for this hook, hence the outer promise.
      instantiateWasm(imports, receiveInstance) {
        WebAssembly.instantiate(wasmBinary, imports).then(({ instance }) => receiveInstance(instance), reject);
        return {};
      },
    }).then(resolve, reject);
  }).catch((err: unknown) => {
    // Let the next node try again, with other bytes
    chordino = null;
    throw err;
  });
  return chordino;
}

class ChordProcessor extends AudioWorkletProcessor {
  private stopped = false;
  private module: ChordinoModule | undefined;
  instance: number | undefined;
  private pBuffer = 0;
  private bufferIndex = 0;
  private readonly bufferSize = 4096;
  private lastChord: string | undefined;
  private lastChangeTimeMs = Math.trunc(currentTime * 1000);
  // Don't report what was read where the input is near-silent, so the
  // matcher's best fit to the noise floor is never shown as a chord.
  private readonly silenceRmsThreshold = 0.01;
  // How much audio each read decodes: enough for the HMM/Viterbi smoothing to
  // settle, bounded so the cost per getPartialResult() call stays flat however
  // long the session runs.
  private readonly windowMs = 10000;
  // How far behind the newest frame the chord shown is read. The newest frames
  // are the least settled ones: the chroma smoothing runs both ways and the
  // Viterbi path can still change there, so they need a little of the audio
  // after them to come out as they would in the offline extraction.
  private readonly delayMs: number;
  // Quiet stretches of the input still in the window, as [startMs, endMs], and
  // the end of the last buffer that was not quiet.
  private readonly quietStretches: [startMs: number, endMs: number][] = [];
  private loudUntilMs = -Infinity;
  private bufferStartMs = 0;
  private readonly audioBuffer = new Float32Array(this.bufferSize);
  /** Settles once the wasm is up, or has failed to load */
  readonly ready: Promise<void>;

  constructor(options?: { processorOptions?: ChordProcessorOptions }) {
    super();
    this.delayMs = options?.processorOptions?.delayMs ?? 1000;
    this.port.onmessage = this.handleMessage.bind(this);
    this.ready = loadChordino(options?.processorOptions?.wasmBinary).then(
      (module) => {
        if (this.stopped) {
          return;
        }
        this.module = module;
        this.pBuffer = module._malloc(this.bufferSize * Float32Array.BYTES_PER_ELEMENT);
        // realtime: each frame comes out as soon as its own audio is in
        this.instance = module.initializePlugin(sampleRate, this.bufferSize, true);
        this.post({ type: 'ready' });
      },
      (err: unknown) => {
        this.post({ type: 'error', message: String((err as Error | null)?.message ?? err) });
      },
    );
  }

  private post(message: ProcessorMessage) {
    this.port.postMessage(message);
  }

  private handleMessage(event: MessageEvent) {
    if (event.data.type === 'stop') {
      this.stopped = true;
    }
  }

  process(inputs: Float32Array[][], _outputs: Float32Array[][], _parameters: Record<string, Float32Array>): boolean {
    const { module, instance } = this;
    if (!module || !instance) {
      if (this.stopped) {
        this.post({ type: 'stopped' });
      }
      return !this.stopped;
    }
    const input = inputs[0];
    if (input && input.length > 0) {
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
        const bufferEndMs = this.bufferStartMs + (this.bufferSize / sampleRate) * 1000;
        const lastQuiet = this.quietStretches.at(-1);
        if (rms >= this.silenceRmsThreshold) {
          this.loudUntilMs = bufferEndMs;
        } else if (lastQuiet && this.bufferStartMs - lastQuiet[1] < 1) {
          lastQuiet[1] = bufferEndMs;
        } else {
          this.quietStretches.push([this.bufferStartMs, bufferEndMs]);
        }
        while (this.quietStretches[0]?.[1] < currentTimeMs - this.windowMs) {
          this.quietStretches.shift();
        }

        // Every buffer goes in, quiet ones too, as in the offline extraction:
        // leaving one out would splice the audio around it and put Chordino's
        // clock behind currentTime.
        module.HEAPF32.set(this.audioBuffer, this.pBuffer >> 2);
        module.processBuffer(instance, this.pBuffer, Math.trunc(this.bufferStartMs));

        // Nothing to read once the input has been quiet for longer than the point
        // read lags behind it (frames come out about 0.2 s after their audio).
        if (this.loudUntilMs > currentTimeMs - this.delayMs - 1000) {
          // Read back the results, re-decoding the last windowMs each call. Not
          // getResult: that one is for the end of the audio, and pads what is
          // still buffered with zeros the next buffers would then follow.
          module.forgetBefore(instance, currentTimeMs - this.windowMs);
          const chordList = module.getPartialResult(instance);

          // The closing N Chordino adds sits on the newest frame; the chord read is
          // the one sounding delayMs before that. Ignore an N there: the EMA
          // smoothing and extension penalty already do the heavy lifting on the
          // C++ side, so a stray N here is just noise, not silence.
          const readAtMs = (chordList.at(-1)?.time ?? 0) * 1000 - this.delayMs;
          const chordItem = chordList.slice(0, -1).findLast((item) => item.time * 1000 <= readAtMs);
          const quiet = this.quietStretches.some(([start, end]) => start <= readAtMs && readAtMs < end);
          if (chordItem && !quiet) {
            if (chordItem.chord !== 'N' && this.lastChord !== chordItem.chord) {
              this.lastChord = chordItem.chord;
              this.lastChangeTimeMs = currentTimeMs;
              this.post({ type: 'chordResult', result: chordItem });
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
    module.releasePlugin(instance);
    this.instance = undefined;
    module._free(this.pBuffer);
    this.post({ type: 'stopped' });
    return false;
  }
}

registerProcessor('chord-processor', ChordProcessor);
