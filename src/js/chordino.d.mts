// Types of chordino.mjs, the module src/build.sh compiles; src/chordextract.js
// puts this API over the C functions src/chordextract.c exports. Pointers
// into the wasm heap, plugin instances and pitch detectors are plain numbers.

/** One chord Chordino read: its label ('N' for no chord) and when it starts, in seconds */
export interface Chord {
  chord: string;
  time: number;
}

/** One beat: when it is, in seconds, and where it falls in its bar, from 1 for the downbeat */
export interface Beat {
  time: number;
  position: number;
}

/** The beats of a recording, with its tempo and time signature */
export interface Beats {
  /** Beats a minute, of the note the time signature counts: the eighth in 6/8 */
  bpm: number;
  /** Beats a bar */
  numerator: number;
  /** The note of a beat: 4 for the quarter, 8 for the eighth */
  denominator: number;
  beats: Beat[];
}

export interface ChordinoModule {
  /**
   * A plugin instance, fed blockSize samples at a time. realtime gets each frame
   * out half a block sooner, at the cost of the first few, and takes the steady
   * background of a live microphone, such as a hum, off each one. 0 when it fails.
   */
  initializePlugin(sampleRate: number, blockSize: number, realtime?: boolean): number;
  /** Feeds the blockSize samples at buffer, which start at milliseconds */
  processBuffer(instance: number, buffer: number, milliseconds: number): void;
  /** Chords of everything fed, padding what is still buffered with zeros: for the end of the audio */
  getResult(instance: number): Chord[];
  /**
   * Beats of the audio getResult read, found alongside its chords. null before
   * getResult, for a realtime instance, and when there is no beat to find.
   */
  getBeats(instance: number): Beats | null;
  /** Chords of the frames so far, for a caller that goes on feeding */
  getPartialResult(instance: number): Chord[];
  /** Drops the frames before milliseconds, so getPartialResult decodes a bounded window */
  forgetBefore(instance: number, milliseconds: number): void;
  resetProcess(instance: number): void;
  releasePlugin(instance: number): void;
  /** Has getResult report its progress, from 0 to 1; null stops it */
  setProgressCallback(callback: ((done: number) => void) | null): void;
  /**
   * A YIN pitch detector for windows of size samples (4096 reaches A0 at
   * 48 kHz). 0 unless size is even.
   */
  createPitch(sampleRate: number, size: number): number;
  /** Fundamental of the size samples at buffer, in Hz; 0 when YIN finds none */
  detectPitch(pitch: number, buffer: number): number;
  /** RMS of the size samples at buffer */
  pitchRms(pitch: number, buffer: number): number;
  releasePitch(pitch: number): void;
  _malloc(size: number): number;
  _free(pointer: number): void;
  /** The heap as floats; replaced when it grows, so read it again after every call */
  readonly HEAPF32: Float32Array;
}

export interface ChordinoOptions {
  /** Instantiates chordino.wasm in place of the glue fetching it; returns {} to go async */
  instantiateWasm?(
    imports: WebAssembly.Imports,
    receiveInstance: (instance: WebAssembly.Instance) => void,
  ): object;
}

export default function createChordino(options?: ChordinoOptions): Promise<ChordinoModule>;
