#include "realtime.h"

#include <limits.h>

// The Vamp SDK's RealTime.cpp, step for step: the frame times depend on its
// rounding. nsec may reach twice a billion in between (realtime_add), which
// still fits an int.
#define ONE_BILLION 1000000000

realtime_t realtime_make(int sec, int nsec) {
    while (nsec <= -ONE_BILLION && sec > INT_MIN) { nsec += ONE_BILLION; --sec; }
    while (nsec >=  ONE_BILLION && sec < INT_MAX) { nsec -= ONE_BILLION; ++sec; }
    while (nsec > 0 && sec < 0) { nsec -= ONE_BILLION; ++sec; }
    while (nsec < 0 && sec > 0) { nsec += ONE_BILLION; --sec; }
    realtime_t time = { sec, nsec };
    return time;
}

realtime_t realtime_from_seconds(double seconds) {
    if (seconds != seconds) {
        return realtime_make(0, 0);
    } else if (seconds >= 0) {
        return realtime_make((int) seconds, (int) ((seconds - (int) seconds) * ONE_BILLION + 0.5));
    } else {
        return realtime_negate(realtime_from_seconds(-seconds));
    }
}

realtime_t realtime_from_milliseconds(int milliseconds) {
    return realtime_make(milliseconds / 1000, (milliseconds % 1000) * 1000000);
}

double realtime_to_seconds(realtime_t time) {
    return time.sec + time.nsec / 1e9;
}

realtime_t realtime_add(realtime_t a, realtime_t b) {
    return realtime_make(a.sec + b.sec, a.nsec + b.nsec);
}

realtime_t realtime_negate(realtime_t time) {
    return realtime_make(-time.sec, -time.nsec);
}

int realtime_compare(realtime_t a, realtime_t b) {
    if (a.sec != b.sec) return a.sec < b.sec ? -1 : 1;
    return a.nsec < b.nsec ? -1 : a.nsec > b.nsec ? 1 : 0;
}

long realtime_to_frame(realtime_t time, unsigned int sampleRate) {
    if (time.sec < 0 || (time.sec == 0 && time.nsec < 0)) {
        return -realtime_to_frame(realtime_negate(time), sampleRate);
    }
    double seconds = time.sec + (double) time.nsec / ONE_BILLION;
    return (long) (seconds * sampleRate + 0.5);
}

realtime_t realtime_from_frame(long frame, unsigned int sampleRate) {
    if (frame < 0) {
        return realtime_negate(realtime_from_frame(-frame, sampleRate));
    }
    int sec = (int) (frame / (long) sampleRate);
    frame -= sec * (long) sampleRate;
    int nsec = (int) (((double) frame / (double) sampleRate) * ONE_BILLION + 0.5);
    return realtime_make(sec, nsec);
}
