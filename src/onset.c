#include "onset.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// Run once a recording, so sized down rather than unrolled by -O3
#define ONCE __attribute__((minsize))

// The bands: 12 an octave from lowHz up to highHz (or half the sample rate),
// as many of them as the bins tell apart
static const double bandsPerOctave = 12;
static const double lowHz = 30;
static const double highHz = 17000;

// Triangular bands between log-spaced centres, each over the bins between the
// centres next to it, weighing 1 in all: madmom's LogarithmicFilterbank with
// its filters normalized and the duplicates removed
ONCE static void make_bands(onset_t *onset, float sampleRate) {
    int bins = onset->size / 2 + 1;
    double binHz = sampleRate / onset->size;
    double top = fmin(highHz, sampleRate / 2);
    int count = (int) floor(log(top / lowHz) / M_LN2 * bandsPerOctave) + 1;
    int *centres = malloc(count * sizeof(int));
    int unique = 0;
    for (int i = 0; i < count; ++i) {
        int bin = (int) floor(lowHz * pow(2, i / bandsPerOctave) / binHz + 0.5);
        if (bin >= bins) break;
        if (unique == 0 || bin > centres[unique - 1]) centres[unique++] = bin;
    }
    // The first and last centres only bound their neighbours
    onset->bandCount = unique > 2 ? unique - 2 : 0;
    onset->firstBin = malloc((onset->bandCount + 1) * sizeof(int));
    onset->firstWeight = malloc((onset->bandCount + 1) * sizeof(int));
    onset->weights = malloc(bins * 2 * sizeof(float));
    onset->bassBands = 0;
    int weight = 0;
    for (int band = 0; band < onset->bandCount; ++band) {
        int start = centres[band], centre = centres[band + 1], stop = centres[band + 2];
        onset->firstBin[band] = start + 1;
        onset->firstWeight[band] = weight;
        float *weights = onset->weights + weight;
        double sum = 0;
        for (int bin = start + 1; bin < stop; ++bin) {
            double value = bin <= centre
                ? (double) (bin - start) / (centre - start)
                : (double) (stop - bin) / (stop - centre);
            weights[bin - start - 1] = (float) value;
            sum += value;
        }
        for (int bin = start + 1; bin < stop; ++bin) {
            weights[bin - start - 1] /= sum;
        }
        weight += stop - start - 1;
        if (centre * binHz < ONSET_BASS_HZ) onset->bassBands = band + 1;
    }
    onset->firstWeight[onset->bandCount] = weight;
    free(centres);
}

ONCE onset_t *onset_create(float sampleRate) {
    onset_t *onset = malloc(sizeof(onset_t));
    onset->hop = (int) fmax(1, floor(sampleRate / ONSET_RATE + 0.5));
    onset->period = onset->hop / sampleRate;
    // About 46 ms: 2048 samples at 44.1 and 48 kHz
    onset->size = 1 << (int) fmax(8, floor(log(sampleRate * 0.0464) / M_LN2 + 0.5));
    onset->spectrum = spectrum_create(onset->size);
    make_bands(onset, sampleRate);
    onset->previous = malloc(onset->bandCount * sizeof(float));
    onset->current = malloc(onset->bandCount * sizeof(float));
    onset->pending = malloc(onset->size * sizeof(float));
    onset->strength = vector_create(sizeof(float));
    onset->bass = vector_create(sizeof(float));
    onset_reset(onset);
    return onset;
}

void onset_destroy(onset_t *onset) {
    spectrum_destroy(onset->spectrum);
    free(onset->firstBin);
    free(onset->firstWeight);
    free(onset->weights);
    free(onset->previous);
    free(onset->current);
    free(onset->pending);
    vector_destroy(&onset->strength);
    vector_destroy(&onset->bass);
    free(onset);
}

void onset_reset(onset_t *onset) {
    // Half a window of silence before the first sample, for the first frame
    // to be centred on it
    memset(onset->pending, 0, onset->size * sizeof(float));
    onset->pendingCount = onset->size / 2;
    onset->fed = 0;
    onset->started = false;
    vector_clear(&onset->strength);
    vector_clear(&onset->bass);
}

// Adds the frame of the window in pending and steps past hop samples of it
static void step(onset_t *onset) {
    const float *bins = spectrum_compute(onset->spectrum, onset->pending);
    float *current = onset->current, *previous = onset->previous;
    for (int band = 0; band < onset->bandCount; ++band) {
        const float *weights = onset->weights + onset->firstWeight[band];
        int count = onset->firstWeight[band + 1] - onset->firstWeight[band];
        const float *bin = bins + onset->firstBin[band] * 2;
        double sum = 0;
        for (int i = 0; i < count; ++i) {
            sum += weights[i] * sqrt((double) bin[i * 2] * bin[i * 2] + (double) bin[i * 2 + 1] * bin[i * 2 + 1]);
        }
        current[band] = (float) log10(1 + sum);
    }
    double strength = 0, bass = 0;
    if (onset->started) {
        int last = onset->bandCount - 1;
        for (int band = 0; band <= last; ++band) {
            float before = previous[band];
            if (band > 0 && previous[band - 1] > before) before = previous[band - 1];
            if (band < last && previous[band + 1] > before) before = previous[band + 1];
            float rise = current[band] - before;
            if (rise <= 0) continue;
            strength += rise;
            if (band < onset->bassBands) bass += rise;
        }
    }
    *(float *) vector_push(&onset->strength, NULL, 1) = (float) strength;
    *(float *) vector_push(&onset->bass, NULL, 1) = (float) bass;
    onset->previous = current;
    onset->current = previous;
    onset->started = true;

    onset->pendingCount -= onset->hop;
    memmove(onset->pending, onset->pending + onset->hop, onset->pendingCount * sizeof(float));
}

void onset_process(onset_t *onset, const float *samples, int count) {
    onset->fed += count;
    while (count > 0) {
        int take = onset->size - onset->pendingCount;
        if (take > count) take = count;
        memcpy(onset->pending + onset->pendingCount, samples, take * sizeof(float));
        onset->pendingCount += take;
        samples += take;
        count -= take;
        if (onset->pendingCount == onset->size) step(onset);
    }
}

void onset_finish(onset_t *onset) {
    // Up to the frame centred on the last sample
    long frames = (onset->fed + onset->hop - 1) / onset->hop;
    while (onset->strength.count < frames) {
        memset(onset->pending + onset->pendingCount, 0, (onset->size - onset->pendingCount) * sizeof(float));
        onset->pendingCount = onset->size;
        step(onset);
    }
}
