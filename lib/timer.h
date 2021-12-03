#ifndef TIMER_H_
#define TIMER_H_

#include "macro.h"

struct timer_ctx
{
#if defined(OS_WIN)
    double interval;
#elif defined(OS_DARWIN) 
    double interval;
    uint64_t(*timefunc)(void);
#else
#endif
    uint64_t starttick;
};
/*
* \brief          初始化
*/
static inline void timer_init(struct timer_ctx *pctx)
{
#if defined(OS_WIN)
    pctx->interval = 0;
    LARGE_INTEGER freq;
    ASSERTAB(QueryPerformanceFrequency(&freq), ERRORSTR(ERRNO));
    pctx->interval = 1.0 / freq.QuadPart;
#elif defined(OS_DARWIN) 
    mach_timebase_info_data_t timebase;
    ASSERTAB(KERN_SUCCESS == mach_timebase_info(&timebase), "mach_timebase_info error.");
    pctx->interval = (double)timebase.numer / (double)timebase.denom;
    pctx->timefunc = (uint64_t(*)(void)) dlsym(RTLD_DEFAULT, "mach_continuous_time");
    if (NULL == pctx->timefunc)
    {
        pctx->timefunc = mach_absolute_time;
    }
#else
#endif
}
/*
* \brief          当前时间
*/
static inline uint64_t timer_nanosec(struct timer_ctx *pctx)
{
#if defined(OS_WIN)
    LARGE_INTEGER lnow;
    ASSERTAB(QueryPerformanceCounter(&lnow), ERRORSTR(ERRNO));
    return (uint64_t)(lnow.QuadPart * pctx->interval * NANOSEC);
#elif defined(OS_AIX)
    timebasestruct_t t;
    read_wall_time(&t, TIMEBASE_SZ);
    time_base_to_time(&t, TIMEBASE_SZ);
    return (((uint64_t)t.tb_high) * NANOSEC + t.tb_low);
#elif defined(OS_SUN)
    return gethrtime();
#elif defined(OS_DARWIN)
    return (uint64_t)(pctx->timefunc() * pctx->interval);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (((uint64_t)ts.tv_sec) * NANOSEC + ts.tv_nsec);
#endif
};
/*
* \brief          开始计时
*/
static inline void timer_start(struct timer_ctx *pctx)
{
    pctx->starttick = timer_nanosec(pctx);
};
/*
* \brief          结束计时
* \return         用时 纳秒
*/
static inline uint64_t timer_elapsed(struct timer_ctx *pctx)
{
    return timer_nanosec(pctx) - pctx->starttick;
};

#endif//TIMER_H_
