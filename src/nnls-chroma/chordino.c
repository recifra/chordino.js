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
#include "../vector.h"

#include "chordino_inspect.h"
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
// Score factor of the inversions (C/E, G/B), on top of the note-count penalty. A
// plain chord has two templates, its root in the bass and no bass note in
// particular, which split its salience between them, while each inversion has
// one; so where the bass says little, as under a phone or tablet microphone that
// barely hears it, or a hum sitting on B, an inversion led each of them and the
// decoder took it. Played in root position, G came out as G/B and C as C/G. An
// inversion now has to be clearly in the bass. On training.mp3 and voicing.mp3,
// 4 to 5 points more chords right to the inversion, about 1 less to the root.
static const double slashPenalty = 0.97;
// Score factor of a chord with the same notes as one on another root (Fsus2 and
// Csus4, Am7 and C6) where that other root is the lowest note and no bass is
// played. Such chords differ only in their bass templates, so with nothing in the
// bass their choice was left to whatever stray energy sat there, and under a
// tablet's microphone F4 G4 C5 read as Csus4 and Bb3 Eb4 F4 as Ebsus2: NNLS
// hears the root a fourth implies under it (C under G4 C5, Eb under Bb3 Eb4), and
// a speaker's distortion adds it too (C5 - G4 = C3, 2 Bb3 - Eb4 = Eb3). The ear
// names the chord from its lowest note. Only chords of the same notes give way,
// so a lowest E says nothing about C against Em, and played over a bass the
// bass templates decide as before. Streamed live, training.mp3 gets 0.5 points
// more chords right, and both it and voicing.mp3 0.3 to 0.4 more roots.
static const double otherRootFactor = 0.95;
// The lowest note of a frame is the lowest whose NNLS activation holds at least
// this share of the strongest one's through lowestNoteFrames frames either side:
// stray notes come and go, and the ones that distortion and NNLS add under a
// chord are weaker than its notes.
static const double lowestNoteShare = 0.7;
static const int lowestNoteFrames = 2;
// A bass is played where a note the bass window weighs at least half holds this
// share of the strongest
static const double playedBassShare = 0.5;
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
// How fast the background (see remove_background) rises after a quieter frame
// set it, in dB a second, while the frames are quiet
static const float backgroundRiseDb = 0.5;
// How much over the background comes off: twice it, so a hum swaying a few dB
// above its quietest goes too
static const float backgroundOver = 2;
// A frame is quiet, and lets the background rise, while its magnitudes add up
// to under this many times the background's
static const double backgroundQuiet = 4;

enum { hamwinlength = nBPS * 6 + 1 };

struct chordino {
    float inputSampleRate;
    size_t blockSize;
    int frameCount;             // frames since the start or the last reset, for the tuning
    // The frames kept (see chordino_forget_before): their times (realtime_t)
    // and their log-frequency spectra, nNote bins a third of a semitone apart
    // each (one element of nNote floats per frame)
    vector_t frameTimes;
    vector_t logSpectrum;
    // Raw stacked (bass, treble) chroma, 24 floats per element, of the first
    // frames, kept between chordino_decode calls so a caller decoding again and
    // again pays the NNLS of each frame once; those frames keep the tuning
    // estimated when they were first decoded. Chord matching runs on a smoothed
    // copy.
    vector_t rawChromas;
    // The 84 semitone activations NNLS found for each of those frames, for the
    // lowest note (see lowest_notes) and chordino_inspect_frame
    vector_t semitones;
    float meanTunings[nBPS];
    float sinvalues[nBPS];
    float cosvalues[nBPS];
    // The sparse matrix from the magnitude spectrum to the log-frequency one
    int kernelSize;
    float *kernelValue;
    int *kernelFftIndex;
    int *kernelNoteIndex;
    float *magnitude;           // chordino_process's, blockSize / 2
    // With removeBackground: the background of each magnitude bin, whether a
    // frame has set it yet, and what it is multiplied by from one frame to the next
    float *background;
    bool backgroundSet;
    float backgroundRise;
    float *dict;                // the note dictionary, nNote bins for each of 84 semitones
    float hw[hamwinlength];
    chord_dictionary_t *dictionary;
    // What the last chordino_decode returned: chordino_chord_t,
    // chordino_note_t, and per frame realtime_t, float and float
    vector_t chords;
    vector_t notes;
    vector_t times;
    vector_t harmonicChange;
    vector_t logLikelihood;
    chordino_result_t result;
};

chordino_t *chordino_create(float sampleRate, bool removeBackground) {
    chordino_t *c = calloc(1, sizeof(chordino_t));
    c->inputSampleRate = sampleRate;
    c->frameTimes = vector_create(sizeof(realtime_t));
    c->logSpectrum = vector_create(nNote * sizeof(float));
    c->rawChromas = vector_create(24 * sizeof(float));
    c->semitones = vector_create(84 * sizeof(float));
    c->chords = vector_create(sizeof(chordino_chord_t));
    c->notes = vector_create(sizeof(chordino_note_t));
    c->times = vector_create(sizeof(realtime_t));
    c->harmonicChange = vector_create(sizeof(float));
    c->logLikelihood = vector_create(sizeof(float));
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
    if (removeBackground) {
        c->background = malloc(c->blockSize/2 * sizeof(float));
        c->backgroundRise = pow(10, backgroundRiseDb * CHORDINO_STEP_SIZE / sampleRate / 20);
    }
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
    free(c->background);
    free(c->kernelValue);
    free(c->kernelFftIndex);
    free(c->kernelNoteIndex);
    vector_destroy(&c->frameTimes);
    vector_destroy(&c->logSpectrum);
    vector_destroy(&c->rawChromas);
    vector_destroy(&c->semitones);
    vector_destroy(&c->chords);
    vector_destroy(&c->notes);
    vector_destroy(&c->times);
    vector_destroy(&c->harmonicChange);
    vector_destroy(&c->logLikelihood);
    free(c);
}

void chordino_reset(chordino_t *c) {
    // Clear buffers, reset stored values, etc
    c->frameCount = 0;
    c->backgroundSet = false;
    vector_clear(&c->frameTimes);
    vector_clear(&c->logSpectrum);
    for (int iBPS = 0; iBPS < nBPS; ++iBPS) {
        c->meanTunings[iBPS] = 0;
    }
    vector_clear(&c->rawChromas);
    vector_clear(&c->semitones);
}

void chordino_forget_before(chordino_t *c, realtime_t start) {
    const realtime_t *frameTimes = c->frameTimes.data;
    int drop = 0;
    while (drop < c->frameTimes.count && realtime_compare(frameTimes[drop], start) < 0) drop++;
    vector_erase(&c->frameTimes, 0, drop);
    vector_erase(&c->logSpectrum, 0, drop);
    int dropChroma = drop < c->rawChromas.count ? drop : c->rawChromas.count;
    vector_erase(&c->rawChromas, 0, dropChroma);
    vector_erase(&c->semitones, 0, dropChroma);
}

// Takes the steady background off the magnitude spectrum, in place. The
// background of a bin follows the quietest it has been lately, the way
// ignoreBackground in tuner-live-worker.mjs follows the RMS: it drops at once to
// a quieter frame, and rises backgroundRiseDb a second after it, but only
// through quiet frames. A hum or a whine stays as loud frame after frame, so it
// is all background and goes, and it goes before the tuning is estimated, which
// it would otherwise pull. Under a microphone near a speaker left on, a 60 Hz
// hum and a tone at 397 Hz, a quarter-tone off G4, were all that most of the
// frames of typing and talking had in tune, and the chords the matcher read in
// them had G. A note rises over the floor and falls back to it once it ends,
// so only the floor comes off it; but in the bass of a tablet microphone the
// floor is close under the notes, and a Bb held through Bbsus2 and Bbsus4 rose
// into the background within 20 s, which left Eb the bass and read Ebsus2. So
// the background holds still while anything plays over it.
static void remove_background(chordino_t *c, float *magnitude) {
    int nBin = (int)(c->blockSize/2);
    double level = 0;
    double floor = 0;
    for (int iBin = 0; iBin < nBin; iBin++) {
        level += magnitude[iBin];
        floor += c->background[iBin];
    }
    float rise = !c->backgroundSet || level < backgroundQuiet * floor ? c->backgroundRise : 1;
    for (int iBin = 0; iBin < nBin; iBin++) {
        float background = c->backgroundSet ? fminf(magnitude[iBin], c->background[iBin] * rise) : magnitude[iBin];
        c->background[iBin] = background;
        magnitude[iBin] = fmaxf(magnitude[iBin] - backgroundOver * background, 0);
    }
    c->backgroundSet = true;
}

// The pitch class (0 = A) of the lowest note of each frame where no bass is
// played, -1 where one is (see otherRootFactor). Each note counts with its
// activation at its least within lowestNoteFrames frames either side.
static void lowest_notes(const float *semitones, int nFrame, int *lowest) {
    for (int iFrame = 0; iFrame < nFrame; iFrame++) {
        float held[84];
        float strongest = 0;
        for (int iNote = 0; iNote < 84; iNote++) {
            float least = INFINITY;
            for (int jFrame = iFrame - lowestNoteFrames; jFrame <= iFrame + lowestNoteFrames; jFrame++) {
                if (jFrame >= 0 && jFrame < nFrame) least = fminf(least, semitones[jFrame * 84 + iNote]);
            }
            held[iNote] = least;
            strongest = fmaxf(strongest, least);
        }
        bool bass = false;
        for (int iNote = 0; iNote < 84; iNote++) {
            if (basswindow[iNote] >= 0.5 && held[iNote] >= playedBassShare * strongest) bass = true;
        }
        lowest[iFrame] = -1;
        for (int iNote = 0; !bass && strongest > 0 && iNote < 84; iNote++) {
            if (held[iNote] >= lowestNoteShare * strongest) {
                lowest[iFrame] = iNote % 12;
                break;
            }
        }
    }
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

    // Quiet frames too, so that the background starts from the room and not from
    // the first note played in it; not digital silence, as a microphone sends
    // before its first samples, from which it could never rise again
    if (c->background && maxmag > 0) {
        remove_background(c, magnitude);
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
    vector_push(&c->frameTimes, &timestamp, 1);
    vector_push(&c->logSpectrum, nm, 1);
}

// Whether a chord template (24 bins, bass first) holds just a major triad on root
// and its minor 7th, whatever its bass
static bool dominant_seventh(const float *chord, int root) {
    int noteCount = 0;
    for (int iBin = 12; iBin < 24; iBin++) {
        if (chord[iBin] > 0) noteCount++;
    }
    static const int intervals[] = {0, 4, 7, 10};
    for (int i = 0; i < 4; i++) {
        if (!(chord[12 + (root + intervals[i]) % 12] > 0)) return false;
    }
    return noteCount == 4;
}

// Reports frame of nFrame through a stage spanning from-to on the 0-1 scale
static void report(void (*progress)(double done), double from, double to, int frame, int nFrame) {
    if (progress) progress(from + (to - from) * frame / nFrame);
}

const chordino_result_t *chordino_decode(chordino_t *c, void (*progress)(double done)) {
    memset(&c->result, 0, sizeof(c->result));
    int nFrame = c->frameTimes.count;
    if (nFrame == 0) return &c->result;
    int nChord = c->dictionary->size;
    const float *chorddict = c->dictionary->templates;

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
    //  - but not the dominant 7th, which is no extension of a triad but a chord of its
    //    own, the V7 into its tonic. Penalised, its seventh had to sound at about 0.6
    //    of the strongest note just to tie with the triad, so the Bb7 held for 2 s
    //    before Eb in a hymn, with Ab at 0.6-0.7, came out as Bb all through, and of
    //    the 38 s of dominant 7ths in training.mp3 under 4 s came out as such. The 7ths
    //    it adds where the reference has the triad do have the 7th in the chroma (B7
    //    with A at 0.7, G7 with F at 0.5-0.9, and the G7/F the reference has as F).
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
        if (iChord != nChord - 1 && noteCount > 3 && !dominant_seventh(chorddict + 24 * iChord, iChord % 12)) {
            noteCountPenalty[iChord] = pow(0.93, noteCount - 3);
        }
        if (iChord != nChord - 1 && noteCount < 3) {
            noteCountPenalty[iChord] = pow(0.93, 3 - noteCount);
        }
        if (strchr(c->dictionary->names[iChord], '/')) {
            noteCountPenalty[iChord] *= slashPenalty;
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
    int nCached = c->rawChromas.count;
    int nFresh = nFrame - nCached;
    int count = nCached;
    float *tunedSpec = malloc((nFresh > 0 ? nFresh : 1) * nNote * sizeof(float));

    realtime_t *timestamps = vector_resize(&c->times, nFrame);
    memcpy(timestamps, c->frameTimes.data, nFrame * sizeof(realtime_t));

    report(progress, 0, progressTuning, count, nFrame);
    for (int iFresh = 0; iFresh < nFresh; ++iFresh) {
        const float *currentLogSpectrum = (const float *) c->logSpectrum.data + (nCached + iFresh) * nNote;
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
    float *chordchange = vector_resize(&c->harmonicChange, nFrame);
    for (int iFrame = 0; iFrame < nFrame; iFrame++) chordchange[iFrame] = 0;
    count = nCached;

#ifdef CHORDINO_INSPECT
    bool clipwarned = false;
    chordino_inspect_begin(nChord, c->dictionary->names);
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

        // the bass chroma (0-11) stacked under the treble one (12-23), zeros to start
        float *chroma = vector_push(&c->rawChromas, NULL, 1);
        float *semitones = vector_push(&c->semitones, NULL, 1);

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
                semitones[signifIndex[iNote]] = x[iNote];
            }
        }

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
    const float *rawChromas = c->rawChromas.data;
    double *smoothedChromas = malloc(nFrame * 24 * sizeof(double));
    double runningChroma[24] = {0};
    for (int iFrame = 0; iFrame < nFrame; iFrame++) {
        // Bins 0-11 are the bass chroma, 12-23 the treble one (stacked above).
        for (int i = 0; i < 24; i++) {
            double alpha = (i < 12) ? chromaEmaAlphaBass : chromaEmaAlpha;
            runningChroma[i] = iFrame == 0 ? rawChromas[iFrame * 24 + i]
                : alpha * rawChromas[iFrame * 24 + i] + (1 - alpha) * runningChroma[i];
            smoothedChromas[iFrame * 24 + i] = runningChroma[i] / 2;
        }
    }
    for (int iFrame = nFrame - 1; iFrame >= 0; iFrame--) {
        for (int i = 0; i < 24; i++) {
            double alpha = (i < 12) ? chromaEmaAlphaBass : chromaEmaAlpha;
            runningChroma[i] = iFrame == nFrame - 1 ? rawChromas[iFrame * 24 + i]
                : alpha * rawChromas[iFrame * 24 + i] + (1 - alpha) * runningChroma[i];
            smoothedChromas[iFrame * 24 + i] += runningChroma[i] / 2;
        }
    }

    // Scores kept as scored, before the exponential and the normalisation below
    // discard how well each candidate actually fit (see the gate further down).
    double *rawChordValue = malloc(nChord * sizeof(double));
    const int *sameNotesRoots = c->dictionary->sameNotesRoots;
    int *lowest = malloc((nFrame > 0 ? nFrame : 1) * sizeof(int));
    lowest_notes(c->semitones.data, nFrame, lowest);
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
            if (lowest[iFrame] >= 0 && lowest[iFrame] != iChord % 12 && (sameNotesRoots[iChord] >> lowest[iFrame] & 1)) {
                tempchordvalue *= otherRootFactor;
            }
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
        chordino_inspect_frame(realtime_to_seconds(timestamps[iFrame]),
                               rawChromas + iFrame * 24, smoothedChroma, currentChordSalience,
                               (const float *) c->semitones.data + iFrame * 84, gate);
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

    vector_clear(&c->chords);
    vector_clear(&c->notes);
    vector_push(&c->chords, &(chordino_chord_t) {c->dictionary->names[chordpath[0]], timestamps[0]}, 1);
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
            vector_push(&c->chords, &(chordino_chord_t) {c->dictionary->names[chordpath[iFrame]], timestamps[iFrame]}, 1);
            oldchord = chordpath[iFrame];
            // chord notes
            for (int iNote = 0; iNote < nOldnotes; ++iNote) { // finish duration of old chord
                oldnotes[iNote].duration = realtime_add(oldnotes[iNote].duration, timestamps[iFrame]);
            }
            vector_push(&c->notes, oldnotes, nOldnotes);
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

    float *logLikelihood = vector_resize(&c->logLikelihood, nFrame);
    for (int iFrame = 0; iFrame < nFrame; ++iFrame) {
        logLikelihood[iFrame] = -log(scale[iFrame]);
    }

    vector_push(&c->chords, &(chordino_chord_t) {"N", timestamps[nFrame-1]}, 1);

    for (int iNote = 0; iNote < nOldnotes; ++iNote) { // finish duration of old chord
        oldnotes[iNote].duration = realtime_add(oldnotes[iNote].duration, timestamps[nFrame-1]);
    }
    vector_push(&c->notes, oldnotes, nOldnotes);

    free(delta);
    free(trans);
    free(init);
    free(scale);
    free(chordpath);
    free(rawChordValue);
    free(lowest);
    free(smoothedChromas);
    free(chordogram);
    free(tunedSpec);
    free(bassNorm);
    free(trebleNorm);
    free(noteCountPenalty);

    report(progress, progressSalience, 1, 1, 1);

    c->result.chordCount = c->chords.count;
    c->result.chords = c->chords.data;
    c->result.noteCount = c->notes.count;
    c->result.notes = c->notes.data;
    c->result.frameCount = nFrame;
    c->result.times = c->times.data;
    c->result.harmonicChange = c->harmonicChange.data;
    c->result.logLikelihood = c->logLikelihood.data;
    return &c->result;
}
