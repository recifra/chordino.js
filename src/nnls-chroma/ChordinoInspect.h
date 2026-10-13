/* -*- c-basic-offset: 4 indent-tabs-mode: nil -*-  vi:set ts=8 sts=4 sw=4: */

/*
  Inspection hooks for internal visualization (examples/player and
  examples/realtime).

  Only compiled when CHORDINO_INSPECT is defined, which the separate
  chordino-inspect.js build does (see BUILD.md). Without it Chordino.cpp
  compiles to exactly what ships in chordino.js. The hooks are free functions so
  the Chordino class keeps its signature; src/inspect.cpp implements them.
*/

#ifndef _CHORDINO_INSPECT_
#define _CHORDINO_INSPECT_

#ifdef CHORDINO_INSPECT

#include <string>
#include <vector>

// Called once per getRemainingFeatures(), before the first frame.
void chordinoInspectBegin(const std::vector<std::string> &chordNames);

// Called for every frame, in order. Time is in seconds rather than a RealTime.
//  chroma:         24 bins, bass (0-11) stacked under treble (12-23), A-based
//  smoothedChroma: the 24 bins the chord matcher actually scores
//  salience:       one value per chord in chordNames, after the confidence gate
//                  (what the Viterbi decoder receives)
//  semitones:      84 semitone activations, A0 upwards
//  gate:           confidence gate outcome: 0 passed, 1 the best chord did not
//                  beat N by enough, 2 a chord on another root came too close
// chroma and semitones are null for the frames an earlier call already ran NNLS
// on (see Chordino::m_rawChromas): they are only worked out once.
void chordinoInspectFrame(double seconds,
                          const std::vector<float> *chroma,
                          const std::vector<double> &smoothedChroma,
                          const std::vector<double> &salience,
                          const float *semitones,
                          int gate);

#endif

#endif
