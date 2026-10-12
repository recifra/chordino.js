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

#include "Chordino.h"

#include "ChordinoInspect.h"
#include "chromamethods.h"
#include "viterbi.h"

#include <cstdlib>
#include <fstream>
#include <cmath>

#include <algorithm>

const bool debug_on = false;
// Relative weight of the bass half of the match against the treble half. The two
// halves are normalized independently (see below), so this is the only thing setting
// how much say the bass note has; it is a plain ratio between two comparable
// quantities rather than a gain applied to one of them.
const double bassWeight = 0.6;
// Leaky integrator applied to chroma before chord matching (forwards and backwards,
// see getRemainingFeatures), so a chord needs to be
// sustained for roughly this long to dominate the signal used for scoring -- a
// single noisy/transient frame can't flip the match. alpha tuned for an effective
// window of ~300ms at the analysis frame rate (preferred step size 2048 samples,
// ~46ms/frame at 44.1kHz): alpha = 1 - exp(-frameMs/windowMs).
const double chromaEmaAlpha = 0.15;
// The bass bins get a much faster integrator. Which bass note is sounding is the
// only thing separating a chord from its inversions (F and F/A share all three
// treble notes and differ only by which bass bin is 1.0 instead of 0.25), so it is a
// small difference riding on a large common part -- exactly what slow smoothing
// erases, on top of the previous chord's bass bleeding into the current one.
const double chromaEmaAlphaBass = 0.5;
// Confidence gate. The salience below is normalized to sum to 1, which makes the
// match a forced choice: something always wins, however badly it fits. These two
// ratios restore a real "none of the above" by comparing the winner's raw score
// against the no-chord hypothesis and against the best candidate on a *different*
// root. Speech and singing slide through roots (every syllable implies a new one),
// so they score high in absolute terms but win by a tiny margin over rival roots --
// which is exactly what this catches. Variants sharing the winner's root (C vs C7
// vs Csus4) are refinements rather than ambiguity, so they don't count against it.
// Both are ratios, so they don't need recalibrating when input gain changes.
const double minChordVsNoChord = 1.05;
const double minRootMargin = 1.05;
// How hard a frame that fails the gate is pushed towards N.
const double noConfidenceDamping = 0.01;

Chordino::Chordino(float inputSampleRate) :
    NNLSBase(inputSampleRate),
    m_chorddict(0),
    m_chordnotes(0),
    m_chordnames(0)    
{
    if (debug_on) cerr << "--> Chordino" << endl;
}

Chordino::~Chordino()
{
    if (debug_on) cerr << "--> ~Chordino" << endl;
}

string
Chordino::getIdentifier() const
{
    if (debug_on) cerr << "--> getIdentifier" << endl;
    return "chordino";
}

string
Chordino::getName() const
{
    if (debug_on) cerr << "--> getName" << endl;
    return "Chordino";
}

string
Chordino::getDescription() const
{
    if (debug_on) cerr << "--> getDescription" << endl;
    return "Chordino provides a simple chord transcription based on NNLS Chroma (as in the NNLS Chroma plugin). A built-in set of chord profiles is used to calculate frame-wise chord similarities. A simple (non-state-of-the-art!) algorithm smoothes these to provide a chord transcription using a standard HMM/Viterbi approach.";
}

// Parameters used to be exposed through Vamp's named-parameter machinery so a host
// like Sonic Visualiser could tune them at runtime. This build embeds its own host
// and never sets them, so the defaults in NNLSBase's constructor are the only values
// that ever apply; the descriptor list is gone and PluginBase's empty default is
// inherited instead.

Chordino::OutputList
Chordino::getOutputDescriptors() const
{
    if (debug_on) cerr << "--> getOutputDescriptors" << endl;
    OutputList list;
    
    int index = 0;

    float featureRate =
        (m_stepSize == 0) ? m_inputSampleRate/2048 : m_inputSampleRate/m_stepSize;
    
    OutputDescriptor d7;
    d7.identifier = "simplechord";
    d7.name = "Chord Estimate";
    d7.description = "Estimated chord times and labels.";
    d7.unit = "";
    d7.hasFixedBinCount = true;
    d7.binCount = 0;
    d7.hasKnownExtents = false;
    d7.isQuantized = false;
    d7.sampleType = OutputDescriptor::VariableSampleRate;
    d7.hasDuration = false;
    d7.sampleRate = featureRate;
    list.push_back(d7);
    m_outputChords = index++;
    
    OutputDescriptor chordnotes;
    chordnotes.identifier = "chordnotes";
    chordnotes.name = "Note Representation of Chord Estimate";
    chordnotes.description = "A simple representation of the estimated chord with bass note (if applicable) and chord notes.";
    chordnotes.unit = "MIDI units";
    chordnotes.hasFixedBinCount = true;
    chordnotes.binCount = 1;
    chordnotes.hasKnownExtents = true;
    chordnotes.minValue = 0;
    chordnotes.maxValue = 127;
    chordnotes.isQuantized = true;
    chordnotes.quantizeStep = 1;
    chordnotes.sampleType = OutputDescriptor::VariableSampleRate;
    chordnotes.hasDuration = true;
    chordnotes.sampleRate = featureRate;
    list.push_back(chordnotes);
    m_outputChordnotes = index++;
    
    OutputDescriptor d8;
    d8.identifier = "harmonicchange";
    d8.name = "Harmonic Change Value";
    d8.description = "An indication of the likelihood of harmonic change. Depends on the chord dictionary. Calculation is different depending on whether the Viterbi algorithm is used for chord estimation, or the simple chord estimate.";
    d8.unit = "";
    d8.hasFixedBinCount = true;
    d8.binCount = 1;
    d8.hasKnownExtents = false;
    d8.isQuantized = false;
    d8.sampleType = OutputDescriptor::FixedSampleRate;
    d8.sampleRate = featureRate;
    d8.hasDuration = false;
    list.push_back(d8);
    m_outputHarmonicChange = index++;
  
    OutputDescriptor loglikelihood;
    loglikelihood.identifier = "loglikelihood";
    loglikelihood.name = "Log-Likelihood of Chord Estimate";
    loglikelihood.description = "Logarithm of the likelihood value of the simple chord estimate.";
    loglikelihood.unit = "";
    loglikelihood.hasFixedBinCount = true;
    loglikelihood.binCount = 1;
    loglikelihood.hasKnownExtents = false;
    loglikelihood.isQuantized = false;
    loglikelihood.sampleType = OutputDescriptor::FixedSampleRate;
    loglikelihood.sampleRate = featureRate;
    loglikelihood.hasDuration = false;
    list.push_back(loglikelihood);
    m_outputLoglikelihood = index++;
  
    return list;
}

bool
Chordino::initialise(size_t channels, size_t stepSize, size_t blockSize)
{	
    if (debug_on) {
        cerr << "--> initialise";
    }
    
    if (!NNLSBase::initialise(channels, stepSize, blockSize)) {
        return false;
    }
    m_chordnames = chordDictionary(&m_chorddict, &m_chordnotes, m_boostN, m_harte_syntax);
    return true;
}

void
Chordino::reset()
{
    if (debug_on) cerr << "--> reset";
    NNLSBase::reset();
}

Chordino::FeatureSet
Chordino::process(const float *const *inputBuffers, Vamp::RealTime timestamp)
{   
    if (debug_on) cerr << "--> process" << endl;

    NNLSBase::baseProcess(inputBuffers, timestamp);

    return FeatureSet();
}

Chordino::FeatureSet
Chordino::getRemainingFeatures()
{
    // cerr << hw[0] << hw[1] << endl;
    if (debug_on) cerr << "--> getRemainingFeatures" << endl;    
    FeatureSet fsOut;
    if (m_logSpectrum.size() == 0) return fsOut;
    int nChord = m_chordnames.size();

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
    vector<double> bassNorm(nChord, 1.0);
    vector<double> trebleNorm(nChord, 1.0);
    vector<double> noteCountPenalty(nChord, 1.0);
    for (int iChord = 0; iChord < nChord; iChord++) {
        double bassSumSq = 0;
        double trebleSumSq = 0;
        int noteCount = 0;
        for (int iBin = 0; iBin < 12; iBin++) {
            double v = m_chorddict[24 * iChord + iBin];
            bassSumSq += v * v;
        }
        for (int iBin = 12; iBin < 24; iBin++) {
            double v = m_chorddict[24 * iChord + iBin];
            trebleSumSq += v * v;
            if (v > 0) noteCount++;
        }
        if (bassSumSq > 0) bassNorm[iChord] = sqrt(bassSumSq);
        if (trebleSumSq > 0) trebleNorm[iChord] = sqrt(trebleSumSq);
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
        meanTuningReal += m_meanTunings[iBPS] * cosvalues[iBPS];
        meanTuningImag += m_meanTunings[iBPS] * sinvalues[iBPS];
    }
    float cumulativetuning = 440 * pow(2,atan2(meanTuningImag, meanTuningReal)/(24*M_PI));
    float normalisedtuning = atan2(meanTuningImag, meanTuningReal)/(2*M_PI);
    int intShift = floor(normalisedtuning * 3);
    float floatShift = normalisedtuning * 3 - intShift; // floatShift is a really bad name for this
		    
    char buffer0 [50];
		
    sprintf(buffer0, "estimated tuning: %0.1f Hz", cumulativetuning);
		    
		    
    /** Tune Log-Frequency Spectrogram
        calculate a tuned log-frequency spectrogram (currentTunedSpec): use the tuning estimated above (kinda f0) to 
        perform linear interpolation on the existing log-frequency spectrogram (kinda currentLogSpectrum).
    **/
    if (debug_on) cerr << endl << "[Chordino Plugin] Tuning Log-Frequency Spectrogram ... ";
					
    int count = 0;
		
    FeatureList tunedSpec;
    int nFrame = m_logSpectrum.size();
    
    vector<Vamp::RealTime> timestamps;

    for (FeatureList::iterator i = m_logSpectrum.begin(); i != m_logSpectrum.end(); ++i) {
        Feature currentLogSpectrum = *i;
        Feature currentTunedSpec; // tuned log-frequency spectrum
        currentTunedSpec.hasTimestamp = true;
        currentTunedSpec.timestamp = currentLogSpectrum.timestamp;
        timestamps.push_back(currentLogSpectrum.timestamp);
        currentTunedSpec.values.push_back(0.0); currentTunedSpec.values.push_back(0.0); // set lower edge to zero
		
        if (m_tuneLocal) {
            intShift = floor(m_localTuning[count] * 3);
            floatShift = m_localTuning[count] * 3 - intShift; // floatShift is a really bad name for this
        }
		        
        // cerr << intShift << " " << floatShift << endl;
		        
        for (int k = 2; k < (int)currentLogSpectrum.values.size() - 3; ++k) { // interpolate all inner bins
            float tempValue = currentLogSpectrum.values[k + intShift] * (1-floatShift) + currentLogSpectrum.values[k+intShift+1] * floatShift;
            currentTunedSpec.values.push_back(tempValue);
        }
		        
        currentTunedSpec.values.push_back(0.0); currentTunedSpec.values.push_back(0.0); currentTunedSpec.values.push_back(0.0); // upper edge
        vector<float> runningmean = SpecialConvolution(currentTunedSpec.values,hw);
        vector<float> runningstd;
        for (int i = 0; i < nNote; i++) { // first step: squared values into vector (variance)
            runningstd.push_back((currentTunedSpec.values[i] - runningmean[i]) * (currentTunedSpec.values[i] - runningmean[i]));
        }
        runningstd = SpecialConvolution(runningstd,hw); // second step convolve
        for (int i = 0; i < nNote; i++) { 
            runningstd[i] = sqrt(runningstd[i]); // square root to finally have running std
            if (runningstd[i] > 0) {
                // currentTunedSpec.values[i] = (currentTunedSpec.values[i] / runningmean[i]) > thresh ? 
                // 		                    (currentTunedSpec.values[i] - runningmean[i]) / pow(runningstd[i],m_whitening) : 0;
                currentTunedSpec.values[i] = (currentTunedSpec.values[i] - runningmean[i]) > 0 ?
                    (currentTunedSpec.values[i] - runningmean[i]) / pow(runningstd[i],m_whitening) : 0;
            }
            if (currentTunedSpec.values[i] < 0) {
                cerr << "ERROR: negative value in logfreq spectrum" << endl;
            }
        }
        tunedSpec.push_back(currentTunedSpec);
        count++;
    }
    if (debug_on) cerr << "done." << endl;
	    
    /** Semitone spectrum and chromagrams
        Semitone-spaced log-frequency spectrum derived from the tuned log-freq spectrum above. the spectrum
        is inferred using a non-negative least squares algorithm.
        Three different kinds of chromagram are calculated, "treble", "bass", and "both" (which means 
        bass and treble stacked onto each other).
    **/
    if (m_useNNLS == 0) {
        if (debug_on) cerr << "[Chordino Plugin] Mapping to semitone spectrum and chroma ... ";
    } else {
        if (debug_on) cerr << "[Chordino Plugin] Performing NNLS and mapping to chroma ... ";
    }

	    
    vector<vector<double> > chordogram;
    vector<vector<int> > scoreChordogram;
    vector<float> chordchange = vector<float>(tunedSpec.size(),0);
    count = 0;

    FeatureList chromaList;

    bool clipwarned = false;

    // Raw stacked chroma of every frame; chord matching runs on a smoothed copy
    // computed once all of them are known (see below).
    vector<vector<float> > rawChromas;
#ifdef CHORDINO_INSPECT
    vector<vector<float> > inspectSemitones;
    chordinoInspectBegin(m_chordnames);
#endif

    for (FeatureList::iterator it = tunedSpec.begin(); it != tunedSpec.end(); ++it) {
        Feature currentTunedSpec = *it; // logfreq spectrum
        Feature currentChromas; // treble and bass chromagram

        currentChromas.hasTimestamp = true;
        currentChromas.timestamp = currentTunedSpec.timestamp;    

        float b[nNote];
	
        bool some_b_greater_zero = false;
        float sumb = 0;
        for (int i = 0; i < nNote; i++) {
            // b[i] = m_dict[(nNote * count + i) % (nNote * 84)];
            b[i] = currentTunedSpec.values[i];
            sumb += b[i];
            if (b[i] > 0) {
                some_b_greater_zero = true;
            }            
        }
	    
        // here's where the non-negative least squares algorithm calculates the note activation x
	
        vector<float> chroma = vector<float>(12, 0);
        vector<float> basschroma = vector<float>(12, 0);
        float currval;
        int iSemitone = 0;
#ifdef CHORDINO_INSPECT
        float semitones[84] = {0};
#endif
			
        if (some_b_greater_zero) {
            if (m_useNNLS == 0) {
                for (int iNote = nBPS/2 + 2; iNote < nNote - nBPS/2; iNote += nBPS) {
                    currval = 0;
                    for (int iBPS = -nBPS/2; iBPS < nBPS/2+1; ++iBPS) {
                        currval += b[iNote + iBPS] * (1-abs(iBPS*1.0/(nBPS/2+1)));						
                    }
                    chroma[iSemitone % 12] += currval * treblewindow[iSemitone];
                    basschroma[iSemitone % 12] += currval * basswindow[iSemitone];
#ifdef CHORDINO_INSPECT
                    semitones[iSemitone] = currval;
#endif
                    iSemitone++;
                }
		        
            } else {
                float x[84+1000];
                for (int i = 1; i < 1084; ++i) x[i] = 1.0;
                vector<int> signifIndex;
                int index=0;
                sumb /= 84.0;
                for (int iNote = nBPS/2 + 2; iNote < nNote - nBPS/2; iNote += nBPS) {
                    float currval = 0;
                    for (int iBPS = -nBPS/2; iBPS < nBPS/2+1; ++iBPS) {
                        currval += b[iNote + iBPS]; 
                    }
                    if (currval > 0) signifIndex.push_back(index);
                    index++;
                }
                float rnorm;
                float w[84+1000];
                float zz[84+1000];
                int indx[84+1000];
                int mode;
                int dictsize = nNote*signifIndex.size();
                // cerr << "dictsize is " << dictsize << "and values size" << f3.values.size()<< endl;
                float *curr_dict = new float[dictsize];
                for (int iNote = 0; iNote < (int)signifIndex.size(); ++iNote) {
                    for (int iBin = 0; iBin < nNote; iBin++) {
                        curr_dict[iNote * nNote + iBin] = 1.0 * m_dict[signifIndex[iNote] * nNote + iBin];
                    }
                }
                nnls(curr_dict, nNote, nNote, signifIndex.size(), b, x, &rnorm, w, zz, indx, &mode);
                delete [] curr_dict;
                for (int iNote = 0; iNote < (int)signifIndex.size(); ++iNote) {
                    // cerr << mode << endl;
                    chroma[signifIndex[iNote] % 12] += x[iNote] * treblewindow[signifIndex[iNote]];
                    basschroma[signifIndex[iNote] % 12] += x[iNote] * basswindow[signifIndex[iNote]];
#ifdef CHORDINO_INSPECT
                    semitones[signifIndex[iNote]] = x[iNote];
#endif
                }
            }	
        }

        vector<float> origchroma = chroma;
        chroma.insert(chroma.begin(), basschroma.begin(), basschroma.end()); // just stack the both chromas
        currentChromas.values = chroma;
        rawChromas.push_back(chroma);

        if (m_doNormalizeChroma > 0) {
            vector<float> chromanorm = vector<float>(3,0);			
            switch (int(m_doNormalizeChroma)) {
            case 0: // should never end up here
                break;
            case 1:
                chromanorm[0] = *max_element(origchroma.begin(), origchroma.end());
                chromanorm[1] = *max_element(basschroma.begin(), basschroma.end());
                chromanorm[2] = max(chromanorm[0], chromanorm[1]);
                break;
            case 2:
                for (vector<float>::iterator it = chroma.begin(); it != chroma.end(); ++it) {
                    chromanorm[2] += *it; 						
                }
                break;
            case 3:
                for (vector<float>::iterator it = chroma.begin(); it != chroma.end(); ++it) {
                    chromanorm[2] += pow(*it,2); 						
                }
                chromanorm[2] = sqrt(chromanorm[2]);
                break;
            }
            if (chromanorm[2] > 0) {
                for (int i = 0; i < (int)chroma.size(); i++) {
                    currentChromas.values[i] /= chromanorm[2];
                }
            }
        }

        chromaList.push_back(currentChromas);
#ifdef CHORDINO_INSPECT
        inspectSemitones.push_back(vector<float>(semitones, semitones + 84));
#endif
        count++;
    }

    // Smoothed copy of the chroma used only for chord matching (see chromaEmaAlpha
    // above); the raw per-frame chroma is left untouched for the exported
    // chromagram feature and for tuning/normalisation, which should reflect the
    // actual instantaneous signal.
    //
    // The integrator runs forwards and backwards and the two are averaged. A
    // forward-only one lags by (1 - alpha) / alpha frames, about 0.26 s for the
    // treble, which made every chord change come out that much late; the two
    // directions lag by the same amount the opposite way, so their average is
    // centred on the frame. Everything is buffered until here anyway, so the
    // future frames are available.
    vector<vector<double> > smoothedChromas(nFrame, vector<double>(24, 0.0));
    vector<double> runningChroma(24, 0.0);
    for (int iFrame = 0; iFrame < nFrame; iFrame++) {
        // Bins 0-11 are the bass chroma, 12-23 the treble one (stacked above).
        for (int i = 0; i < 24; i++) {
            double alpha = (i < 12) ? chromaEmaAlphaBass : chromaEmaAlpha;
            runningChroma[i] = iFrame == 0 ? rawChromas[iFrame][i]
                : alpha * rawChromas[iFrame][i] + (1 - alpha) * runningChroma[i];
            smoothedChromas[iFrame][i] = runningChroma[i] / 2;
        }
    }
    for (int iFrame = nFrame - 1; iFrame >= 0; iFrame--) {
        for (int i = 0; i < 24; i++) {
            double alpha = (i < 12) ? chromaEmaAlphaBass : chromaEmaAlpha;
            runningChroma[i] = iFrame == nFrame - 1 ? rawChromas[iFrame][i]
                : alpha * rawChromas[iFrame][i] + (1 - alpha) * runningChroma[i];
            smoothedChromas[iFrame][i] += runningChroma[i] / 2;
        }
    }

    for (int iFrame = 0; iFrame < nFrame; iFrame++) {
        const vector<double> &smoothedChroma = smoothedChromas[iFrame];

        // local chord estimation
        vector<double> currentChordSalience;
        double tempchordvalue = 0;
        double sumchordvalue = 0;
        // Scores kept as scored, before the exponential and the normalisation below
        // discard how well each candidate actually fit (see the gate further down).
        vector<double> rawChordValue(nChord, 0.0);
	        
        for (int iChord = 0; iChord < nChord; iChord++) {
            double bassMatch = 0;
            double trebleMatch = 0;
            for (int iBin = 0; iBin < 12; iBin++) {
                bassMatch += m_chorddict[24 * iChord + iBin] * smoothedChroma[iBin];
            }
            for (int iBin = 12; iBin < 24; iBin++) {
                trebleMatch += m_chorddict[24 * iChord + iBin] * smoothedChroma[iBin];
            }
            tempchordvalue = trebleMatch / trebleNorm[iChord]
                + bassWeight * bassMatch / bassNorm[iChord];
            tempchordvalue *= noteCountPenalty[iChord];
            if (iChord == nChord-1) tempchordvalue *= .7;
            if (tempchordvalue < 0) tempchordvalue = 0.0;
            if (tempchordvalue > 200.0) {
                if (!clipwarned) {
                    cerr << "WARNING: interim chroma contains extreme chord value " << tempchordvalue << ", clipping this and any others that appear" << endl;
                    clipwarned = true;
                }
                tempchordvalue = 200.0;
            }
            rawChordValue[iChord] = tempchordvalue;
            tempchordvalue = pow(1.3, tempchordvalue);
            sumchordvalue += tempchordvalue;
            currentChordSalience.push_back(tempchordvalue);
        }

        // Chords are laid out as [type][root] with 12 roots per type (see
        // chordDictionary), so a candidate's root is simply its index mod 12, and the
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
                if (m_chorddict[24 * iChord + iBin] > 0 && m_chorddict[24 * bestChord + iBin] > 0) sharedNotes++;
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
            // ViterbiPath multiplies these observations directly and the harmonic
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
        chordogram.push_back(currentChordSalience);
#ifdef CHORDINO_INSPECT
        int gate = 0;
        if (!(rawChordValue[bestChord] > minChordVsNoChord * rawChordValue[nChord-1])) gate = 1;
        else if (!confident) gate = 2;
        const Feature &frameChromas = chromaList[iFrame];
        chordinoInspectFrame(frameChromas.timestamp.sec + frameChromas.timestamp.nsec / 1e9,
                             frameChromas.values,
                             smoothedChroma, currentChordSalience, inspectSemitones[iFrame].data(), gate);
#endif
    }
    if (debug_on) cerr << "done." << endl;
		
    vector<Feature> oldnotes;

    if (debug_on) cerr << "[Chordino Plugin] HMM Chord Estimation ... ";
    double selftransprob = 0.99;
 
    // Uniform prior: forcing init[nChord-1]=1 (N) here makes every decode assume the
    // previous state was "no chord", which is only true at the very start of a session.
    // With periodic resetProcess() calls (real-time streaming), that false certainty
    // reintroduces a spurious N bias at every reset boundary, since selftransprob makes
    // it costly for the Viterbi path to leave N even when real chord evidence is strong.
    vector<double> init = vector<double>(nChord,1.0/nChord);
    
    double *delta;
    delta = (double *)malloc(sizeof(double)*nFrame*nChord);                
    
    vector<vector<double> > trans;
    for (int iChord = 0; iChord < nChord; iChord++) {
        vector<double> temp = vector<double>(nChord,(1-selftransprob)/(nChord-1));            
        temp[iChord] = selftransprob;
        trans.push_back(temp);
    }
    vector<double> scale;
    vector<int> chordpath = ViterbiPath(init, trans, chordogram, delta, &scale);
    
    auto prepareChordNotes = [&](int iFrame) {
        for (int iNote = 0; iNote < (int)m_chordnotes[chordpath[iFrame]].size(); ++iNote) { // prepare notes of current chord
            Feature chordnote_feature;
            chordnote_feature.hasTimestamp = true;
            chordnote_feature.timestamp = timestamps[iFrame];
            chordnote_feature.values.push_back(m_chordnotes[chordpath[iFrame]][iNote]);
            chordnote_feature.hasDuration = true;
            chordnote_feature.duration = -timestamps[iFrame]; // this will be corrected at the next chord
            oldnotes.push_back(chordnote_feature);
        }
    };

    Feature chord_feature; // chord estimate
    chord_feature.hasTimestamp = true;
    chord_feature.timestamp = timestamps[0];
    chord_feature.label = m_chordnames[chordpath[0]];
    fsOut[m_outputChords].push_back(chord_feature);
    // With the uniform prior the path no longer always starts on N, so track the first
    // chord as already emitted; otherwise it is reported again at the second frame.
    int oldchord = chordpath[0];
    prepareChordNotes(0);
    
    chordchange[0] = 0;
    for (int iFrame = 1; iFrame < (int)chordpath.size(); ++iFrame) {
        if (chordpath[iFrame] != oldchord ) {
            // chord
            Feature chord_feature; // chord estimate
            chord_feature.hasTimestamp = true;
            chord_feature.timestamp = timestamps[iFrame];
            chord_feature.label = m_chordnames[chordpath[iFrame]];
            fsOut[m_outputChords].push_back(chord_feature);
            oldchord = chordpath[iFrame];         
            // chord notes
            for (int iNote = 0; iNote < (int)oldnotes.size(); ++iNote) { // finish duration of old chord
                oldnotes[iNote].duration = oldnotes[iNote].duration + timestamps[iFrame];
                fsOut[m_outputChordnotes].push_back(oldnotes[iNote]);
            }
            oldnotes.clear();
            prepareChordNotes(iFrame);
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
    
    float logscale = 0;
    for (int iFrame = 0; iFrame < nFrame; ++iFrame) {
        logscale -= log(scale[iFrame]);
        Feature loglikelihood;
        loglikelihood.hasTimestamp = true;
        loglikelihood.timestamp = timestamps[iFrame];
        loglikelihood.values.push_back(-log(scale[iFrame]));
        // cerr << chordchange[iFrame] << endl;
        fsOut[m_outputLoglikelihood].push_back(loglikelihood);
    }
    logscale /= nFrame;

    chord_feature.hasTimestamp = true;
    chord_feature.timestamp = timestamps[timestamps.size()-1];
    chord_feature.label = "N";
    fsOut[m_outputChords].push_back(chord_feature);
    
    for (int iNote = 0; iNote < (int)oldnotes.size(); ++iNote) { // finish duration of old chord
        oldnotes[iNote].duration = oldnotes[iNote].duration + timestamps[timestamps.size()-1];
        fsOut[m_outputChordnotes].push_back(oldnotes[iNote]);
    }
    
    if (debug_on) cerr << "done." << endl;

    for (int iFrame = 0; iFrame < nFrame; iFrame++) {
        Feature chordchange_feature;
        chordchange_feature.hasTimestamp = true;
        chordchange_feature.timestamp = timestamps[iFrame];
        chordchange_feature.values.push_back(chordchange[iFrame]);
//        cerr << "putting value " << chordchange[iFrame] << " at time " << chordchange_feature.timestamp << endl;
        fsOut[m_outputHarmonicChange].push_back(chordchange_feature);
    }

    free(delta);
    
    // for (int iFrame = 0; iFrame < nFrame; iFrame++) cerr << fsOut[m_outputHarmonicChange][iFrame].values[0] << endl;
    
    return fsOut;     
}
