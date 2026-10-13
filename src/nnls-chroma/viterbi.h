/* -*- c-basic-offset: 4 indent-tabs-mode: nil -*-  vi:set ts=8 sts=4 sw=4: */

/*
  NNLS-Chroma / Chordino

  Audio feature extraction plugins for chromagram and chord
  estimation.

  Centre for Digital Music, Queen Mary University of London.
  This file copyright 2008-2010 Matthias Mauch and QMUL.

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License as
  published by the Free Software Foundation; either version 2 of the
  License, or (at your option) any later version.  See the file
  COPYING included with this distribution for more information.
*/

#ifndef _VITERBI_H_
#define _VITERBI_H_

#ifdef __cplusplus
extern "C" {
#endif

// The most likely path through nState states over nFrame frames, given the
// initial probabilities init[nState], the transition probabilities
// trans[from * nState + to] and the observations obs[frame * nState + state].
// Writes the forward probabilities, scaled to sum to 1 in every frame, to
// delta[frame * nState + state], the scale of each frame to scale[nFrame] and
// the path to path[nFrame].
void viterbi_path(int nState, int nFrame, const double *init, const double *trans,
                  const double *obs, double *delta, double *scale, int *path);

#ifdef __cplusplus
}
#endif

#endif
