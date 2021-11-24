#include "srey.h"
#include "timer.h"

struct srey_ctx
{
    volatile int32_t stop;//停止标志
    volatile atomic64_t ids;
    uint32_t accuracy;//计时器精度
    uint32_t workercnt;
    uint32_t waiting;
    struct netev_ctx *netev;//网络
    struct thread_ctx *thr_worker;
    struct thread_ctx thr_tw;
    struct timer_ctx timer;//计时器
    struct tw_ctx tw;//时间轮
    mutex_ctx mu_worker;
    cond_ctx cond_worker;
};

static inline uint32_t _cur_tick(struct srey_ctx *pctx)
{
    return (uint32_t)(timer_nanosec(&pctx->timer) / pctx->accuracy);
}
sid_t srey_id(void *pparam)
{
    return (sid_t)ATOMIC64_ADD(&((struct srey_ctx *)pparam)->ids, 1);
}
struct srey_ctx *srey_new()
{
    struct srey_ctx *pctx = MALLOC(sizeof(struct srey_ctx));
    ASSERTAB(NULL != pctx, ERRSTR_MEMORY);
    srand((uint32_t)time(NULL));
    pctx->stop = 0;
    pctx->waiting = 0;
    pctx->ids = 1;
    pctx->accuracy = 1000 * 1000;
    pctx->workercnt = procscnt() * 2;
    pctx->thr_worker = MALLOC(sizeof(struct thread_ctx) * pctx->workercnt);
    ASSERTAB(NULL != pctx->thr_worker, ERRSTR_MEMORY);
    for (uint32_t i = 0; i < pctx->workercnt; i++)
    {
        thread_init(&pctx->thr_worker[i]);
    }
    mutex_init(&pctx->mu_worker);
    cond_init(&pctx->cond_worker);
    thread_init(&pctx->thr_tw);
    timer_init(&pctx->timer);
    tw_init(&pctx->tw, _cur_tick(pctx));
    pctx->netev = netev_new(&pctx->tw, 0, srey_id, pctx);
    return pctx;
}
void srey_free(struct srey_ctx *pctx)
{
    netev_free(pctx->netev);
    pctx->stop = 1;
    thread_join(&pctx->thr_tw);
    cond_broadcast(&pctx->cond_worker);
    for (uint32_t i = 0; i < pctx->workercnt; i++)
    {
        thread_join(&pctx->thr_worker[i]);
    }
    mutex_free(&pctx->mu_worker);
    cond_free(&pctx->cond_worker);
    tw_free(&pctx->tw);
    FREE(pctx->thr_worker);
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
static void _worker(void *pparam)
{
    struct srey_ctx *pctx = (struct srey_ctx *)pparam;
    while (0 == pctx->stop)
    {
        mutex_lock(&pctx->mu_worker);
        pctx->waiting++;
        if (0 == pctx->stop)
        {
            cond_wait(&pctx->cond_worker, &pctx->mu_worker);
        }
        pctx->waiting--;
        mutex_unlock(&pctx->mu_worker);
    }
}
void srey_wakeup(struct srey_ctx *pctx)
{
    if (pctx->waiting > 0)
    {
        cond_signal(&pctx->cond_worker);
    }
}
void srey_loop(struct srey_ctx *pctx)
{
    for (uint32_t i = 0; i < pctx->workercnt; i++)
    {
        thread_creat(&pctx->thr_worker[i], _worker, pctx);
        thread_waitstart(&pctx->thr_worker[i]);
    }
    thread_creat(&pctx->thr_tw, _tw_loop, pctx);
    thread_waitstart(&pctx->thr_tw);
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
