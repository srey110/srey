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
void timer_init(struct timer_ctx *pctx);
/*
* \brief          当前时间
*/
uint64_t timer_cur(struct timer_ctx *pctx);
/*
* \brief          开始计时
*/
void timer_start(struct timer_ctx *pctx);
/*
* \brief          结束计时
* \return         用时 纳秒
*/
uint64_t timer_elapsed(struct timer_ctx *pctx);

#endif//TIMER_H_
