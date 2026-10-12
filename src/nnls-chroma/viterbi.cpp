
#include "viterbi.h"
#include <algorithm>
#include <iostream>

// True when every state stays put with one probability and moves to each other
// state with another, the same for all of them (Chordino's chord transitions).
static bool uniformTransitions(const std::vector<vector<double> > &trans, double *stay, double *move) {
    int nState = trans.size();
    if (nState < 2) return false;
    *stay = trans[0][0];
    *move = trans[0][1];
    for (int iState = 0; iState < nState; ++iState) {
        for (int jState = 0; jState < nState; ++jState) {
            if (trans[iState][jState] != (iState == jState ? *stay : *move)) return false;
        }
    }
    return true;
}

std::vector<int> ViterbiPath(const std::vector<double> &init, const std::vector<vector<double> > &trans, const std::vector<vector<double> > &obs, double *delta, vector<double> *scale) {
    
    int nState = init.size();
    int nFrame = obs.size();
    
    // check for consistency
    if ((int)trans[0].size() != nState || (int)trans.size() != nState || (int)obs[0].size() != nState) {
        cerr << "ERROR: matrix sizes inconsistent." << endl;
    }

    vector<vector<int> > psi; //  "matrix" of remembered indices of the best transitions
    vector<int> path = vector<int>(nFrame, nState-1); // the final output path (current assignment arbitrary, makes sense only for Chordino, where nChord-1 is the "no chord" label)
    
    double deltasum = 0;
    
    /* initialise first frame */
    for (int iState = 0; iState < nState; ++iState) {
        delta[iState] = init[iState] * obs[0][iState];
//	cerr << "init[" << iState << "] = " << init[iState] << ", obs[0][" << iState << "] = " << obs[0][iState] << endl;
        deltasum += delta[iState];
    }
    for (int iState = 0; iState < nState; ++iState) delta[iState] /= deltasum; // normalise (scale)
    scale->push_back(1.0/deltasum);
    psi.push_back(vector<int>(nState,0));

//    cerr << "nState = " << nState << ", deltasum = " << deltasum << endl;
    
    /* With uniform transitions the best previous state of j is either j itself or
       the best of all the others, the same for every j, so each frame takes
       O(nState) instead of scanning every pair. Both paths compute the same products
       and keep the lowest index on a tie, so they decode the same path. */
    double stay = 0, move = 0;
    bool uniform = uniformTransitions(trans, &stay, &move);
    vector<double> moved(uniform ? nState : 0);

    /* rest of the forward step */
    for (int iFrame = 1; iFrame < nFrame; ++iFrame) {
        deltasum = 0;
        psi.push_back(vector<int>(nState,0));
        const double *previous = delta + (iFrame-1) * nState;
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
            if (obs[iFrame][jState] > 0) {
                if (uniform) {
                    int other = jState == best ? runnerUp : best;
                    for (int iState : {std::min(jState, other), std::max(jState, other)}) {
                        double currentValue = iState == jState ? previous[iState] * stay : moved[iState];
                        if (currentValue > bestValue) {
                            bestValue = currentValue;
                            bestState = iState;
                        }
                    }
                } else {
                    for (int iState = 0; iState < nState; ++iState) {
                        double currentValue = previous[iState] * trans[iState][jState];
                        if (currentValue > bestValue) {
                            bestValue = currentValue;
                            bestState = iState;
                        }
                    }
                }
            }

            delta[iFrame * nState + jState] = bestValue * obs[iFrame][jState];
            deltasum += delta[iFrame * nState + jState];
            psi[iFrame][jState] = bestState;
        }
        if (deltasum > 0) {
            for (int iState = 0; iState < nState; ++iState) {            
                delta[iFrame * nState + iState] /= deltasum; // normalise (scale)
            }
            scale->push_back(1.0/deltasum);
        } else {
            for (int iState = 0; iState < nState; ++iState) {            
                delta[iFrame * nState + iState] = 1.0/nState;
            }
            scale->push_back(1.0);
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
        path[iFrame] = psi[iFrame+1][path[iFrame+1]];
    }    
    
    return path;
}
