#ifndef CHORDINO_PITCH_H
#define CHORDINO_PITCH_H

#include "kissfft/kiss_fftr.h"

#ifdef __cplusplus
extern "C" {
#endif

// YIN over windows of size samples, the tuner's detector: the fundamental of
// one window, with nothing kept from one call to the next. The difference
// function comes from a cross-correlation through KISS FFT, as app-recifra's
// pitch-detector.ts worked it out, instead of the direct sum (size² / 4 steps
// a window). Lags go up to size / 2, so 4096 samples reach A0 at 48 kHz.
typedef struct {
    float sampleRate;
    int size;
    kiss_fftr_cfg fft;
    kiss_fftr_cfg inverse;
    double *samples;        // FFT input, then the cross-correlation
    kiss_fft_cpx *first;    // spectra of the first half, zero-padded,
    kiss_fft_cpx *whole;    // and of the whole window
    double *difference;     // per lag, then normalized; size / 2 of them
} pitch_t;

// NULL unless size is even
pitch_t *pitch_create(float sampleRate, int size);
void pitch_destroy(pitch_t *pitch);
// RMS of the size samples at block
double pitch_rms(const pitch_t *pitch, const float *block);
// Fundamental of the size samples at block, in Hz; 0 when YIN finds none
double pitch_detect(pitch_t *pitch, const float *block);

#ifdef __cplusplus
}
#endif

#endif
