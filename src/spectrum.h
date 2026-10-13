#ifndef CHORDINO_SPECTRUM_H
#define CHORDINO_SPECTRUM_H

#include "kissfft/kiss_fftr.h"
#include "window.h"

#ifdef __cplusplus
extern "C" {
#endif

// The spectra a frequency-domain Vamp plugin takes, made as the Vamp SDK's
// PluginInputDomainAdapter made them: a block of size samples, Hann windowed,
// through a real FFT in double precision.
typedef struct {
    int size;
    window_t *window;
    double *windowed;
    kiss_fftr_cfg fft;
    kiss_fft_cpx *complex;
    float *bins;
} spectrum_t;

spectrum_t *spectrum_create(int size);
void spectrum_destroy(spectrum_t *spectrum);
// The size / 2 + 1 bins of the size samples at block, as floats: re, im, re, ...
// Valid until the next call.
const float *spectrum_compute(spectrum_t *spectrum, const float *block);

#ifdef __cplusplus
}
#endif

#endif
