#ifndef CHORDINO_REALTIME_H
#define CHORDINO_REALTIME_H

#ifdef __cplusplus
extern "C" {
#endif

// A time in whole seconds and nanoseconds, with the arithmetic of the Vamp
// SDK's RealTime, so that frames are timed exactly as Chordino always had them:
// both parts carry the sign, and nsec stays within a second.
typedef struct {
    int sec;
    int nsec;
} realtime_t;

// sec and nsec as given, normalised: nsec within a second, with the sign of sec
realtime_t realtime_make(int sec, int nsec);
// Rounded to the nanosecond; NaN gives 0
realtime_t realtime_from_seconds(double seconds);
realtime_t realtime_from_milliseconds(int milliseconds);
double realtime_to_seconds(realtime_t time);
realtime_t realtime_add(realtime_t a, realtime_t b);
realtime_t realtime_negate(realtime_t time);
// The frame at time, rounded to the nearest one
long realtime_to_frame(realtime_t time, unsigned int sampleRate);
realtime_t realtime_from_frame(long frame, unsigned int sampleRate);

#ifdef __cplusplus
}
#endif

#endif
