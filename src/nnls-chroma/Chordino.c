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

// The Chordino Vamp plugin, with what was its base class NNLSBase, behind the
// functions of src/chordino.h. A host could set the plugin's parameters; here
// they are the constants below, and the branches only other values reached are
// gone: no local tuning, no spectral roll-on, NNLS always, no chroma
// normalisation.
//
// Upstream wrote it in C++; this is C, with the float and double maths of each
// line the one the C++ overloads picked (atan2f, floorf, sqrtf and powf for
// floats), so that every value comes out as it did.

#include "../chordino.h"

#include "ChordinoInspect.h"
#include "chromamethods.h"
#include "viterbi.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#ifdef CHORDINO_INSPECT
#include <stdio.h>
#endif

// Spectral whitening, the spread of the note templates' harmonics, the weight
// of the no-chord template and chord names in Harte syntax, as the plugin's
// parameters defaulted
static const float whitening = 1.0;
static const float sParam = 0.7;
static const float boostN = 0.1;
static const bool harteSyntax = false;
// Where each stage of chordino_decode ends on the 0-1 scale progress reports,
// in proportion to the time it takes (measured on a 5 minute song) so the
// progress moves at a steady pace: tuning, NNLS chroma (nearly all of it), chord
// salience, then the Viterbi decode and the output features.
static const double progressTuning = 0.02;
static const double progressChroma = 0.94;
static const double progressSalience = 0.98;
// Relative weight of the bass half of the match against the treble half. The two
// halves are normalized independently (see below), so this is the only thing setting
// how much say the bass note has; it is a plain ratio between two comparable
// quantities rather than a gain applied to one of them.
static const double bassWeight = 0.6;
// Leaky integrator applied to chroma before chord matching (forwards and backwards,
// see chordino_decode), so a chord needs to be
// sustained for roughly this long to dominate the signal used for scoring -- a
// single noisy/transient frame can't flip the match. alpha tuned for an effective
// window of ~300ms at the analysis frame rate (step size 2048 samples,
// ~46ms/frame at 44.1kHz): alpha = 1 - exp(-frameMs/windowMs).
static const double chromaEmaAlpha = 0.15;
// The bass bins get a much faster integrator. Which bass note is sounding is the
// only thing separating a chord from its inversions (F and F/A share all three
// treble notes and differ only by which bass bin is 1.0 instead of 0.25), so it is a
// small difference riding on a large common part -- exactly what slow smoothing
// erases, on top of the previous chord's bass bleeding into the current one.
static const double chromaEmaAlphaBass = 0.5;
// Confidence gate. The salience below is normalized to sum to 1, which makes the
// match a forced choice: something always wins, however badly it fits. These two
// ratios restore a real "none of the above" by comparing the winner's raw score
// against the no-chord hypothesis and against the best candidate on a *different*
// root. Speech and singing slide through roots (every syllable implies a new one),
// so they score high in absolute terms but win by a tiny margin over rival roots --
// which is exactly what this catches. Variants sharing the winner's root (C vs C7
// vs Csus4) are refinements rather than ambiguity, so they don't count against it.
// Both are ratios, so they don't need recalibrating when input gain changes.
static const double minChordVsNoChord = 1.05;
static const double minRootMargin = 1.05;
// How hard a frame that fails the gate is pushed towards N.
static const double noConfidenceDamping = 0.01;
// How peaked the per-frame chord distribution handed to the Viterbi decoder is:
// salience = salienceBase^score. Upstream used 1.3, which spread each frame over
// the ~400 templates so evenly that the right chord led its neighbours by about
// 0.25 nat per frame, while entering and leaving a chord costs the decoder about
// 20 nat. A chord had to last ~2 s to pay for that, so changes under a couple of
// seconds were smoothed away even where the chroma clearly moved. 1.3^4: the
// gain levels off there, sharper only splits chords into more segments.
static const double salienceBase = 2.8561;

enum { hamwinlength = nBPS * 6 + 1 };

struct chordino {
    float inputSampleRate;
    size_t blockSize;
    int frameCount;             // frames since the start or the last reset, for the tuning
    // The frames kept (see chordino_forget_before): their times and their
    // log-frequency spectra, nNote bins a third of a semitone apart each
    int nFrame;
    int frameCapacity;
    realtime_t *frameTimes;
    float *logSpectrum;
    // Raw stacked (bass, treble) chroma, 24 bins, of the first nRawChroma
    // frames, kept between chordino_decode calls so a caller decoding again and
    // again pays the NNLS of each frame once; those frames keep the tuning
    // estimated when they were first decoded. Chord matching runs on a smoothed
    // copy.
    int nRawChroma;
    float *rawChromas;
    float meanTunings[nBPS];
    float sinvalues[nBPS];
    float cosvalues[nBPS];
    // The sparse matrix from the magnitude spectrum to the log-frequency one
    int kernelSize;
    float *kernelValue;
    int *kernelFftIndex;
    int *kernelNoteIndex;
    float *magnitude;           // chordino_process's, blockSize / 2
    float *dict;                // the note dictionary, nNote bins for each of 84 semitones
    float hw[hamwinlength];
    chord_dictionary_t *dictionary;
    // What the last chordino_decode returned
    int chordCapacity;
    chordino_chord_t *chords;
    int noteCapacity;
    chordino_note_t *notes;
    int outputCapacity;
    realtime_t *times;
    float *harmonicChange;
    float *logLikelihood;
    chordino_result_t result;
};

// Makes room for count elements of size bytes at *array, which has room for *capacity
static void reserve(void **array, int *capacity, int count, size_t size) {
    if (count <= *capacity) return;
    int grown = *capacity > 0 ? *capacity : 64;
    while (grown < count) grown *= 2;
    *array = realloc(*array, grown * size);
    *capacity = grown;
}

chordino_t *chordino_create(float sampleRate) {
    chordino_t *c = calloc(1, sizeof(chordino_t));
    c->inputSampleRate = sampleRate;
    // make the *note* dictionary matrix
    c->dict = calloc(nNote * 84, sizeof(float));

	dictionary_matrix(c->dict, sParam);

	// make things for tuning estimation
	for (int iBPS = 0; iBPS < nBPS; ++iBPS) {
        c->sinvalues[iBPS] = sin(2*M_PI*(iBPS*1.0/nBPS));
        c->cosvalues[iBPS] = cos(2*M_PI*(iBPS*1.0/nBPS));
    }


	// make hamming window of length 1/2 octave
    float hamwinsum = 0;
    for (int i = 0; i < hamwinlength; ++i) {
        c->hw[i] = 0.54 - 0.46 * cos((2*M_PI*i)/(hamwinlength-1));
        hamwinsum += 0.54 - 0.46 * cos((2*M_PI*i)/(hamwinlength-1));
    }
    for (int i = 0; i < hamwinlength; ++i) c->hw[i] = c->hw[i] / hamwinsum;


    // initialise the tuning
    for (int iBPS = 0; iBPS < nBPS; ++iBPS) {
        c->meanTunings[iBPS] = 0;
    }

    size_t blockSize = CHORDINO_BLOCK_SIZE;
    c->blockSize = blockSize;
    c->frameCount = 0;
    c->magnitude = malloc(c->blockSize/2 * sizeof(float));
    int tempn = nNote * c->blockSize/2;
    float *tempkernel = malloc(tempn * sizeof(float));

    log_freq_matrix(c->inputSampleRate, c->blockSize, tempkernel);
    // I don't know if this is wise: manually making a sparse matrix
    int nonzero = 0;
    for (int i = 0; i < tempn; ++i) {
        if (tempkernel[i] > 0) nonzero++;
    }
    c->kernelValue = malloc(nonzero * sizeof(float));
    c->kernelFftIndex = malloc(nonzero * sizeof(int));
    c->kernelNoteIndex = malloc(nonzero * sizeof(int));
    for (int iNote = 0; iNote < nNote; ++iNote) {
        for (int iFFT = 0; iFFT < (int)(blockSize/2); ++iFFT) {
            if (tempkernel[iFFT + blockSize/2 * iNote] > 0) {
                c->kernelValue[c->kernelSize] = tempkernel[iFFT + blockSize/2 * iNote];
                c->kernelFftIndex[c->kernelSize] = iFFT;
                c->kernelNoteIndex[c->kernelSize] = iNote;
                c->kernelSize++;
            }
        }
    }
    free(tempkernel);

    c->dictionary = chord_dictionary_create(boostN, harteSyntax);
    return c;
}

void chordino_destroy(chordino_t *c) {
    chord_dictionary_destroy(c->dictionary);
    free(c->dict);
    free(c->magnitude);
    free(c->kernelValue);
    free(c->kernelFftIndex);
    free(c->kernelNoteIndex);
    free(c->frameTimes);
    free(c->logSpectrum);
    free(c->rawChromas);
    free(c->chords);
    free(c->notes);
    free(c->times);
    free(c->harmonicChange);
    free(c->logLikelihood);
    free(c);
}

void chordino_reset(chordino_t *c) {
    // Clear buffers, reset stored values, etc
    c->frameCount = 0;
    c->nFrame = 0;
    for (int iBPS = 0; iBPS < nBPS; ++iBPS) {
        c->meanTunings[iBPS] = 0;
    }
    c->nRawChroma = 0;
}

void chordino_forget_before(chordino_t *c, realtime_t start) {
    int drop = 0;
    while (drop < c->nFrame && realtime_compare(c->frameTimes[drop], start) < 0) drop++;
    if (drop == 0) return;
    c->nFrame -= drop;
    memmove(c->frameTimes, c->frameTimes + drop, c->nFrame * sizeof(realtime_t));
    memmove(c->logSpectrum, c->logSpectrum + drop * nNote, c->nFrame * nNote * sizeof(float));
    int dropChroma = drop < c->nRawChroma ? drop : c->nRawChroma;
    c->nRawChroma -= dropChroma;
    memmove(c->rawChromas, c->rawChromas + dropChroma * 24, c->nRawChroma * 24 * sizeof(float));
}

void chordino_process(chordino_t *c, const float *fbuf, realtime_t timestamp) {
    c->frameCount++;
    float *magnitude = c->magnitude;

    // make magnitude
    float maxmag = -10000;
    for (int iBin = 0; iBin < (int)(c->blockSize/2); iBin++) {
        magnitude[iBin] = sqrtf(fbuf[2 * iBin] * fbuf[2 * iBin] +
                                fbuf[2 * iBin + 1] * fbuf[2 * iBin + 1]);
        if (magnitude[iBin]>c->blockSize*1.0) magnitude[iBin] = c->blockSize; // a valid audio signal (between -1 and 1) should not be limited here.
        if (maxmag < magnitude[iBin]) maxmag = magnitude[iBin];
    }

    if (maxmag < 2) {
        for (int iBin = 0; iBin < (int)(c->blockSize/2); iBin++) {
            magnitude[iBin] = 0;
        }
    }

    // note magnitude mapping using pre-calculated matrix
    float nm[nNote]; // note magnitude
    for (int iNote = 0; iNote < nNote; iNote++) {
        nm[iNote] = 0; // initialise as 0
    }
    for (int binCount = 0; binCount < c->kernelSize; binCount++) {
        nm[c->kernelNoteIndex[binCount]] += magnitude[c->kernelFftIndex[binCount]] * c->kernelValue[binCount];
    }

    float one_over_N = 1.0/c->frameCount;
    // update means of complex tuning variables
    for (int iBPS = 0; iBPS < nBPS; ++iBPS) c->meanTunings[iBPS] *= (float)(c->frameCount-1)*one_over_N;

    for (int iTone = 0; iTone < round(nNote*0.62/nBPS)*nBPS+1; iTone = iTone + nBPS) {
        for (int iBPS = 0; iBPS < nBPS; ++iBPS) c->meanTunings[iBPS] += nm[iTone + iBPS]*one_over_N;
    }

    // remember note magnitude
    if (c->nFrame == c->frameCapacity) {
        int capacity = c->frameCapacity;
        reserve((void **) &c->frameTimes, &capacity, c->nFrame + 1, sizeof(realtime_t));
        c->logSpectrum = realloc(c->logSpectrum, capacity * nNote * sizeof(float));
        c->rawChromas = realloc(c->rawChromas, capacity * 24 * sizeof(float));
        c->frameCapacity = capacity;
    }
    c->frameTimes[c->nFrame] = timestamp;
    memcpy(c->logSpectrum + c->nFrame * nNote, nm, nNote * sizeof(float));
    c->nFrame++;
}

// Reports frame of nFrame through a stage spanning from-to on the 0-1 scale
static void report(void (*progress)(double done), double from, double to, int frame, int nFrame) {
    if (progress) progress(from + (to - from) * frame / nFrame);
}

const chordino_result_t *chordino_decode(chordino_t *c, void (*progress)(double done)) {
    memset(&c->result, 0, sizeof(c->result));
    if (c->nFrame == 0) return &c->result;
    int nChord = c->dictionary->size;
    const float *chorddict = c->dictionary->templates;
    int nFrame = c->nFrame;

    // L2 norm per candidate template. Raw dot-product scoring favours templates with
    // more populated bins (7th/9th/11th/13th chords, add-tone chords, bass/slash
    // variants) purely because they have more chances to pick up incidental
    // harmonic/noise energy across bins that aren't really part of the chord being
    // played (e.g. F13 shares all 3 notes of Dm plus 3 extra, so it can outscore Dm
    // whenever those 3 extra bins have any stray energy). Dividing by each template's
    // own norm removes that size bias.
    //
    // The bass half and the treble half get their *own* norms, and are matched
    // separately. Normalizing the stacked 24-bin template as one vector destroys
    // inversion detection: C, C/E and C/G carry an identical treble template and
    // differ only in the bass, so the treble part is common-mode and has to cancel
    // out of the comparison -- but it only cancels if every candidate divides it by
    // the same number. It doesn't. A slash template carries a 1.0 bass spike that a
    // plain triad's template lacks, so it has the larger combined norm, so the shared
    // treble energy (which dominates: the bass window dies above Bb3 while the treble
    // window spans the whole range) is discounted more for the slash reading than for
    // the plain one. That difference is far bigger than the bass difference it is
    // supposed to be decided by, and it always points the same way, so every
    // inversion was reported as its root-position chord. Splitting the norms makes
    // the treble term genuinely identical across the three, leaving the bass as the
    // sole discriminator -- which is the whole point of having bass bins.
    //
    // Note-count penalty, applied in both directions:
    //  - beyond a triad: normalizing by template norm isn't enough for chords like
    //    9/11/13, because their "extra" notes often ARE the real harmonic overtones
    //    of notes already in a simpler chord (e.g. Bb's own 5th and flat-7th partials
    //    are F and Ab -- exactly the two "extra" notes Bb13 adds on top of a Gm
    //    triad's G/Bb/D). That energy is real, not noise, so norm alone can't tell the
    //    extended chord apart from harmonic bleed off the simpler one.
    //  - below a triad: the norm cuts the other way for the two-note power-chord ("5")
    //    templates. Dropping the third shrinks the norm faster than it shrinks the dot
    //    product, so C5 outscored a fully-voiced C major. Penalise thinner-than-triad
    //    templates by the same factor so a "5" only wins when the third really is
    //    absent.
    double *bassNorm = malloc(nChord * sizeof(double));
    double *trebleNorm = malloc(nChord * sizeof(double));
    double *noteCountPenalty = malloc(nChord * sizeof(double));
    for (int iChord = 0; iChord < nChord; iChord++) {
        double bassSumSq = 0;
        double trebleSumSq = 0;
        int noteCount = 0;
        for (int iBin = 0; iBin < 12; iBin++) {
            double v = chorddict[24 * iChord + iBin];
            bassSumSq += v * v;
        }
        for (int iBin = 12; iBin < 24; iBin++) {
            double v = chorddict[24 * iChord + iBin];
            trebleSumSq += v * v;
            if (v > 0) noteCount++;
        }
        bassNorm[iChord] = bassSumSq > 0 ? sqrt(bassSumSq) : 1.0;
        trebleNorm[iChord] = trebleSumSq > 0 ? sqrt(trebleSumSq) : 1.0;
        noteCountPenalty[iChord] = 1.0;
        if (iChord != nChord - 1 && noteCount > 3) {
            noteCountPenalty[iChord] = pow(0.93, noteCount - 3);
        }
        if (iChord != nChord - 1 && noteCount < 3) {
            noteCountPenalty[iChord] = pow(0.93, 3 - noteCount);
        }
    }
    //
    /**  Calculate Tuning
         calculate tuning from (using the angle of the complex number defined by the
         cumulative mean real and imag values)
    **/
    float meanTuningImag = 0;
    float meanTuningReal = 0;
    for (int iBPS = 0; iBPS < nBPS; ++iBPS) {
        meanTuningReal += c->meanTunings[iBPS] * c->cosvalues[iBPS];
        meanTuningImag += c->meanTunings[iBPS] * c->sinvalues[iBPS];
    }
    float normalisedtuning = atan2f(meanTuningImag, meanTuningReal)/(2*M_PI);
    int intShift = floorf(normalisedtuning * 3);
    float floatShift = normalisedtuning * 3 - intShift; // floatShift is a really bad name for this

    /** Tune Log-Frequency Spectrogram
        calculate a tuned log-frequency spectrogram (currentTunedSpec): use the tuning estimated above (kinda f0) to
        perform linear interpolation on the existing log-frequency spectrogram (kinda currentLogSpectrum).
    **/
    // Frames an earlier call already worked the chroma out for (see rawChromas):
    // only the ones after them go through tuning and NNLS below.
    int nCached = c->nRawChroma;
    int nFresh = nFrame - nCached;
    int count = nCached;
    float *tunedSpec = malloc((nFresh > 0 ? nFresh : 1) * nNote * sizeof(float));

    reserve((void **) &c->times, &c->outputCapacity, nFrame, sizeof(realtime_t));
    c->harmonicChange = realloc(c->harmonicChange, c->outputCapacity * sizeof(float));
    c->logLikelihood = realloc(c->logLikelihood, c->outputCapacity * sizeof(float));
    realtime_t *timestamps = c->times;
    memcpy(timestamps, c->frameTimes, nFrame * sizeof(realtime_t));

    report(progress, 0, progressTuning, count, nFrame);
    for (int iFresh = 0; iFresh < nFresh; ++iFresh) {
        const float *currentLogSpectrum = c->logSpectrum + (nCached + iFresh) * nNote;
        float *currentTunedSpec = tunedSpec + iFresh * nNote; // tuned log-frequency spectrum
        currentTunedSpec[0] = 0.0; currentTunedSpec[1] = 0.0; // set lower edge to zero

        for (int k = 2; k < nNote - 3; ++k) { // interpolate all inner bins
            float tempValue = currentLogSpectrum[k + intShift] * (1-floatShift) + currentLogSpectrum[k+intShift+1] * floatShift;
            currentTunedSpec[k] = tempValue;
        }

        currentTunedSpec[nNote - 3] = 0.0; currentTunedSpec[nNote - 2] = 0.0; currentTunedSpec[nNote - 1] = 0.0; // upper edge
        float runningmean[nNote];
        special_convolution(currentTunedSpec, c->hw, hamwinlength, runningmean);
        float variance[nNote];
        for (int i = 0; i < nNote; i++) { // first step: squared values into vector (variance)
            variance[i] = (currentTunedSpec[i] - runningmean[i]) * (currentTunedSpec[i] - runningmean[i]);
        }
        float runningstd[nNote];
        special_convolution(variance, c->hw, hamwinlength, runningstd); // second step convolve
        for (int i = 0; i < nNote; i++) {
            runningstd[i] = sqrtf(runningstd[i]); // square root to finally have running std
            if (runningstd[i] > 0) {
                currentTunedSpec[i] = (currentTunedSpec[i] - runningmean[i]) > 0 ?
                    (currentTunedSpec[i] - runningmean[i]) / powf(runningstd[i],whitening) : 0;
            }
#ifdef CHORDINO_INSPECT
            if (currentTunedSpec[i] < 0) {
                fprintf(stderr, "ERROR: negative value in logfreq spectrum\n");
            }
#endif
        }
        count++;
        report(progress, 0, progressTuning, count, nFrame);
    }

    /** Semitone spectrum and chromagrams
        Semitone-spaced log-frequency spectrum derived from the tuned log-freq spectrum above. the spectrum
        is inferred using a non-negative least squares algorithm.
        Three different kinds of chromagram are calculated, "treble", "bass", and "both" (which means
        bass and treble stacked onto each other).
    **/
    double *chordogram = malloc(nFrame * nChord * sizeof(double)); // nFrame rows of nChord saliences
    float *chordchange = c->harmonicChange;
    for (int iFrame = 0; iFrame < nFrame; iFrame++) chordchange[iFrame] = 0;
    count = nCached;

#ifdef CHORDINO_INSPECT
    bool clipwarned = false;
    float *inspectSemitones = calloc((nFresh > 0 ? nFresh : 1) * 84, sizeof(float));
    chordinoInspectBegin(nChord, c->dictionary->names);
#endif

    for (int iFresh = 0; iFresh < nFresh; ++iFresh) {
        const float *currentTunedSpec = tunedSpec + iFresh * nNote; // logfreq spectrum

        float b[nNote];

        bool some_b_greater_zero = false;
        for (int i = 0; i < nNote; i++) {
            b[i] = currentTunedSpec[i];
            if (b[i] > 0) {
                some_b_greater_zero = true;
            }
        }

        // here's where the non-negative least squares algorithm calculates the note activation x

        // the bass chroma (0-11) stacked under the treble one (12-23)
        float *chroma = c->rawChromas + c->nRawChroma * 24;
        for (int i = 0; i < 24; i++) chroma[i] = 0;

        if (some_b_greater_zero) {
            float x[84+1000];
            for (int i = 1; i < 1084; ++i) x[i] = 1.0;
            int signifIndex[84];
            int nSignif = 0;
            int index=0;
            for (int iNote = nBPS/2 + 2; iNote < nNote - nBPS/2; iNote += nBPS) {
                float currval = 0;
                for (int iBPS = -nBPS/2; iBPS < nBPS/2+1; ++iBPS) {
                    currval += b[iNote + iBPS];
                }
                if (currval > 0) signifIndex[nSignif++] = index;
                index++;
            }
            float rnorm;
            float w[84+1000];
            float zz[84+1000];
            int indx[84+1000];
            int mode;
            float *curr_dict = malloc((nSignif > 0 ? nSignif : 1) * nNote * sizeof(float));
            for (int iNote = 0; iNote < nSignif; ++iNote) {
                for (int iBin = 0; iBin < nNote; iBin++) {
                    curr_dict[iNote * nNote + iBin] = 1.0 * c->dict[signifIndex[iNote] * nNote + iBin];
                }
            }
            nnls(curr_dict, nNote, nNote, nSignif, b, x, &rnorm, w, zz, indx, &mode);
            free(curr_dict);
            for (int iNote = 0; iNote < nSignif; ++iNote) {
                chroma[12 + signifIndex[iNote] % 12] += x[iNote] * treblewindow[signifIndex[iNote]];
                chroma[signifIndex[iNote] % 12] += x[iNote] * basswindow[signifIndex[iNote]];
#ifdef CHORDINO_INSPECT
                inspectSemitones[iFresh * 84 + signifIndex[iNote]] = x[iNote];
#endif
            }
        }
        c->nRawChroma++;

        count++;
        report(progress, progressTuning, progressChroma, count, nFrame);
    }

    // Smoothed copy of the chroma used only for chord matching (see chromaEmaAlpha
    // above); the raw per-frame chroma is left untouched, as it should reflect the
    // actual instantaneous signal.
    //
    // The integrator runs forwards and backwards and the two are averaged. A
    // forward-only one lags by (1 - alpha) / alpha frames, about 0.26 s for the
    // treble, which made every chord change come out that much late; the two
    // directions lag by the same amount the opposite way, so their average is
    // centred on the frame. Everything is buffered until here anyway, so the
    // future frames are available.
    double *smoothedChromas = malloc(nFrame * 24 * sizeof(double));
    double runningChroma[24] = {0};
    for (int iFrame = 0; iFrame < nFrame; iFrame++) {
        // Bins 0-11 are the bass chroma, 12-23 the treble one (stacked above).
        for (int i = 0; i < 24; i++) {
            double alpha = (i < 12) ? chromaEmaAlphaBass : chromaEmaAlpha;
            runningChroma[i] = iFrame == 0 ? c->rawChromas[iFrame * 24 + i]
                : alpha * c->rawChromas[iFrame * 24 + i] + (1 - alpha) * runningChroma[i];
            smoothedChromas[iFrame * 24 + i] = runningChroma[i] / 2;
        }
    }
    for (int iFrame = nFrame - 1; iFrame >= 0; iFrame--) {
        for (int i = 0; i < 24; i++) {
            double alpha = (i < 12) ? chromaEmaAlphaBass : chromaEmaAlpha;
            runningChroma[i] = iFrame == nFrame - 1 ? c->rawChromas[iFrame * 24 + i]
                : alpha * c->rawChromas[iFrame * 24 + i] + (1 - alpha) * runningChroma[i];
            smoothedChromas[iFrame * 24 + i] += runningChroma[i] / 2;
        }
    }

    // Scores kept as scored, before the exponential and the normalisation below
    // discard how well each candidate actually fit (see the gate further down).
    double *rawChordValue = malloc(nChord * sizeof(double));
    for (int iFrame = 0; iFrame < nFrame; iFrame++) {
        const double *smoothedChroma = smoothedChromas + iFrame * 24;

        // local chord estimation
        double *currentChordSalience = chordogram + iFrame * nChord;
        double tempchordvalue = 0;
        double sumchordvalue = 0;

        for (int iChord = 0; iChord < nChord; iChord++) {
            double bassMatch = 0;
            double trebleMatch = 0;
            for (int iBin = 0; iBin < 12; iBin++) {
                bassMatch += chorddict[24 * iChord + iBin] * smoothedChroma[iBin];
            }
            for (int iBin = 12; iBin < 24; iBin++) {
                trebleMatch += chorddict[24 * iChord + iBin] * smoothedChroma[iBin];
            }
            tempchordvalue = trebleMatch / trebleNorm[iChord]
                + bassWeight * bassMatch / bassNorm[iChord];
            tempchordvalue *= noteCountPenalty[iChord];
            if (iChord == nChord-1) tempchordvalue *= .7;
            if (tempchordvalue < 0) tempchordvalue = 0.0;
            if (tempchordvalue > 200.0) {
#ifdef CHORDINO_INSPECT
                if (!clipwarned) {
                    fprintf(stderr, "WARNING: interim chroma contains extreme chord value %g, clipping this and any others that appear\n", tempchordvalue);
                    clipwarned = true;
                }
#endif
                tempchordvalue = 200.0;
            }
            rawChordValue[iChord] = tempchordvalue;
            tempchordvalue = pow(salienceBase, tempchordvalue);
            sumchordvalue += tempchordvalue;
            currentChordSalience[iChord] = tempchordvalue;
        }

        // Chords are laid out as [type][root] with 12 roots per type (see
        // chord_dictionary_create), so a candidate's root is simply its index mod 12, and the
        // last entry is the no-chord hypothesis N.
        int bestChord = 0;
        for (int iChord = 1; iChord < nChord - 1; iChord++) {
            if (rawChordValue[iChord] > rawChordValue[bestChord]) bestChord = iChord;
        }
        // Neighbouring chords on another root are not rivals either: G and Em, C and Am,
        // F and Dm share two of their three notes, so a clean, sustained G scores within
        // a few percent of Em by construction. Counting them failed the margin on whole
        // stretches of plain diatonic music (about 15% of frames on a worship song),
        // and since a rejected frame outweighs many accepted ones in the Viterbi
        // decode, those stretches came out as N. Speech and singing still fail the
        // margin, against chords sharing at most one note with the winner.
        double bestRivalRoot = 0;
        for (int iChord = 0; iChord < nChord - 1; iChord++) {
            if (iChord % 12 == bestChord % 12) continue;
            int sharedNotes = 0;
            for (int iBin = 12; iBin < 24; iBin++) {
                if (chorddict[24 * iChord + iBin] > 0 && chorddict[24 * bestChord + iBin] > 0) sharedNotes++;
            }
            if (sharedNotes >= 2) continue;
            if (rawChordValue[iChord] > bestRivalRoot) bestRivalRoot = rawChordValue[iChord];
        }
        bool confident =
            rawChordValue[bestChord] > minChordVsNoChord * rawChordValue[nChord-1] &&
            rawChordValue[bestChord] > minRootMargin * bestRivalRoot;

        if (sumchordvalue > 0) {
            for (int iChord = 0; iChord < nChord; iChord++) {
                currentChordSalience[iChord] /= sumchordvalue;
            }
        } else {
            currentChordSalience[nChord-1] = 1.0;
        }

        if (!confident) {
            // Nothing fits well enough to call, so shift the weight onto N rather than
            // reporting the least-bad guess. Damp the others instead of zeroing them:
            // viterbi_path multiplies these observations directly and the harmonic
            // change feature divides by them, so a hard zero would poison both. Keeping
            // it a distribution also lets the Viterbi override a single gated frame
            // when the surrounding context is strong.
            double gatedSum = currentChordSalience[nChord-1];
            for (int iChord = 0; iChord < nChord - 1; iChord++) {
                currentChordSalience[iChord] *= noConfidenceDamping;
                gatedSum += currentChordSalience[iChord];
            }
            if (gatedSum > 0) {
                for (int iChord = 0; iChord < nChord; iChord++) {
                    currentChordSalience[iChord] /= gatedSum;
                }
            }
        }
#ifdef CHORDINO_INSPECT
        int gate = 0;
        if (!(rawChordValue[bestChord] > minChordVsNoChord * rawChordValue[nChord-1])) gate = 1;
        else if (!confident) gate = 2;
        // Only frames this call ran NNLS on have their chroma and semitones here
        bool fresh = iFrame >= nCached;
        chordinoInspectFrame(realtime_to_seconds(timestamps[iFrame]),
                             fresh ? c->rawChromas + iFrame * 24 : NULL,
                             smoothedChroma, currentChordSalience,
                             fresh ? inspectSemitones + (iFrame - nCached) * 84 : NULL, gate);
#endif
        report(progress, progressChroma, progressSalience, iFrame + 1, nFrame);
    }

    // The notes of the chord being decoded, until the next one gives them their duration
    chordino_note_t oldnotes[25];
    int nOldnotes = 0;

    double selftransprob = 0.99;

    // Uniform prior: forcing init[nChord-1]=1 (N) here makes every decode assume the
    // previous state was "no chord", which is only true at the very start of a session.
    // With periodic resets (real-time streaming), that false certainty
    // reintroduces a spurious N bias at every reset boundary, since selftransprob makes
    // it costly for the Viterbi path to leave N even when real chord evidence is strong.
    double *init = malloc(nChord * sizeof(double));
    for (int iChord = 0; iChord < nChord; iChord++) init[iChord] = 1.0/nChord;

    double *delta = malloc(sizeof(double)*nFrame*nChord);

    double *trans = malloc(nChord * nChord * sizeof(double));
    for (int i = 0; i < nChord * nChord; i++) trans[i] = (1-selftransprob)/(nChord-1);
    for (int iChord = 0; iChord < nChord; iChord++) {
        trans[iChord * nChord + iChord] = selftransprob;
    }
    double *scale = malloc(nFrame * sizeof(double));
    int *chordpath = malloc(nFrame * sizeof(int));
    viterbi_path(nChord, nFrame, init, trans, chordogram, delta, scale, chordpath);

    int nChords = 0;
    int nNotes = 0;
    reserve((void **) &c->chords, &c->chordCapacity, 2, sizeof(chordino_chord_t));
    c->chords[nChords++] = (chordino_chord_t) {c->dictionary->names[chordpath[0]], timestamps[0]};
    // With the uniform prior the path no longer always starts on N, so track the first
    // chord as already emitted; otherwise it is reported again at the second frame.
    int oldchord = chordpath[0];
    const chord_dictionary_t *d = c->dictionary;
    for (int iNote = d->noteStart[chordpath[0]]; iNote < d->noteStart[chordpath[0] + 1]; ++iNote) { // prepare notes of current chord
        oldnotes[nOldnotes++] = (chordino_note_t) {d->notes[iNote], timestamps[0], realtime_negate(timestamps[0])}; // the duration will be corrected at the next chord
    }

    chordchange[0] = 0;
    for (int iFrame = 1; iFrame < nFrame; ++iFrame) {
        if (chordpath[iFrame] != oldchord ) {
            // chord
            reserve((void **) &c->chords, &c->chordCapacity, nChords + 2, sizeof(chordino_chord_t));
            c->chords[nChords++] = (chordino_chord_t) {c->dictionary->names[chordpath[iFrame]], timestamps[iFrame]};
            oldchord = chordpath[iFrame];
            // chord notes
            reserve((void **) &c->notes, &c->noteCapacity, nNotes + nOldnotes, sizeof(chordino_note_t));
            for (int iNote = 0; iNote < nOldnotes; ++iNote) { // finish duration of old chord
                oldnotes[iNote].duration = realtime_add(oldnotes[iNote].duration, timestamps[iFrame]);
                c->notes[nNotes++] = oldnotes[iNote];
            }
            nOldnotes = 0;
            for (int iNote = d->noteStart[chordpath[iFrame]]; iNote < d->noteStart[chordpath[iFrame] + 1]; ++iNote) { // prepare notes of current chord
                oldnotes[nOldnotes++] = (chordino_note_t) {d->notes[iNote], timestamps[iFrame], realtime_negate(timestamps[iFrame])};
            }
        }
        /* calculating simple chord change prob */
        for (int iChord = 0; iChord < nChord; iChord++) {
            double num = delta[(iFrame-1) * nChord + iChord];
            double denom = delta[iFrame * nChord + iChord];
            double eps = 1e-7;
            if (denom < eps) denom = eps;
            chordchange[iFrame-1] += num * log(num / denom + eps);
        }
    }

    for (int iFrame = 0; iFrame < nFrame; ++iFrame) {
        c->logLikelihood[iFrame] = -log(scale[iFrame]);
    }

    c->chords[nChords++] = (chordino_chord_t) {"N", timestamps[nFrame-1]};

    reserve((void **) &c->notes, &c->noteCapacity, nNotes + nOldnotes, sizeof(chordino_note_t));
    for (int iNote = 0; iNote < nOldnotes; ++iNote) { // finish duration of old chord
        oldnotes[iNote].duration = realtime_add(oldnotes[iNote].duration, timestamps[nFrame-1]);
        c->notes[nNotes++] = oldnotes[iNote];
    }

    free(delta);
    free(trans);
    free(init);
    free(scale);
    free(chordpath);
    free(rawChordValue);
    free(smoothedChromas);
    free(chordogram);
    free(tunedSpec);
    free(bassNorm);
    free(trebleNorm);
    free(noteCountPenalty);
#ifdef CHORDINO_INSPECT
    free(inspectSemitones);
#endif

    report(progress, progressSalience, 1, 1, 1);

    c->result.chordCount = nChords;
    c->result.chords = c->chords;
    c->result.noteCount = nNotes;
    c->result.notes = c->notes;
    c->result.frameCount = nFrame;
    c->result.times = c->times;
    c->result.harmonicChange = c->harmonicChange;
    c->result.logLikelihood = c->logLikelihood;
    return &c->result;
}
