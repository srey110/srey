#include "timer.h"

#define NANOSEC 1000000000

void timer_init(timer_ctx *ctx)
{
#if defined(OS_WIN)
    ctx->interval = 0;
    LARGE_INTEGER freq;
    ASSERTAB(QueryPerformanceFrequency(&freq), ERRORSTR(ERRNO));
    ctx->interval = 1.0 / freq.QuadPart;
#elif defined(OS_DARWIN) 
    mach_timebase_info_data_t timebase;
    ASSERTAB(KERN_SUCCESS == mach_timebase_info(&timebase), "mach_timebase_info error.");
    ctx->interval = (double)timebase.numer / (double)timebase.denom;
    ctx->timefunc = (uint64_t(*)(void)) dlsym(RTLD_DEFAULT, "mach_continuous_time");
    if (NULL == ctx->timefunc)
    {
        ctx->timefunc = mach_absolute_time;
    }
#else
#endif
}
uint64_t timer_cur(timer_ctx *ctx)
{
#if defined(OS_WIN)
    LARGE_INTEGER now;
    ASSERTAB(QueryPerformanceCounter(&now), ERRORSTR(ERRNO));
    return (uint64_t)(now.QuadPart * ctx->interval * NANOSEC);
#elif defined(OS_AIX)
    timebasestruct_t t;
    read_wall_time(&t, TIMEBASE_SZ);
    time_base_to_time(&t, TIMEBASE_SZ);
    return (((uint64_t)t.tb_high) * NANOSEC + t.tb_low);
#elif defined(OS_SUN)
    return gethrtime();
#elif defined(OS_DARWIN)
    return (uint64_t)(ctx->timefunc() * ctx->interval);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (((uint64_t)ts.tv_sec) * NANOSEC + ts.tv_nsec);
#endif
}
void timer_start(timer_ctx *ctx)
{
    ctx->starttick = timer_cur(ctx);
}
uint64_t timer_elapsed(timer_ctx *ctx)
{
    return timer_cur(ctx) - ctx->starttick;
}
uint64_t timer_elapsed_ms(timer_ctx *ctx)
{
    return (timer_cur(ctx) - ctx->starttick) / TIMER_ACCURACY;
}
