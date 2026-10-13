#include "spectrum.h"

#include <stdlib.h>

spectrum_t *spectrum_create(int size) {
    spectrum_t *spectrum = malloc(sizeof(spectrum_t));
    spectrum->size = size;
    spectrum->window = window_create_hann(size);
    spectrum->windowed = malloc(size * sizeof(double));
    spectrum->fft = kiss_fftr_alloc(size, 0, NULL, NULL);
    spectrum->complex = malloc((size / 2 + 1) * sizeof(kiss_fft_cpx));
    spectrum->bins = malloc((size + 2) * sizeof(float));
    return spectrum;
}

void spectrum_destroy(spectrum_t *spectrum) {
    window_destroy(spectrum->window);
    free(spectrum->windowed);
    kiss_fftr_free(spectrum->fft);
    free(spectrum->complex);
    free(spectrum->bins);
    free(spectrum);
}

const float *spectrum_compute(spectrum_t *spectrum, const float *block) {
    int half = spectrum->size / 2;
    double *windowed = spectrum->windowed;
    window_cut(spectrum->window, block, windowed);
    // Centred on the start of the FFT, so the phases are those of the middle
    for (int i = 0; i < half; ++i) {
        double value = windowed[i];
        windowed[i] = windowed[i + half];
        windowed[i + half] = value;
    }
    kiss_fftr(spectrum->fft, windowed, spectrum->complex);
    for (int i = 0; i <= half; ++i) {
        spectrum->bins[i * 2] = (float) spectrum->complex[i].r;
        spectrum->bins[i * 2 + 1] = (float) spectrum->complex[i].i;
    }
    return spectrum->bins;
}
