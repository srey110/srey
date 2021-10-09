#include "timer.h"

SREY_NS_BEGIN

ctimer::ctimer() 
{
#ifdef OS_WIN
    interval = 0;
    LARGE_INTEGER freq;
    ASSERTAB(QueryPerformanceFrequency(&freq), ERRORSTR(ERRNO));
    interval = 1.0 / freq.QuadPart;
#endif
}
uint64_t ctimer::nanosec()
{
#ifdef OS_WIN
    LARGE_INTEGER lnow;
    ASSERTAB(QueryPerformanceCounter(&lnow), ERRORSTR(ERRNO));
    return (uint64_t)(lnow.QuadPart * interval * NANOSEC);
#elif defined(OS_AIX)
    timebasestruct_t t;
    read_wall_time(&t, TIMEBASE_SZ);
    time_base_to_time(&t, TIMEBASE_SZ);
    return (((uint64_t)t.tb_high) * NANOSEC + t.tb_low);
#elif defined(OS_SOLARIS)
    return gethrtime();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (((uint64_t)ts.tv_sec) * NANOSEC + ts.tv_nsec);
#endif
}
void ctimer::start()
{
    starttick = nanosec();
}
uint64_t ctimer::elapsed()
{
    return nanosec() - starttick;
}

SREY_NS_END
