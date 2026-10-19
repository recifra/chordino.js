#include "pitch.h"

#include <math.h>
#include <stdlib.h>

// The normalized difference a lag has to go under to be the period, as in
// app-recifra
static const double threshold = 0.1;

pitch_t *pitch_create(float sampleRate, int size) {
    // kiss_fftr takes even sizes only
    if (size < 2 || size % 2 != 0) return NULL;
    pitch_t *pitch = malloc(sizeof(pitch_t));
    pitch->sampleRate = sampleRate;
    pitch->size = size;
    pitch->fft = kiss_fftr_alloc(size, 0, NULL, NULL);
    pitch->inverse = kiss_fftr_alloc(size, 1, NULL, NULL);
    pitch->samples = malloc(size * sizeof(double));
    pitch->first = malloc((size / 2 + 1) * sizeof(kiss_fft_cpx));
    pitch->whole = malloc((size / 2 + 1) * sizeof(kiss_fft_cpx));
    pitch->difference = malloc(size / 2 * sizeof(double));
    return pitch;
}

void pitch_destroy(pitch_t *pitch) {
    kiss_fftr_free(pitch->fft);
    kiss_fftr_free(pitch->inverse);
    free(pitch->samples);
    free(pitch->first);
    free(pitch->whole);
    free(pitch->difference);
    free(pitch);
}

double pitch_rms(const pitch_t *pitch, const float *block) {
    double sum = 0;
    for (int i = 0; i < pitch->size; ++i) {
        sum += (double) block[i] * block[i];
    }
    return sqrt(sum / pitch->size);
}

// YIN's difference function, d(tau) = the sum over the first half of
// (x[i] - x[i + tau])², for every lag under size / 2 at once, as
// e(0) + e(tau) - 2 r(tau): the energies of the first half and of the half
// that starts at tau, less twice their cross-correlation, which the FFT gives.
static void difference(pitch_t *pitch, const float *block) {
    int size = pitch->size;
    int half = size / 2;
    double *samples = pitch->samples;
    // The first half alone, zero-padded, against the whole window: with tau
    // under half, i + tau stays inside it, so the circular correlation never
    // wraps
    for (int i = 0; i < size; ++i) {
        samples[i] = i < half ? block[i] : 0;
    }
    kiss_fftr(pitch->fft, samples, pitch->first);
    for (int i = 0; i < size; ++i) {
        samples[i] = block[i];
    }
    kiss_fftr(pitch->fft, samples, pitch->whole);
    // conj(first) * whole
    for (int k = 0; k <= half; ++k) {
        kiss_fft_cpx a = pitch->first[k];
        kiss_fft_cpx b = pitch->whole[k];
        pitch->first[k].r = a.r * b.r + a.i * b.i;
        pitch->first[k].i = a.r * b.i - a.i * b.r;
    }
    // Unscaled: size times r(tau)
    kiss_fftri(pitch->inverse, pitch->first, samples);

    double energyFirst = 0;
    for (int i = 0; i < half; ++i) {
        energyFirst += (double) block[i] * block[i];
    }
    // The energy of the half that starts at tau, slid along
    double energyShifted = energyFirst;
    double *d = pitch->difference;
    d[0] = 0;
    for (int tau = 1; tau < half; ++tau) {
        double leaving = block[tau - 1];
        double entering = block[tau + half - 1];
        energyShifted += entering * entering - leaving * leaving;
        // Rounding can take it just under 0
        d[tau] = fmax(0, energyFirst + energyShifted - 2 * samples[tau] / size);
    }
}

double pitch_detect(pitch_t *pitch, const float *block) {
    int half = pitch->size / 2;
    double *d = pitch->difference;
    difference(pitch, block);
    // Cumulative mean normalized difference, in place: d(tau) over the mean of
    // d(1) to d(tau), which makes d(1) 1
    d[0] = 1;
    double runningSum = 0;
    for (int tau = 1; tau < half; ++tau) {
        runningSum += d[tau];
        d[tau] = runningSum > 0 ? d[tau] * tau / runningSum : 1;
    }
    // The first dip under the threshold, followed down to its bottom
    int tau = 2;
    while (tau < half && d[tau] >= threshold) ++tau;
    if (tau == half) return 0;
    while (tau + 1 < half && d[tau + 1] < d[tau]) ++tau;
    if (tau + 1 == half) return pitch->sampleRate / tau;
    // The parabola through the bottom and its neighbours. The lag before is
    // higher, as the dip came down from it, and the one after no lower, so
    // the parabola opens upwards and its vertex is within half a lag.
    double s0 = d[tau - 1];
    double s1 = d[tau];
    double s2 = d[tau + 1];
    return pitch->sampleRate / (tau + (s2 - s0) / (2 * (2 * s1 - s2 - s0)));
}
