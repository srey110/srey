#ifndef TIMER_H_
#define TIMER_H_

#include "macro.h"

struct timer_ctx
{
#if defined(OS_WIN)
    double interval;
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
#elif defined(OS_SUN)
    return gethrtime();
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
* \return         用时 微秒
*/
static inline uint64_t timer_elapsed(struct timer_ctx *pctx)
{
    return timer_nanosec(pctx) - pctx->starttick;
};

#endif//TIMER_H_
