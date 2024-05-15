#ifndef TIMER_H_
#define TIMER_H_

#include "macro.h"

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


void timer_init(timer_ctx *ctx);
uint64_t timer_cur(timer_ctx *ctx);
uint64_t timer_cur_ms(timer_ctx *ctx);
void timer_start(timer_ctx *ctx);
uint64_t timer_elapsed(timer_ctx *ctx);
uint64_t timer_elapsed_ms(timer_ctx *ctx);

#endif//TIMER_H_
