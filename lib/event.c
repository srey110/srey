#include "event.h"
#include "timer.h"

struct event_ctx
{
    volatile int32_t stop;//停止标志
    u_long accuracy;//计时器精度
    struct thread_ctx thtw;
    struct timer_ctx timer;//计时器
    struct tw_ctx tw;//时间轮
    struct netev_ctx *netev;//网络
};

static inline u_long _cur_tick(struct event_ctx *pctx)
{
    return (u_long)(timer_nanosec(&pctx->timer) / pctx->accuracy);
}
struct event_ctx *event_new()
{
    struct event_ctx *pctx = MALLOC(sizeof(struct event_ctx));
    if (NULL == pctx)
    {
        LOG_ERROR("%s", ERRSTR_MEMORY);
        return NULL;
    }

    srand((uint32_t)time(NULL));
    pctx->stop = 0;
    pctx->accuracy = 1000 * 1000;
    thread_init(&pctx->thtw);
    timer_init(&pctx->timer);
    tw_init(&pctx->tw, _cur_tick(pctx));
    pctx->netev = netev_new(&pctx->tw, 0);
    return pctx;
}
void event_free(struct event_ctx *pctx)
{
    netev_free(pctx->netev);
    pctx->stop = 1;
    thread_join(&pctx->thtw);
    tw_free(&pctx->tw);
    FREE(pctx);
}
static void _tw_loop(void *pparam)
{
    struct event_ctx *pctx = (struct event_ctx *)pparam;
    while (0 == pctx->stop)
    {
        tw_run(&pctx->tw, _cur_tick(pctx));
        USLEEP(10);
    }
}
void event_loop(struct event_ctx *pctx)
{
    thread_creat(&pctx->thtw, _tw_loop, pctx);
    thread_waitstart(&pctx->thtw);
    netev_loop(pctx->netev);
}
struct netev_ctx *event_netev(struct event_ctx *pctx)
{
    return pctx->netev;
}
struct tw_ctx *event_tw(struct event_ctx *pctx)
{
    return &pctx->tw;
}
