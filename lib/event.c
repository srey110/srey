#include "event.h"

void event_init(struct event_ctx *pctx, const u_long ulaccuracy)
{
    ASSERTAB(ulaccuracy > 0, "param error.");
    pctx->accuracy = ulaccuracy;
    pctx->stop = 0;
    srand((uint32_t)time(NULL));

    thread_init(&pctx->thwot);
    thread_init(&pctx->thfree);
    chan_init(&pctx->chfree, ONEK * 4, 1);
    timer_init(&pctx->timer);
    netev_init(&pctx->netev);
}
void event_free(struct event_ctx *pctx)
{
    netev_free(&pctx->netev);
    ATOMIC_SET(&pctx->stop, 1);
    thread_join(&pctx->thwot);
    chan_close(&pctx->chfree);    
    thread_join(&pctx->thfree); 
    wot_free(&pctx->wot);
    chan_free(&pctx->chfree);
}
static void _delay_free(void *p1, void *p2, void *p3)
{
    void *ptmp;
    struct sock_ctx *psock;
    struct ev_time_ctx *ptimeev;
    struct event_ctx *pctx = (struct event_ctx *)p1;
    while (0 == ATOMIC_GET(&pctx->stop))
    {
        ptmp = chan_recv(&pctx->chfree);
        if (NULL != ptmp)
        {
            ptimeev = UPCAST(ptmp, struct ev_time_ctx, ev);
            psock = (struct sock_ctx *)ptimeev->data;
            SAFE_FREE(ptimeev);
            event_sock_free(pctx, psock);
        }      
    }
}
static void _wot_loop(void *p1, void *p2, void *p3)
{
    struct event_ctx *pctx = (struct event_ctx *)p1;
    wot_init(&pctx->wot, event_tick(pctx));
    while (0 == ATOMIC_GET(&pctx->stop))
    {
        wot_run(&pctx->wot, event_tick(pctx));
    }
}
void event_loop(struct event_ctx *pctx)
{
    thread_creat(&pctx->thfree, _delay_free, pctx, NULL, NULL);
    thread_creat(&pctx->thwot, _wot_loop, pctx, NULL, NULL);
    netev_loop(&pctx->netev);
}
