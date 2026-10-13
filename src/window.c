#include "window.h"

#include <math.h>
#include <stdlib.h>

window_t *window_create_hann(int size) {
    window_t *window = malloc(sizeof(window_t));
    window->size = size;
    window->values = malloc(size * sizeof(double));
    // Vamp's cosine window with a0 = a1 = 0.5 and no higher terms: adding and
    // subtracting those zero terms changed no bit of these
    for (int i = 0; i < size; ++i) {
        window->values[i] = 0.5 - 0.5 * cos((2 * M_PI * i) / size);
    }
    return window;
}

void window_destroy(window_t *window) {
    free(window->values);
    free(window);
}

void window_cut(const window_t *window, const float *src, double *dst) {
    for (int i = 0; i < window->size; ++i) {
        dst[i] = src[i] * window->values[i];
    }
}
