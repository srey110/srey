#ifndef TIMER_H_
#define TIMER_H_

#include "base/macro.h"

typedef struct timer_ctx {
#if defined(OS_WIN)
    uint64_t freq;                //QueryPerformanceFrequency 计数频率（Hz）
#elif defined(OS_DARWIN)
    uint32_t numer;               //mach_timebase 分子（nanoseconds = ticks * numer / denom）
    uint32_t denom;               //mach_timebase 分母
    uint64_t(*timefunc)(void);    //实际使用的时间函数（优先 mach_continuous_time）
#else
#endif
    uint64_t starttick;           //计时起始时刻（纳秒）
}timer_ctx;
/// <summary>
/// 初始化计时器
/// </summary>
/// <param name="ctx">timer_ctx</param>
void timer_init(timer_ctx *ctx);
/// <summary>
/// 当前时刻
/// </summary>
/// <param name="ctx">timer_ctx</param>
/// <returns>纳秒</returns>
uint64_t timer_cur(timer_ctx *ctx);
/// <summary>
/// 当前时刻
/// </summary>
/// <param name="ctx">timer_ctx</param>
/// <returns>毫秒</returns>
uint64_t timer_cur_ms(timer_ctx *ctx);
/// <summary>
/// 当前线程累计 CPU 时间（用户态 + 内核态），不含 IO 等待 / 被抢占；
/// 用于 dispatch 占用统计，区别于 timer_cur 的单调 wall clock。
/// </summary>
/// <returns>纳秒</returns>
uint64_t timer_thread_cpu_ns(void);
/// <summary>
/// 开始计时
/// </summary>
/// <param name="ctx">timer_ctx</param>
void timer_start(timer_ctx *ctx);
/// <summary>
/// 耗时
/// </summary>
/// <param name="ctx">timer_ctx</param>
/// <returns>纳秒</returns>
uint64_t timer_elapsed(timer_ctx *ctx);
/// <summary>
/// 耗时
/// </summary>
/// <param name="ctx">timer_ctx</param>
/// <returns>毫秒</returns>
uint64_t timer_elapsed_ms(timer_ctx *ctx);

#endif//TIMER_H_
