#include "chan.h"

void chan_init(struct chan_ctx *pctx, const int32_t icapacity, const int32_t iexpandqu)
{
    ASSERTAB(icapacity > 0, "capacity must big than 0.");
    pctx->expand = iexpandqu;
    pctx->closed = 0;
    pctx->rwaiting = 0;
    pctx->wwaiting = 0;
    queue_init(&pctx->queue, icapacity);
    mutex_init(&pctx->mmutex);
    cond_init(&pctx->rcond);
    cond_init(&pctx->wcond);
}
void chan_free(struct chan_ctx *pctx)
{
    mutex_free(&pctx->mmutex);
    cond_free(&pctx->rcond);
    cond_free(&pctx->wcond);
    queue_free(&pctx->queue);
}
void chan_close(struct chan_ctx *pctx)
{
    mutex_lock(&pctx->mmutex);
    if (0 == pctx->closed)
    {
        pctx->closed = 1;
        cond_broadcast(&pctx->rcond);
        cond_broadcast(&pctx->wcond);
    }
    mutex_unlock(&pctx->mmutex);
}
int32_t _chan_send(struct chan_ctx *pctx, struct message_ctx *pdata)
{
    if (0 != pctx->expand)
    {
        queue_expand(&pctx->queue);
    }
    while (0 == pctx->closed
        && queue_size(&pctx->queue) == queue_cap(&pctx->queue))
    {
        //队列满 阻塞直到有数据被移除.
        pctx->wwaiting++;
        cond_wait(&pctx->wcond, &pctx->mmutex);
        pctx->wwaiting--;
    }
    if (0 != pctx->closed)
    {
        return ERR_FAILED;
    }
    (void)queue_push(&pctx->queue, pdata);
    if (pctx->rwaiting > 0)
    {
        //通知可读.
        cond_signal(&pctx->rcond);
    }

    return ERR_OK;
}
int32_t _chan_recv(struct chan_ctx *pctx, struct message_ctx *pdata)
{
    while (0 == queue_size(&pctx->queue))
    {
        if (0 != pctx->closed)
        {
            return ERR_FAILED;
        }

        //阻塞直到有数据.
        pctx->rwaiting++;
        cond_wait(&pctx->rcond, &pctx->mmutex);
        pctx->rwaiting--;
    }
    int32_t irtn = queue_pop(&pctx->queue, pdata);
    if (pctx->wwaiting > 0)
    {
        //通知可写.
        cond_signal(&pctx->wcond);
    }

    return irtn;
}
int32_t chan_send(struct chan_ctx *pctx, struct message_ctx *pdata)
{
    int32_t irtn = ERR_FAILED;
    mutex_lock(&pctx->mmutex);
    if (0 == pctx->closed)
    {
        irtn = _chan_send(pctx, pdata);
    }
    mutex_unlock(&pctx->mmutex);
    return irtn;
}
int32_t chan_trysend(struct chan_ctx *pctx, struct message_ctx *pdata)
{
    int32_t irtn = ERR_FAILED;
    mutex_lock(&pctx->mmutex);
    if (0 == pctx->closed
        && queue_size(&pctx->queue) < queue_cap(&pctx->queue))
    {
        irtn = _chan_send(pctx, pdata);
    }
    mutex_unlock(&pctx->mmutex);
    return irtn;
}
int32_t chan_recv(struct chan_ctx *pctx, struct message_ctx *pdata)
{
    mutex_lock(&pctx->mmutex);
    int32_t irtn = _chan_recv(pctx, pdata);
    mutex_unlock(&pctx->mmutex);
    return irtn;
}
int32_t chan_tryrecv(struct chan_ctx *pctx, struct message_ctx *pdata)
{
    int32_t irtn = ERR_FAILED;
    mutex_lock(&pctx->mmutex);
    if (queue_size(&pctx->queue) > 0)
    {
        irtn = _chan_recv(pctx, pdata);
    }
    mutex_unlock(&pctx->mmutex);
    return irtn;
}
int32_t chan_size(struct chan_ctx *pctx)
{
    mutex_lock(&pctx->mmutex);
    int32_t isize = queue_size(&pctx->queue);
    mutex_unlock(&pctx->mmutex);
    return isize;
}
int32_t chan_closed(struct chan_ctx *pctx)
{
    mutex_lock(&pctx->mmutex);
    int32_t iclosed = pctx->closed;
    mutex_unlock(&pctx->mmutex);
    return iclosed;
}
