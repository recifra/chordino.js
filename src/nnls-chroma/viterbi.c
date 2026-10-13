#include "viterbi.h"

#include <stdbool.h>
#include <stdlib.h>

// True when every state stays put with one probability and moves to each other
// state with another, the same for all of them (Chordino's chord transitions).
static bool uniformTransitions(int nState, const double *trans, double *stay, double *move) {
    if (nState < 2) return false;
    *stay = trans[0];
    *move = trans[1];
    for (int iState = 0; iState < nState; ++iState) {
        for (int jState = 0; jState < nState; ++jState) {
            if (trans[iState * nState + jState] != (iState == jState ? *stay : *move)) return false;
        }
    }
    return true;
}

void viterbi_path(int nState, int nFrame, const double *init, const double *trans,
                  const double *obs, double *delta, double *scale, int *path) {
    int *psi = malloc(nFrame * nState * sizeof(int)); //  "matrix" of remembered indices of the best transitions
    for (int iFrame = 0; iFrame < nFrame; ++iFrame) path[iFrame] = nState-1; // the final output path (current assignment arbitrary, makes sense only for Chordino, where nChord-1 is the "no chord" label)

    double deltasum = 0;

    /* initialise first frame */
    for (int iState = 0; iState < nState; ++iState) {
        delta[iState] = init[iState] * obs[iState];
        deltasum += delta[iState];
    }
    for (int iState = 0; iState < nState; ++iState) delta[iState] /= deltasum; // normalise (scale)
    scale[0] = 1.0/deltasum;
    for (int iState = 0; iState < nState; ++iState) psi[iState] = 0;

    /* With uniform transitions the best previous state of j is either j itself or
       the best of all the others, the same for every j, so each frame takes
       O(nState) instead of scanning every pair. Both paths compute the same products
       and keep the lowest index on a tie, so they decode the same path. */
    double stay = 0, move = 0;
    bool uniform = uniformTransitions(nState, trans, &stay, &move);
    double *moved = uniform ? malloc(nState * sizeof(double)) : NULL;

    /* rest of the forward step */
    for (int iFrame = 1; iFrame < nFrame; ++iFrame) {
        deltasum = 0;
        const double *previous = delta + (iFrame-1) * nState;
        const double *observed = obs + iFrame * nState;
        int best = 0; // best state to move from, then the best one other than it
        int runnerUp = 1;
        if (uniform) {
            for (int iState = 0; iState < nState; ++iState) {
                moved[iState] = previous[iState] * move;
                if (moved[iState] > moved[best]) best = iState;
            }
            runnerUp = best == 0 ? 1 : 0;
            for (int iState = 0; iState < nState; ++iState) {
                if (iState != best && moved[iState] > moved[runnerUp]) runnerUp = iState;
            }
        }
        /* every state wants to know which previous state suits it best */
        for (int jState = 0; jState < nState; ++jState) {
            int bestState = nState - 1;
            double bestValue = 0;
            if (observed[jState] > 0) {
                if (uniform) {
                    int other = jState == best ? runnerUp : best;
                    // the two candidates, lowest index first
                    int candidates[2] = { jState < other ? jState : other, jState < other ? other : jState };
                    for (int k = 0; k < 2; ++k) {
                        int iState = candidates[k];
                        double currentValue = iState == jState ? previous[iState] * stay : moved[iState];
                        if (currentValue > bestValue) {
                            bestValue = currentValue;
                            bestState = iState;
                        }
                    }
                } else {
                    for (int iState = 0; iState < nState; ++iState) {
                        double currentValue = previous[iState] * trans[iState * nState + jState];
                        if (currentValue > bestValue) {
                            bestValue = currentValue;
                            bestState = iState;
                        }
                    }
                }
            }

            delta[iFrame * nState + jState] = bestValue * observed[jState];
            deltasum += delta[iFrame * nState + jState];
            psi[iFrame * nState + jState] = bestState;
        }
        if (deltasum > 0) {
            for (int iState = 0; iState < nState; ++iState) {
                delta[iFrame * nState + iState] /= deltasum; // normalise (scale)
            }
            scale[iFrame] = 1.0/deltasum;
        } else {
            for (int iState = 0; iState < nState; ++iState) {
                delta[iFrame * nState + iState] = 1.0/nState;
            }
            scale[iFrame] = 1.0;
        }

    }

    /* initialise backward step */
    double bestValue = 0;
    for (int iState = 0; iState < nState; ++iState) {
        double currentValue = delta[(nFrame-1) * nState + iState];
        if (currentValue > bestValue) {
            bestValue = currentValue;
            path[nFrame-1] = iState;
        }
    }

    /* rest of backward step */
    for (int iFrame = nFrame-2; iFrame > -1; --iFrame) {
        path[iFrame] = psi[(iFrame+1) * nState + path[iFrame+1]];
    }

    free(moved);
    free(psi);
}
