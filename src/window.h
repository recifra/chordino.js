#ifndef CHORDINO_WINDOW_H
#define CHORDINO_WINDOW_H

#ifdef __cplusplus
extern "C" {
#endif

// A Hann window over size samples, in double precision, as the Vamp SDK's
// Window<double> made it for PluginInputDomainAdapter, which used Hann unless
// told otherwise.
typedef struct {
    int size;
    double *values;
} window_t;

window_t *window_create_hann(int size);
void window_destroy(window_t *window);
// Multiplies the size samples at src by the window, into dst
void window_cut(const window_t *window, const float *src, double *dst);

#ifdef __cplusplus
}
#endif

#endif
