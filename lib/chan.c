#include "chan.h"

void chan_init(struct chan_ctx *pctx, const int32_t icapacity)
{
    ASSERTAB(icapacity > 0, "capacity must big than 0.");
    pctx->closed = 0;
    pctx->rwaiting = 0;
    pctx->wwaiting = 0;
    qu_chanmsg_init(&pctx->queue, icapacity);
    mutex_init(&pctx->mmutex);
    cond_init(&pctx->rcond);
    cond_init(&pctx->wcond);
}
void chan_free(struct chan_ctx *pctx)
{
    mutex_free(&pctx->mmutex);
    cond_free(&pctx->rcond);
    cond_free(&pctx->wcond);
    qu_chanmsg_free(&pctx->queue);
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
static int32_t _chan_send(struct chan_ctx *pctx, struct message_ctx *pdata)
{
    while (0 == pctx->closed
        && qu_chanmsg_size(&pctx->queue) == qu_chanmsg_maxsize(&pctx->queue))
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
    qu_chanmsg_push(&pctx->queue, pdata);
    if (pctx->rwaiting > 0)
    {
        //通知可读.
        cond_signal(&pctx->rcond);
    }
    return ERR_OK;
}
static int32_t _chan_recv(struct chan_ctx *pctx, struct message_ctx *pdata)
{
    while (qu_chanmsg_empty(&pctx->queue))
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
    *pdata = *qu_chanmsg_pop(&pctx->queue);
    if (pctx->wwaiting > 0)
    {
        //通知可写.
        cond_signal(&pctx->wcond);
    }

    return ERR_OK;
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
        && qu_chanmsg_size(&pctx->queue) < qu_chanmsg_maxsize(&pctx->queue))
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
    if (qu_chanmsg_size(&pctx->queue) > 0)
    {
        irtn = _chan_recv(pctx, pdata);
    }
    mutex_unlock(&pctx->mmutex);
    return irtn;
}
int32_t chan_size(struct chan_ctx *pctx)
{
    mutex_lock(&pctx->mmutex);
    int32_t isize = qu_chanmsg_size(&pctx->queue);
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
