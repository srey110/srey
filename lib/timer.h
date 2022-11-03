#ifndef TIMER_H_
#define TIMER_H_

#include "macro.h"

typedef struct timer_ctx
{
#if defined(OS_WIN)
    double interval;
#elif defined(OS_DARWIN) 
    double interval;
    uint64_t(*timefunc)(void);
#else
#endif
    uint64_t starttick;
}timer_ctx;

/*
* \brief          初始化
*/
void timer_init(timer_ctx *ctx);
/*
* \brief          当前时间
*/
uint64_t timer_cur(timer_ctx *ctx);
/*
* \brief          开始计时
*/
void timer_start(timer_ctx *ctx);
/*
* \brief          结束计时
* \return         用时 纳秒
*/
uint64_t timer_elapsed(timer_ctx *ctx);

#endif//TIMER_H_
