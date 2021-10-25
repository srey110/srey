#include "server.h"

void server_init(struct server_ctx *pctx, uint64_t ulaccuracy)
{
    ASSERTAB(ulaccuracy > 0, "param error.");
    pctx->accuracy = ulaccuracy;
    pctx->stop = 0;
    srand((uint32_t)time(NULL));

    thread_init(&pctx->thread);
    chan_init(&pctx->chan, ONEK);
    timer_init(&pctx->timer);
    wot_init(&pctx->wot, &pctx->chan, server_tick(pctx));
    pctx->netio = netio_new(&pctx->stop);
}
void server_free(struct server_ctx *pctx)
{
    ATOMIC_SET(&pctx->stop, 1);
    chan_close(&pctx->chan);
    thread_join(&pctx->thread);
    wot_free(&pctx->wot);
    netio_free(pctx->netio);
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
static void _server(void *p1, void *p2, void *p3)
{
    u_long ultick;
    struct ev_ctx *pev;
    struct server_ctx *pctx = (struct server_ctx *)p1;
    while (0 == ATOMIC_GET(&pctx->stop))
    {
        pev = _get_ev(&pctx->chan);
        ultick = server_tick(pctx);
        wot_run(&pctx->wot, pev, ultick);
    }
}
void server_run(struct server_ctx *pctx)
{
    thread_creat(&pctx->thread, _server, pctx, NULL, NULL);
    thread_waitstart(&pctx->thread);
    netio_run(pctx->netio);
}
