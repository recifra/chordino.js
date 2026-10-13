// Types of chordino.mjs, the module src/build.sh compiles; src/chordextract.js
// puts this API over the C functions src/chordextract.c exports. Pointers
// into the wasm heap and plugin instances are plain numbers.

/** One chord Chordino read: its label ('N' for no chord) and when it starts, in seconds */
export interface Chord {
  chord: string;
  time: number;
}

export interface ChordinoModule {
  /**
   * A plugin instance, fed blockSize samples at a time. realtime gets each frame
   * out half a block sooner, at the cost of the first few. 0 when it fails.
   */
  initializePlugin(sampleRate: number, blockSize: number, realtime?: boolean): number;
  /** Feeds the blockSize samples at buffer, which start at milliseconds */
  processBuffer(instance: number, buffer: number, milliseconds: number): void;
  /** Chords of everything fed, padding what is still buffered with zeros: for the end of the audio */
  getResult(instance: number): Chord[];
  /** Chords of the frames so far, for a caller that goes on feeding */
  getPartialResult(instance: number): Chord[];
  /** Drops the frames before milliseconds, so getPartialResult decodes a bounded window */
  forgetBefore(instance: number, milliseconds: number): void;
  resetProcess(instance: number): void;
  releasePlugin(instance: number): void;
  /** Has getResult report its progress, from 0 to 1; null stops it */
  setProgressCallback(callback: ((done: number) => void) | null): void;
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
