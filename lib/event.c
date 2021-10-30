#include "event.h"

void event_init(struct event_ctx *pctx, const u_long ulaccuracy)
{
    ASSERTAB(ulaccuracy > 0, "param error.");
    pctx->accuracy = ulaccuracy;
    pctx->stop = 0;
    srand((uint32_t)time(NULL));

    thread_init(&pctx->thread);
    chan_init(&pctx->chan, ONEK);
    timer_init(&pctx->timer);
    wot_init(&pctx->wot, &pctx->chan, event_tick(pctx));
    netev_init(&pctx->netev);
}
void event_free(struct event_ctx *pctx)
{
    netev_free(&pctx->netev);
    ATOMIC_SET(&pctx->stop, 1);
    chan_close(&pctx->chan);
    thread_join(&pctx->thread);
    wot_free(&pctx->wot);
    chan_free(&pctx->chan);
}
static inline struct ev_ctx *_get_ev(struct chan_ctx *pchan)
{
    if (ERR_OK != chan_canrecv(pchan))
    {
        return NULL;
    }

    void *ptmp;
    if (ERR_OK == chan_recv(pchan, &ptmp))
    {
        return (struct ev_ctx *)ptmp;
    }
    return NULL;
}
static void _loop(void *p1, void *p2, void *p3)
{
    struct ev_ctx *pev;
    struct event_ctx *pctx = (struct event_ctx *)p1;
    while (0 == ATOMIC_GET(&pctx->stop))
    {
        pev = _get_ev(&pctx->chan);
        wot_run(&pctx->wot, pev, event_tick(pctx));
    }
}
void event_loop(struct event_ctx *pctx)
{
    thread_creat(&pctx->thread, _loop, pctx, NULL, NULL);
    thread_waitstart(&pctx->thread);
    netev_loop(&pctx->netev);
}
