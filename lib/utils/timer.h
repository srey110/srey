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
/// 初始化计时器，并把计时起点置为当前时刻；
/// 因此 timer_init 之后可直接调 timer_elapsed，无须先调 timer_start
/// </summary>
/// <param name="ctx">timer_ctx</param>
void timer_init(timer_ctx *ctx);
/// <summary>
/// 当前时刻。单调 wall clock（区别于 timer_thread_cpu_ns 的 CPU 时间）；
/// 无 CLOCK_MONOTONIC 的平台退回 CLOCK_REALTIME，此时会随 NTP 跳变而非单调，
/// 时间轮 jiffies 与各处超时判定的时基精度随之下降——但仍是墙钟，不会像
/// 进程 CPU 时间那样在空闲时几乎停止推进
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
/// 把计时起点重置为当前时刻
/// </summary>
/// <param name="ctx">timer_ctx</param>
void timer_start(timer_ctx *ctx);
/// <summary>
/// 距计时起点（timer_init 或最近一次 timer_start）的耗时
/// </summary>
/// <param name="ctx">timer_ctx</param>
/// <returns>纳秒</returns>
uint64_t timer_elapsed(timer_ctx *ctx);
/// <summary>
/// 距计时起点（timer_init 或最近一次 timer_start）的耗时
/// </summary>
/// <param name="ctx">timer_ctx</param>
/// <returns>毫秒</returns>
uint64_t timer_elapsed_ms(timer_ctx *ctx);

#endif//TIMER_H_
