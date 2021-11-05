#include "event.h"

void event_init(struct event_ctx *pctx, const u_long ulaccuracy)
{
    pctx->accuracy = (0 == ulaccuracy ? 1000 * 1000 * 10 : ulaccuracy);
    pctx->stop = 0;
    srand((uint32_t)time(NULL));

    thread_init(&pctx->thwot);
    thread_init(&pctx->thfree);
    chan_init(&pctx->chfree, ONEK * 4, 1);
    timer_init(&pctx->timer);
    pctx->netev = netev_new();
}
void event_free(struct event_ctx *pctx)
{
    netev_free(pctx->netev);
    pctx->stop = 1;
    thread_join(&pctx->thwot);
    chan_close(&pctx->chfree);    
    thread_join(&pctx->thfree); 
    wot_free(&pctx->wot);
    chan_free(&pctx->chfree);
}
static void _delay_free(void *p1, void *p2, void *p3)
{
    struct ev_ctx *pev;
    struct sock_ctx *psock;
    struct ev_time_ctx *ptimeev;
    struct event_ctx *pctx = (struct event_ctx *)p1;
    while (0 == pctx->stop)
    {
        pev = (struct ev_ctx *)chan_recv(&pctx->chfree);
        if (NULL != pev)
        {
            ptimeev = UPCAST(pev, struct ev_time_ctx, ev);
            psock = (struct sock_ctx *)ptimeev->data;
            if (ERR_OK == _sock_can_free(psock))
            {
                _sock_free(psock);
                FREE(ptimeev);
            }
            else
            {
                ptimeev->expires = 5 + event_tick(pctx);
                _wot_add(&pctx->wot, ptimeev);
            }
        }
    }
}
static void _wot_loop(void *p1, void *p2, void *p3)
{
    struct event_ctx *pctx = (struct event_ctx *)p1;
    wot_init(&pctx->wot, event_tick(pctx));
    while (0 == pctx->stop)
    {
        wot_run(&pctx->wot, event_tick(pctx));
    }
}
void event_loop(struct event_ctx *pctx)
{
    thread_creat(&pctx->thfree, _delay_free, pctx, NULL, NULL);
    thread_creat(&pctx->thwot, _wot_loop, pctx, NULL, NULL);
    netev_loop(pctx->netev);
}
