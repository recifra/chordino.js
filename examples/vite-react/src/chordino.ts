import { useCallback, useEffect, useRef, useState } from 'react';
// Vite bundles each worker with chordino.mjs and emits chordino.wasm once as an
// asset both of them load; the worklet only hands the audio on, so it has none
import ChordLiveWorker from 'chordino.js/chord-live-worker.mjs?worker';
import ChordWorker from 'chordino.js/chord-worker.mjs?worker';
import processorUrl from 'chordino.js/chord-processor.mjs?worker&url';

/** [chord, seconds] marks; '' is no chord, and the last one is ['', duration] */
export type Timestamps = [chord: string, seconds: number][];

/** BPM of the note the meter counts (the eighth in 6/8); null when there are no beats */
export type Tempo = { bpm: number; meter: [numerator: number, denominator: number] } | null;

type WorkerMessage =
  | { type: 'progress'; value: number }
  | { type: 'done'; result: { timestamps: Timestamps; tempo: Tempo } }
  | { type: 'error'; message: string };

type LiveWorkerMessage =
  | { type: 'ready' }
  | { type: 'chordResult'; result: { chord: string; time: number } }
  | { type: 'error'; message: string }
  | { type: 'stopped' };

// The rate recordings are decoded at, and the plugin initialised with
const SAMPLE_RATE = 44100;

export type DetectionState =
  | { status: 'idle' }
  | { status: 'decoding' }
  | { status: 'detecting'; progress: number }
  | { status: 'done'; timestamps: Timestamps; tempo: Tempo }
  | { status: 'error'; message: string };

/** Detects the chords of a whole recording (any format the browser decodes) in a Web Worker. */
export function useChordDetection() {
  const [state, setState] = useState<DetectionState>({ status: 'idle' });
  const workerRef = useRef<Worker | null>(null);

  useEffect(() => () => workerRef.current?.terminate(), []);

  const detect = useCallback(async (file: Blob) => {
    workerRef.current?.terminate();
    workerRef.current = null;
    setState({ status: 'decoding' });
    let samples: Float32Array<ArrayBuffer>;
    try {
      samples = await decode(await file.arrayBuffer());
    } catch (err) {
      setState({ status: 'error', message: `Could not decode: ${(err as Error).message}` });
      return;
    }
    const worker = new ChordWorker();
    workerRef.current = worker;
    const finish = (next: DetectionState) => {
      worker.terminate();
      if (workerRef.current === worker) {
        workerRef.current = null;
        setState(next);
      }
    };
    worker.onmessage = ({ data }: MessageEvent<WorkerMessage>) => {
      if (data.type === 'progress') {
        setState({ status: 'detecting', progress: data.value });
      } else if (data.type === 'done') {
        finish({ status: 'done', timestamps: data.result.timestamps, tempo: data.result.tempo });
      } else {
        finish({ status: 'error', message: data.message });
      }
    };
    worker.onerror = (event) => finish({ status: 'error', message: event.message || 'The worker failed' });
    worker.postMessage({ samples, sampleRate: SAMPLE_RATE }, [samples.buffer]);
  }, []);

  return { state, detect };
}

/** Decodes at SAMPLE_RATE and mixes down to mono; workers have no AudioContext to do it. */
async function decode(data: ArrayBuffer) {
  const buffer = await new OfflineAudioContext(1, 1, SAMPLE_RATE).decodeAudioData(data);
  const mono = new Float32Array(buffer.length);
  for (let channel = 0; channel < buffer.numberOfChannels; channel++) {
    const samples = buffer.getChannelData(channel);
    for (let i = 0; i < mono.length; i++) {
      mono[i] += samples[i] / buffer.numberOfChannels;
    }
  }
  return mono;
}

export type LiveState =
  | { status: 'idle' }
  | { status: 'starting' }
  | { status: 'listening'; chord: string | null }
  | { status: 'error'; message: string };

interface Session {
  stream: MediaStream;
  context: AudioContext;
  node: AudioWorkletNode;
  worker: Worker;
}

/**
 * Detects chords live from the microphone: an AudioWorklet hands the audio to a
 * Web Worker, which runs Chordino. delayMs trades latency for agreement with
 * useChordDetection (see the chordino.js README). Call start from a click or
 * the like, so the AudioContext is allowed to run.
 */
export function useLiveChords(delayMs = 1000) {
  const [state, setState] = useState<LiveState>({ status: 'idle' });
  const sessionRef = useRef<Session | null>(null);

  const stop = useCallback(async () => {
    const session = sessionRef.current;
    sessionRef.current = null;
    if (!session) {
      return;
    }
    session.stream.getTracks().forEach((track) => track.stop());
    session.node.port.postMessage({ type: 'stop' });
    // The plugin goes with the worker
    session.worker.terminate();
    await session.context.close();
  }, []);

  const start = useCallback(async () => {
    if (sessionRef.current) {
      return;
    }
    setState({ status: 'starting' });
    let stream: MediaStream | undefined;
    let context: AudioContext | undefined;
    let worker: Worker | undefined;
    try {
      // Voice processing is for speech: noise suppression takes sustained notes for
      // noise and cuts them, and gain control lifts the noise floor between them
      stream = await navigator.mediaDevices.getUserMedia({
        audio: { echoCancellation: false, noiseSuppression: false, autoGainControl: false },
      });
      context = new AudioContext();
      worker = new ChordLiveWorker();
      await context.audioWorklet.addModule(processorUrl);
      const node = new AudioWorkletNode(context, 'chord-processor', {
        channelCount: 1,
        channelCountMode: 'explicit',
        channelInterpretation: 'discrete',
      });
      worker.onmessage = ({ data }: MessageEvent<LiveWorkerMessage>) => {
        if (data.type === 'ready') {
          setState({ status: 'listening', chord: null });
        } else if (data.type === 'chordResult') {
          setState({ status: 'listening', chord: data.result.chord });
        } else if (data.type === 'error') {
          setState({ status: 'error', message: data.message });
          void stop();
        }
      };
      // The audio goes from the worklet straight to the worker
      const { port1, port2 } = new MessageChannel();
      worker.postMessage({ type: 'start', sampleRate: context.sampleRate, delayMs, port: port1 }, [port1]);
      node.port.postMessage({ type: 'port', port: port2 }, [port2]);
      context.createMediaStreamSource(stream).connect(node);
      node.connect(context.destination); // it outputs silence
      sessionRef.current = { stream, context, node, worker };
    } catch (err) {
      stream?.getTracks().forEach((track) => track.stop());
      worker?.terminate();
      void context?.close();
      setState({ status: 'error', message: (err as Error).message });
    }
  }, [delayMs, stop]);

  const stopAndReset = useCallback(async () => {
    await stop();
    setState({ status: 'idle' });
  }, [stop]);

  useEffect(() => () => void stop(), [stop]);

  return { state, start, stop: stopAndReset };
}
