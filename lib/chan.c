#include "chan.h"

void chan_init(chan_ctx *ctx, const size_t maxsize)
{
    ctx->closed = ctx->rwaiting = ctx->wwaiting = 0;
    qu_void_init(&ctx->queue, maxsize);
    mutex_init(&ctx->mmutex);
    cond_init(&ctx->rcond);
    cond_init(&ctx->wcond);
}
void chan_free(chan_ctx *ctx)
{
    mutex_free(&ctx->mmutex);
    cond_free(&ctx->rcond);
    cond_free(&ctx->wcond);
    qu_void_free(&ctx->queue);
}
void chan_close(chan_ctx *ctx)
{
    mutex_lock(&ctx->mmutex);
    if (0 == ctx->closed)
    {
        ctx->closed = 1;
        cond_broadcast(&ctx->rcond);
        cond_broadcast(&ctx->wcond);
    }
    mutex_unlock(&ctx->mmutex);
}
static int32_t _chan_send(chan_ctx *ctx, void *data)
{
    while (0 == ctx->closed
        && qu_void_size(&ctx->queue) == qu_void_maxsize(&ctx->queue))
    {
        //队列满 阻塞直到有数据被移除.
        ctx->wwaiting++;
        cond_wait(&ctx->wcond, &ctx->mmutex);
        ctx->wwaiting--;
    }
    if (0 != ctx->closed)
    {
        return ERR_FAILED;
    }
    qu_void_push(&ctx->queue, &data);
    if (ctx->rwaiting > 0)
    {
        //通知可读.
        cond_signal(&ctx->rcond);
    }
    return ERR_OK;
}
static int32_t _chan_recv(chan_ctx *ctx, void **data)
{
    while (qu_void_empty(&ctx->queue))
    {
        if (0 != ctx->closed)
        {
            return ERR_FAILED;
        }
        //阻塞直到有数据.
        ctx->rwaiting++;
        cond_wait(&ctx->rcond, &ctx->mmutex);
        ctx->rwaiting--;
    }
    *data = *qu_void_pop(&ctx->queue);
    if (ctx->wwaiting > 0)
    {
        //通知可写.
        cond_signal(&ctx->wcond);
    }
    return ERR_OK;
}
int32_t chan_send(chan_ctx *ctx, void *data)
{
    int32_t rtn = ERR_FAILED;
    mutex_lock(&ctx->mmutex);
    if (0 == ctx->closed)
    {
        rtn = _chan_send(ctx, data);
    }
    mutex_unlock(&ctx->mmutex);
    return rtn;
}
int32_t chan_trysend(chan_ctx *ctx, void *data)
{
    int32_t rtn = ERR_FAILED;
    mutex_lock(&ctx->mmutex);
    if (0 == ctx->closed
        && qu_void_size(&ctx->queue) < qu_void_maxsize(&ctx->queue))
    {
        rtn = _chan_send(ctx, data);
    }
    mutex_unlock(&ctx->mmutex);
    return rtn;
}
int32_t chan_recv(chan_ctx *ctx, void **data)
{
    mutex_lock(&ctx->mmutex);
    int32_t rtn = _chan_recv(ctx, data);
    mutex_unlock(&ctx->mmutex);
    return rtn;
}
int32_t chan_tryrecv(chan_ctx *ctx, void **data)
{
    int32_t rtn = ERR_FAILED;
    mutex_lock(&ctx->mmutex);
    if (qu_void_size(&ctx->queue) > 0)
    {
        rtn = _chan_recv(ctx, data);
    }
    mutex_unlock(&ctx->mmutex);
    return rtn;
}
size_t chan_size(chan_ctx *pctx)
{
    mutex_lock(&pctx->mmutex);
    size_t size = qu_void_size(&pctx->queue);
    mutex_unlock(&pctx->mmutex);
    return size;
}
int32_t chan_closed(chan_ctx *pctx)
{
    mutex_lock(&pctx->mmutex);
    int32_t closed = pctx->closed;
    mutex_unlock(&pctx->mmutex);
    return closed;
}
