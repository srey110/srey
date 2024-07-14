#ifndef TIMER_H_
#define TIMER_H_

#include "base/macro.h"

typedef struct timer_ctx {
#if defined(OS_WIN)
    double interval;
#elif defined(OS_DARWIN) 
    double interval;
    uint64_t(*timefunc)(void);
#else
#endif
    uint64_t starttick;
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
