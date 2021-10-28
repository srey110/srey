#include "server.h"
#include "loger.h"

void server_init(struct server_ctx *pctx, const uint64_t ulaccuracy)
{
    ASSERTAB(ulaccuracy > 0, "param error.");
    pctx->accuracy = ulaccuracy;
    pctx->stop = 0;
    srand((uint32_t)time(NULL));

    thread_init(&pctx->thread);
    chan_init(&pctx->chan, ONEK);
    chan_init(&pctx->chanfree, ONEK);
    timer_init(&pctx->timer);
    wot_init(&pctx->wot, &pctx->chan, server_tick(pctx));
    pctx->netio = netio_new();
}
void server_free(struct server_ctx *pctx)
{
    ATOMIC_SET(&pctx->stop, 1);
    chan_close(&pctx->chan);
    chan_close(&pctx->chanfree);
    thread_join(&pctx->thread);
    wot_free(&pctx->wot);
    netio_free(pctx->netio);
    chan_free(&pctx->chan);
    chan_free(&pctx->chanfree);
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
static inline void _sock_free(struct server_ctx *pctx)
{
    if (ERR_OK != chan_canrecv(&pctx->chanfree))
    {
        return;
    }
    void *ptmp;
    if (ERR_OK != chan_recv(&pctx->chanfree, &ptmp))
    {
        return;
    }

    struct twnode_ctx *pnode = UPCAST(ptmp, struct twnode_ctx, ev);
    struct sock_ctx *psock = pnode->ev.data;
    SAFE_FREE(pnode);
    psock->reffree--;
    atomic_t iref = ATOMIC_GET(&psock->ref);
    atomic_t irefsend = ATOMIC_GET(&psock->refsend);
    if (0 == iref 
        && 0 == irefsend)
    {
        sockctx_free(psock, 0);
    }
    else if(0 == psock->reffree)
    {
        LOG_WARN("free sock_ctx use long time sock %d ref %d refsend %d",
            (int32_t)psock->sock, iref, irefsend);
        sockctx_free(psock, 0);
    }
    else
    {
        if (ERR_OK != server_timeout(pctx, &pctx->chanfree, 10, psock))
        {
            sockctx_free(psock, 0);
        }
    }
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

        _sock_free(pctx);
    }
}
void server_run(struct server_ctx *pctx)
{
    thread_creat(&pctx->thread, _server, pctx, NULL, NULL);
    thread_waitstart(&pctx->thread);
    netio_run(pctx->netio);
}
