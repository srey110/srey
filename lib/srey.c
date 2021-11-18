#include "srey.h"
#include "timer.h"

struct srey_ctx
{
    volatile int32_t stop;//停止标志
    volatile atomic_t ids;
    u_long accuracy;//计时器精度
    struct thread_ctx thtw;
    struct timer_ctx timer;//计时器
    struct tw_ctx tw;//时间轮
    struct netev_ctx *netev;//网络
};

static inline u_long _cur_tick(struct srey_ctx *pctx)
{
    return (u_long)(timer_nanosec(&pctx->timer) / pctx->accuracy);
}
static inline uint32_t _id_creater(void *pparam)
{
    return (uint32_t)ATOMIC_ADD(&((struct srey_ctx *)pparam)->ids, 1);
}
struct srey_ctx *srey_new()
{
    struct srey_ctx *pctx = MALLOC(sizeof(struct srey_ctx));
    if (NULL == pctx)
    {
        LOG_ERROR("%s", ERRSTR_MEMORY);
        return NULL;
    }

    srand((uint32_t)time(NULL));
    pctx->stop = 0;
    pctx->ids = 1;
    pctx->accuracy = 1000 * 1000;
    thread_init(&pctx->thtw);
    timer_init(&pctx->timer);
    tw_init(&pctx->tw, _cur_tick(pctx));
    pctx->netev = netev_new(&pctx->tw, 0, _id_creater, pctx);
    return pctx;
}
void srey_free(struct srey_ctx *pctx)
{
    netev_free(pctx->netev);
    pctx->stop = 1;
    thread_join(&pctx->thtw);
    tw_free(&pctx->tw);
    FREE(pctx);
}
static void _tw_loop(void *pparam)
{
    struct srey_ctx *pctx = (struct srey_ctx *)pparam;
    while (0 == pctx->stop)
    {
        tw_run(&pctx->tw, _cur_tick(pctx));
        USLEEP(1);
    }
}
void srey_loop(struct srey_ctx *pctx)
{
    thread_creat(&pctx->thtw, _tw_loop, pctx);
    thread_waitstart(&pctx->thtw);
    netev_loop(pctx->netev);
}
struct netev_ctx *srey_netev(struct srey_ctx *pctx)
{
    return pctx->netev;
}
struct tw_ctx *srey_tw(struct srey_ctx *pctx)
{
    return &pctx->tw;
}
