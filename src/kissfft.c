// KISS FFT 131.2.0 (src/kissfft/, BSD-3-Clause), compiled from here so that
// NDEBUG applies to it alone: it keeps kiss_fft_log.h's error messages, and
// the fprintf behind them, out of the wasm. build.sh sets kiss_fft_scalar to
// double for every file, as the Vamp SDK's copy had it.
#define NDEBUG
#include "kissfft/kiss_fft.c"
#include "kissfft/kiss_fftr.c"
