// What Chordino, which still takes and gives Vamp::RealTime, needs of it beyond
// its header, over realtime.c: in place of the Vamp SDK's RealTime.cpp, so that
// no Vamp SDK source is compiled, only its plugin headers.

#include <vamp-sdk/RealTime.h>

#include "realtime.h"

_VAMP_SDK_PLUGSPACE_BEGIN(vamp-realtime.cpp)

namespace Vamp {

RealTime::RealTime(int s, int n) {
    realtime_t time = realtime_make(s, n);
    sec = time.sec;
    nsec = time.nsec;
}

RealTime RealTime::fromSeconds(double seconds) {
    realtime_t time = realtime_from_seconds(seconds);
    return RealTime(time.sec, time.nsec);
}

}

_VAMP_SDK_PLUGSPACE_END(vamp-realtime.cpp)
