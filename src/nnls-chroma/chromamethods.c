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

// What upstream wrote in C++, in C: the float and double maths of each line is
// the one the C++ overloads picked (log2f and powf for floats, fabsf and fabs),
// so that every value comes out as it did.

#include "chromamethods.h"

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/** Special Convolution
    Special convolution is as long as the convolvee, i.e. the first argument.
	In the "valid" core part of the convolution it contains the usual convolution
	values, but the parts at the beginning (ending) that would normally be
	calculated using zero padding simply have the same values as the first
	(last) valid convolution bin.
**/

void special_convolution(const float *convolvee, const float *kernel, int lenKernel, float *Z)
{
    float s;
    int m, n;
    int lenConvolvee = nNote;

    assert(lenKernel % 2 != 0); // no exception handling !!!
    for (n = 0; n < nNote; n++) Z[n] = 0;

    for (n = lenKernel - 1; n < lenConvolvee; n++) {
    	s=0.0;
    	for (m = 0; m < lenKernel; m++) {
            s += convolvee[n-m] * kernel[m];
    	}
        Z[n -lenKernel/2] = s;
    }

    // fill upper and lower pads
    for (n = 0; n < lenKernel/2; n++) Z[n] = Z[lenKernel/2];
    for (n = lenConvolvee; n < lenConvolvee +lenKernel/2; n++) Z[n - lenKernel/2] =
                                                                   Z[lenConvolvee - lenKernel/2 -  1];
}

static float cospuls(float x, float centre, float width)
{
    float recipwidth = 1.0/width;
    if (fabsf(x - centre) <= 0.5 * width) {
        return cos((x-centre)*2*M_PI*recipwidth)*.5+.5;
    }
    return 0.0;
}

static float pitchCospuls(float x, float centre, int binsperoctave)
{
    float warpedf = -binsperoctave * (log2f(centre) - log2f(x));
    float out = cospuls(warpedf, 0.0, 2.0);
    // now scale to correct for note density
    float c = log(2.0)/binsperoctave;
    if (x > 0) {
        out = out / (c * x);
    } else {
        out = 0;
    }
    return out;
}

/**
 * Calculates a matrix that can be used to linearly map from the magnitude spectrum to a pitch-scale spectrum.
 * The main purpose of the function is to change the values in the "matrix" pointed to by *outmatrix
 */
void log_freq_matrix(int fs, int blocksize, float *outmatrix) {
	// TODO: rewrite so that everyone understands what is done here.
	// TODO: make this more general, such that it works with all minoctave, maxoctave and whatever nBPS (or check if it already does)

    int binspersemitone = nBPS;
    int minoctave = 0; // this must be 0
    int maxoctave = 7; // this must be 7
    int oversampling = 80;

    // linear frequency vector
    int nFFT = blocksize/2;
    float *fft_f = malloc(nFFT * sizeof(float));
    for (int i = 0; i < blocksize/2; ++i) {
        fft_f[i] = i * (fs * 1.0 / blocksize);
    }
    float fft_width = fs * 2.0 / blocksize;

    // linear oversampled frequency vector
    float *oversampled_f = malloc(oversampling * blocksize/2 * sizeof(float));
    for (int i = 0; i < oversampling * blocksize/2; ++i) {
        oversampled_f[i] = i * ((fs * 1.0 / blocksize) / oversampling);
    }

    // pitch-spaced frequency vector
    int minMIDI = 21 + minoctave * 12 - 1; // this includes one additional semitone!
    int maxMIDI = 21 + maxoctave * 12; // this includes one additional semitone!
    int nCQ = (maxMIDI - minMIDI) * binspersemitone + 1;
    float *cq_f = malloc(nCQ * sizeof(float));
    int nFilled = 0;
    float oob = 1.0/binspersemitone; // one over binspersemitone
    for (int i = minMIDI; i < maxMIDI; ++i) {
        for (int k = 0; k < binspersemitone; ++k)	 {
            cq_f[nFilled++] = 440 * pow(2.0,0.083333333333 * (i+oob*k-69));
        }
    }
    cq_f[nFilled++] = 440 * pow(2.0,0.083333 * (maxMIDI-69));

    float fft_activation[2 * 80];
    for (int iOS = 0; iOS < 2 * oversampling; ++iOS) {
        fft_activation[iOS] = cospuls(oversampled_f[iOS],fft_f[1],fft_width);
    }

    for (int i = 0; i < nFFT * nCQ; ++i) {
        outmatrix[i] = 0.f;
    }

    float cq_activation;
    for (int iFFT = 1; iFFT < nFFT; ++iFFT) {
        // find frequency stretch where the oversampled vector can be non-zero (i.e. in a window of width fft_width around the current frequency)
        int curr_start = oversampling * iFFT - oversampling;
        int curr_end = oversampling * iFFT + oversampling; // don't know if I should add "+1" here
        for (int iCQ = 0; iCQ < nCQ; ++iCQ) {
            if (cq_f[iCQ] * pow(2.0, 0.084) + fft_width > fft_f[iFFT] && cq_f[iCQ] * pow(2.0, -0.084 * 2) - fft_width < fft_f[iFFT]) { // within a generous neighbourhood
                for (int iOS = curr_start; iOS < curr_end; ++iOS) {
                    cq_activation = pitchCospuls(oversampled_f[iOS],cq_f[iCQ],binspersemitone*12);
                    outmatrix[iFFT + nFFT * iCQ] += cq_activation * fft_activation[iOS-curr_start];
                }
             }
        }
    }
    free(fft_f);
    free(oversampled_f);
    free(cq_f);
}

void dictionary_matrix(float* dm, float s_param) {
	// TODO: make this more general, such that it works with all minoctave, maxoctave and even more than one note per semitone
    int binspersemitone = nBPS;
    int minoctave = 0; // this must be 0
    int maxoctave = 7; // this must be 7

    float floatbin;
    float curr_amp;
    // now for every combination calculate the matrix element
    for (int iOut = 0; iOut < 12 * (maxoctave - minoctave); ++iOut) {
        for (int iHarm = 1; iHarm <= 20; ++iHarm) {
            floatbin = ((iOut + 1) * binspersemitone + 1) + binspersemitone * 12 * log2(iHarm);
            curr_amp = powf(s_param,(float)(iHarm-1));
            for (int iNote = 0; iNote < nNote; ++iNote) {
                if (fabs(iNote+1.0-floatbin)<2) {
                    dm[iNote  + nNote * iOut] += cospuls(iNote+1.0, floatbin, binspersemitone + 0.0) * curr_amp;
                }
            }
        }
    }
}

// The name suffixes of each chord type: as usual, then in Harte syntax
static const char *chordTypeNames[] = {
    "", //=1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,1,0,0,1,0,0,0,0
    "", // =0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,1,0,0,1,0,0,0,0
    "m", //=1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,1,0,0,0,1,0,0,0,0
    "m", //=0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,1,0,0,0,1,0,0,0,0
    "º", //=0,0,0,0,0,0,0,0,0,0,1,0,1,0,0,1,0,0,1,0,0,0,1,0
    "º", //=1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,1,0,0,1,0,0,0,1,0
    "6", //=1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,1,0,0,1,0,1,0,0
    "7", //=1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,1,0,0,1,0,0,1,0
    "7M", //=1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,1,0,0,1,0,0,0,1
    "m7", //=1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,1,0,0,0,1,0,0,1,0
    "m6", //=1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,1,0,0,0,1,0,1,0,0
    "", //=0,0,0,0,1,0,0,0,0,0,0,0,1,0,0,0,1,0,0,1,0,0,0,0
    "", //=0,0,0,0,0,0,0,1,0,0,0,0,1,0,0,0,1,0,0,1,0,0,0,0
    "º", //=1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,1,0,0,1,0,0,0,0,0
    "aug", //=1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,1,0,0,0,1,0,0,0
    "", //=0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,1,0,0,1,0,0,0,0
    "", //=0,0,0,0,0,0,0,0,0,0,1,0,1,0,0,0,1,0,0,1,0,0,0,0
    "7", //=0,0,0,0,1,0,0,0,0,0,0,0,1,0,0,0,1,0,0,1,0,0,1,0
    "sus2",
    "sus2",
    "sus4",
    "sus4",
    "5",
    "5",
    "2",
    "2",
    "4",
    "4",
    "9",
    "11",
    "13",
    // Minor inversions. Major had /3rd and /5th slash templates from the start but
    // minor had none, so Am/C and Dm/F were not merely mis-scored, they had no
    // template to win: the closest reading available was the root-position chord.
    "m", //=0,0,0,1,0,0,0,0,0,0,0,0,1,0,0,1,0,0,0,1,0,0,0,0
    "m", //=0,0,0,0,0,0,0,1,0,0,0,0,1,0,0,1,0,0,0,1,0,0,0,0
    // Dominant 7th with the 7th in the bass (D7/C, G7/F): the usual passing chord
    // into the next chord's first inversion, which had no template at all.
    "7", //=0,0,0,0,0,0,0,0,0,0,1,0,1,0,0,0,1,0,0,1,0,0,1,0
};

static const char *harteTypeNames[] = {
    "", //=1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,1,0,0,1,0,0,0,0
    "", // =0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,1,0,0,1,0,0,0,0
    ":min", //=1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,1,0,0,0,1,0,0,0,0
    ":min", //=0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,1,0,0,0,1,0,0,0,0
    ":hdim7", //=0,0,0,0,0,0,0,0,0,0,1,0,1,0,0,1,0,0,1,0,0,0,1,0
    ":hdim7", //=1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,1,0,0,1,0,0,0,1,0
    ":maj6", //=1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,1,0,0,1,0,1,0,0
    ":7", //=1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,1,0,0,1,0,0,1,0
    ":maj7", //=1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,1,0,0,1,0,0,0,1
    ":min7", //=1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,1,0,0,0,1,0,0,1,0
    ":min6", //=1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,1,0,0,0,1,0,1,0,0
    "", //=0,0,0,0,1,0,0,0,0,0,0,0,1,0,0,0,1,0,0,1,0,0,0,0
    "", //=0,0,0,0,0,0,0,1,0,0,0,0,1,0,0,0,1,0,0,1,0,0,0,0
    ":dim", //=1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,1,0,0,1,0,0,0,0,0
    ":aug", //=1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,1,0,0,0,1,0,0,0
    "", //=0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,1,0,0,1,0,0,0,0
    "", //=0,0,0,0,0,0,0,0,0,0,1,0,1,0,0,0,1,0,0,1,0,0,0,0
    ":7", //=0,0,0,0,1,0,0,0,0,0,0,0,1,0,0,0,1,0,0,1,0,0,1,0
    ":sus2",
    ":sus2",
    ":sus4",
    ":sus4",
    ":5",
    ":5",
    ":add9",
    ":add9",
    ":add11",
    ":add11",
    ":9",
    ":11",
    ":13",
    ":min",
    ":min",
    ":7",
};

// The bass (0-11) and treble (12-23) template of each chord type, on A
static const float chordvaluearray[] = {1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,1,0,0,1,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,1,0,0,1,0,0,0,0,
    1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,1,0,0,0,1,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,1,0,0,0,1,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,1,0,1,0,0,1,0,0,1,0,0,0,1,0,
    1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,1,0,0,1,0,0,0,1,0,
    1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,1,0,0,1,0,1,0,0,
    1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,1,0,0,1,0,0,1,0,
    1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,1,0,0,1,0,0,0,1,
    1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,1,0,0,0,1,0,0,1,0,
    1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,1,0,0,0,1,0,1,0,0,
    0,0,0,0,1,0,0,0,0,0,0,0,1,0,0,0,1,0,0,1,0,0,0,0,
    0,0,0,0,0,0,0,1,0,0,0,0,1,0,0,0,1,0,0,1,0,0,0,0,
    1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,1,0,0,1,0,0,0,0,0,
    1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,1,0,0,0,1,0,0,0,
    0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,1,0,0,1,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,1,0,1,0,0,0,1,0,0,1,0,0,0,0,
    0,0,0,0,1,0,0,0,0,0,0,0,1,0,0,0,1,0,0,1,0,0,1,0,
    // sus2: root,2nd,5th
    1,0,0,0,0,0,0,0,0,0,0,0,1,0,1,0,0,0,0,1,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,1,0,1,0,0,0,0,1,0,0,0,0,
    // sus4: root,4th,5th
    1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,1,0,1,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,1,0,1,0,0,0,0,
    // 5 (power chord): root,5th
    1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,
    // 2 (add2): root,2nd,3rd,5th
    1,0,0,0,0,0,0,0,0,0,0,0,1,0,1,0,1,0,0,1,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,1,0,1,0,1,0,0,1,0,0,0,0,
    // 4 (add4): root,3rd,4th,5th
    1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,1,1,0,1,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,1,1,0,1,0,0,0,0,
    // 9 (dominant 9th): root,2nd,3rd,5th,b7
    1,0,0,0,0,0,0,0,0,0,0,0,1,0,1,0,1,0,0,1,0,0,1,0,
    // 11 (dominant 11th, 3rd omitted): root,2nd,4th,5th,b7
    1,0,0,0,0,0,0,0,0,0,0,0,1,0,1,0,0,1,0,1,0,0,1,0,
    // 13 (dominant 13th, 11th omitted): root,2nd,3rd,5th,6th/13th,b7
    1,0,0,0,0,0,0,0,0,0,0,0,1,0,1,0,1,0,0,1,0,1,1,0,
    // m with b3 in the bass (Am/C, Dm/F)
    0,0,0,1,0,0,0,0,0,0,0,0,1,0,0,1,0,0,0,1,0,0,0,0,
    // m with 5th in the bass (Am/E, Dm/A)
    0,0,0,0,0,0,0,1,0,0,0,0,1,0,0,1,0,0,0,1,0,0,0,0,
    // 7 with b7 in the bass (D7/C, G7/F)
    0,0,0,0,0,0,0,0,0,0,1,0,1,0,0,0,1,0,0,1,0,0,1,0};

enum { nChordType = sizeof(chordTypeNames) / sizeof(chordTypeNames[0]) };
_Static_assert(sizeof(harteTypeNames) == sizeof(chordTypeNames), "a Harte name per chord type");
_Static_assert(sizeof(chordvaluearray) / sizeof(chordvaluearray[0]) == 24 * nChordType, "a template per chord type");

// Upstream looked for a user-supplied chord.dict alongside the plugin binary and
// parsed it here. There is no filesystem in the wasm build, so the dictionary
// always comes from the tables above.
chord_dictionary_t *chord_dictionary_create(float boostN, bool harteSyntax) {

	const char* notenames[24] = {
	    "A  (bass)","Bb (bass)","B  (bass)","C  (bass)","C# (bass)","D  (bass)","Eb (bass)","E  (bass)","F  (bass)","F# (bass)","G  (bass)","Ab (bass)",
	    "A","Bb","B","C","C#","D","Eb","E","F","F#","G","Ab"};

	const char* bassnames[13][12] ={
	    {"A","","B","C","C#","D","","E","","F#","G","G#"},
	    {"Bb","","C","Db","D","Eb","","F","","G","Ab","A"},
	    {"B","","C#","D","D#","E","","F#","","G#","A","A#"},
	    {"C","","D","Eb","E","F","","G","","A","Bb","B"},
	    {"C#","","D#","E","E#","F#","","G#","","A#","B","B#"},
	    {"D","","E","F","F#","G","","A","","B","C","C#"},
	    {"Eb","","F","Gb","G","Ab","","Bb","","C","Db","D"},
	    {"E","","F#","G","G#","A","","B","","C#","D","D#"},
	    {"F","","G","Ab","A","Bb","","C","","D","Eb","E"},
	    {"F#","","G#","A","A#","B","","C#","","D#","E","E#"},
	    {"G","","A","Bb","B","C","","D","","E","F","F#"},
	    {"Ab","","Bb","Cb","C","Db","","Eb","","F","Gb","G"},
            {"1","","2","b3","3","4","","5","","6","b7","7"}
	};

    const float *tempChordDict = chordvaluearray;
    const char **tempChordNames = harteSyntax ? harteTypeNames : chordTypeNames;

    chord_dictionary_t *dictionary = malloc(sizeof(chord_dictionary_t));
    int size = nChordType * 12 + 1;
    dictionary->size = size;
    dictionary->templates = malloc(size * 24 * sizeof(float));
    dictionary->names = malloc(size * sizeof(char *));
    dictionary->noteStart = malloc((size + 1) * sizeof(int));
    dictionary->notes = malloc(size * 25 * sizeof(int)); // at most a bass note, then 12 and 12
    float *loadedChordDict = dictionary->templates;
    int nLoaded = 0;
    int nNotes = 0;
    int iChordOut = 0;

    for (int iType = 0; iType < nChordType; ++iType) {
        // now make all 12 chords of every type
        for (int iSemitone = 0; iSemitone < 12; iSemitone++) {
            dictionary->noteStart[iChordOut] = nNotes;
            // add bass slash notation
            const char *slashNotation = "";
            for (int kSemitone = 1; kSemitone < 12; kSemitone++) {
                if (tempChordDict[24*iType+(kSemitone) % 12] > 0.99) {
					if (!harteSyntax) {
						slashNotation = bassnames[iSemitone][kSemitone];
					} else {
					        slashNotation = bassnames[12][kSemitone];
                                        }
                }
            }
            if (slashNotation[0] == 0) dictionary->notes[nNotes++] = MIDI_basenote + (iSemitone+12) % 12;
            for (int kSemitone = 0; kSemitone < 12; kSemitone++) { // bass pitch classes
                float bassValue = 0;
                if (tempChordDict[24*iType+(kSemitone - iSemitone + 12) % 12]==1) {
                    bassValue = 1;
                    dictionary->notes[nNotes++] = MIDI_basenote + (kSemitone+12) % 12;
                } else {
                    // Non-designated chord notes still get some bass credit, since they
                    // do show up down there, but this gap against the designated bass
                    // note is the *only* thing telling C from C/E -- their treble
                    // templates are identical and cancel out of the comparison. At 0.5
                    // the contrast was half what it is here.
                    if (tempChordDict[24*iType+((kSemitone - iSemitone + 12) % 12) + 12] == 1) bassValue = 0.25;
                }
                loadedChordDict[nLoaded++] = bassValue;
            }
            for (int kSemitone = 0; kSemitone < 12; kSemitone++) { // chord pitch classes
                loadedChordDict[nLoaded++] = tempChordDict[24*iType+((kSemitone - iSemitone + 12) % 12) + 12];
                if (tempChordDict[24*iType+((kSemitone - iSemitone + 12) % 12) + 12] > 0) dictionary->notes[nNotes++] = MIDI_basenote + (kSemitone+12+6) % 12 - 6 + 24;
            }
            const char *root = notenames[12+iSemitone];
            const char *type = tempChordNames[iType];
            char *chordName = malloc(strlen(root) + strlen(type) + 1 + strlen(slashNotation) + 1);
            strcpy(chordName, root);
            strcat(chordName, type);
            if (slashNotation[0] != 0) {
                strcat(chordName, "/");
                strcat(chordName, slashNotation);
            }
            dictionary->names[iChordOut++] = chordName;
        }
    }


    dictionary->sameNotesRoots = calloc(size, sizeof(int));
    for (int iChord = 0; iChord < iChordOut; iChord++) {
        for (int jChord = 0; jChord < iChordOut; jChord++) {
            bool same = jChord % 12 != iChord % 12;
            for (int iBin = 12; same && iBin < 24; iBin++) {
                same = (loadedChordDict[24 * iChord + iBin] > 0) == (loadedChordDict[24 * jChord + iBin] > 0);
            }
            if (same) dictionary->sameNotesRoots[iChord] |= 1 << (jChord % 12);
        }
    }

    // N type
    dictionary->noteStart[iChordOut] = nNotes;
    char *noChord = malloc(2);
    strcpy(noChord, "N");
    dictionary->names[iChordOut++] = noChord;
    dictionary->noteStart[iChordOut] = nNotes;
    for (int kSemitone = 0; kSemitone < 12; kSemitone++) loadedChordDict[nLoaded++] = 0.5;
    for (int kSemitone = 0; kSemitone < 12; kSemitone++) loadedChordDict[nLoaded++] = 1.0;
    float exponent = 2.0;
    for (int iChord = 0; iChord < nLoaded/24; iChord++) {
        float stand = 0;
        for (int iST = 0; iST < 24; ++iST) {
            stand += powf(fabsf(loadedChordDict[24 * iChord + iST]),exponent)/24;
        }
        if (iChord < nLoaded/24 - 1) {
            stand = powf(stand,1.0f/exponent);
        } else {
            stand = powf(stand,1.0f/exponent) / (1+boostN);
        }
        for (int iST = 0; iST < 24; ++iST) {
            loadedChordDict[24 * iChord + iST] /= stand;
        }

    }
    return dictionary;
}

void chord_dictionary_destroy(chord_dictionary_t *dictionary) {
    for (int i = 0; i < dictionary->size; ++i) free(dictionary->names[i]);
    free(dictionary->names);
    free(dictionary->templates);
    free(dictionary->noteStart);
    free(dictionary->notes);
    free(dictionary->sameNotesRoots);
    free(dictionary);
}
