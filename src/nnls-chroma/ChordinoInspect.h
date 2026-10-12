/* -*- c-basic-offset: 4 indent-tabs-mode: nil -*-  vi:set ts=8 sts=4 sw=4: */

/*
  Inspection hooks for internal visualization (examples/player).

  Only compiled when CHORDINO_INSPECT is defined, which the separate
  chordino-inspect.js build does (see src/BUILD.md). Without it Chordino.cpp
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

// Called for every frame, in order. Time is in seconds rather than a RealTime,
// which lives in a different namespace on the plugin and the host side.
//  chroma:         24 bins, bass (0-11) stacked under treble (12-23), A-based
//  smoothedChroma: the 24 bins the chord matcher actually scores
//  salience:       one value per chord in chordNames, after the confidence gate
//                  (what the Viterbi decoder receives)
//  semitones:      84 semitone activations, A0 upwards
void chordinoInspectFrame(double seconds,
                          const std::vector<float> &chroma,
                          const std::vector<double> &smoothedChroma,
                          const std::vector<double> &salience,
                          const float *semitones);

#endif

#endif
