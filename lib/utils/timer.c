#include "utils/timer.h"

#define NANOSEC 1000000000

void timer_init(timer_ctx *ctx) {
#if defined(OS_WIN)
    ctx->freq = 0;
    LARGE_INTEGER freq;
    ASSERTAB(QueryPerformanceFrequency(&freq), ERRORSTR(ERRNO));
    ctx->freq = (uint64_t)freq.QuadPart;
#elif defined(OS_DARWIN)
    mach_timebase_info_data_t timebase;
    ASSERTAB(KERN_SUCCESS == mach_timebase_info(&timebase), ERRORSTR(ERRNO));
    ctx->numer = timebase.numer;
    ctx->denom = timebase.denom;
    ctx->timefunc = (uint64_t(*)(void)) dlsym(RTLD_DEFAULT, "mach_continuous_time");
    if (NULL == ctx->timefunc) {
        ctx->timefunc = mach_absolute_time;
    }
#else
    (void)ctx;
#endif
}
uint64_t timer_cur(timer_ctx *ctx) {
#if defined(OS_WIN)
    LARGE_INTEGER now;
    ASSERTAB(QueryPerformanceCounter(&now), ERRORSTR(ERRNO));
    uint64_t ticks = (uint64_t)now.QuadPart;
    return (ticks / ctx->freq) * NANOSEC + (ticks % ctx->freq) * NANOSEC / ctx->freq;
#elif defined(OS_AIX)
    (void)ctx;
    timebasestruct_t t;
    read_wall_time(&t, TIMEBASE_SZ);
    time_base_to_time(&t, TIMEBASE_SZ);
    return (((uint64_t)t.tb_high) * NANOSEC + t.tb_low);
#elif defined(OS_SUN)
    (void)ctx;
    return gethrtime();
#elif defined(OS_DARWIN)
    uint64_t ticks = ctx->timefunc();
    return (ticks / ctx->denom) * ctx->numer + (ticks % ctx->denom) * ctx->numer / ctx->denom;
#else
    (void)ctx;
    struct timespec ts;
#if defined(OS_HPUX)
    clock_gettime(CLOCK_VIRTUAL, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (((uint64_t)ts.tv_sec) * NANOSEC + ts.tv_nsec);
#endif
}
uint64_t timer_cur_ms(timer_ctx *ctx) {
    return timer_cur(ctx) / TIMER_ACCURACY;
}
uint64_t timer_thread_cpu_ns(void) {
#if defined(OS_WIN)
    FILETIME create, exit, kernel, user;
    GetThreadTimes(GetCurrentThread(), &create, &exit, &kernel, &user);
    uint64_t k = ((uint64_t)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime;
    uint64_t u = ((uint64_t)user.dwHighDateTime << 32) | user.dwLowDateTime;
    return (k + u) * 100;
#else
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (((uint64_t)ts.tv_sec) * NANOSEC + ts.tv_nsec);
#endif
}
void timer_start(timer_ctx *ctx) {
    ctx->starttick = timer_cur(ctx);
}
uint64_t timer_elapsed(timer_ctx *ctx) {
    return timer_cur(ctx) - ctx->starttick;
}
uint64_t timer_elapsed_ms(timer_ctx *ctx) {
    return (timer_cur(ctx) - ctx->starttick) / TIMER_ACCURACY;
}
