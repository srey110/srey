#include "chan.h"

typedef struct select_ctx
{
    int32_t recv;
    int32_t index;
    void *pmsg_in;
    struct chan_ctx *chan;
} select_ctx;

void chan_init(struct chan_ctx *pctx, const int32_t icapacity)
{
    pctx->queue = NULL;
    if (icapacity > 0)
    {
        pctx->queue = (struct queue_ctx *)MALLOC(sizeof(struct queue_ctx));
        ASSERTAB(NULL != pctx->queue, ERRSTR_MEMORY);
        queue_init(pctx->queue, icapacity);
    }

    pctx->closed = 0;
    pctx->rwaiting = 0;
    pctx->wwaiting = 0;
    pctx->data = NULL;

    mutex_init(&pctx->rmutex);
    mutex_init(&pctx->wmutex);
    mutex_init(&pctx->mmutex);
    cond_init(&pctx->rcond);
    cond_init(&pctx->wcond);
}
void chan_free(struct chan_ctx *pctx)
{
    mutex_free(&pctx->rmutex);
    mutex_free(&pctx->wmutex);
    mutex_free(&pctx->mmutex);
    cond_free(&pctx->rcond);
    cond_free(&pctx->wcond);
    if (NULL != pctx->queue)
    {
        queue_free(pctx->queue);
        SAFE_FREE(pctx->queue);
    }
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
int32_t _bufferedsend(struct chan_ctx *pctx, void *pdata)
{
    mutex_lock(&pctx->mmutex);
    while (queue_size(pctx->queue) == queue_cap(pctx->queue))
    {
        //队列满 阻塞直到有数据被移除.
        pctx->wwaiting++;
        cond_wait(&pctx->wcond, &pctx->mmutex);
        pctx->wwaiting--;
    }

    int32_t isuccess = queue_push(pctx->queue, pdata);

    if (pctx->rwaiting > 0)
    {
        //通知可读.
        cond_signal(&pctx->rcond);
    }

    mutex_unlock(&pctx->mmutex);

    return isuccess;
}
int32_t _bufferedrecv(struct chan_ctx *pctx, void **pdata)
{
    mutex_lock(&pctx->mmutex);
    while (0 == queue_size(pctx->queue))
    {
        if (0 != pctx->closed)
        {
            mutex_unlock(&pctx->mmutex);
            return ERR_FAILED;
        }

        //阻塞直到有数据.
        pctx->rwaiting++;
        cond_wait(&pctx->rcond, &pctx->mmutex);
        pctx->rwaiting--;
    }

    *pdata = queue_pop(pctx->queue);

    if (pctx->wwaiting > 0)
    {
        //通知可写.
        cond_signal(&pctx->wcond);
    }

    mutex_unlock(&pctx->mmutex);

    return ERR_OK;
}
int32_t _unbufferedsend(struct chan_ctx *pctx, void* pdata)
{
    mutex_lock(&pctx->wmutex);
    mutex_lock(&pctx->mmutex);

    if (0 != pctx->closed)
    {
        mutex_unlock(&pctx->mmutex);
        mutex_unlock(&pctx->wmutex);
        return ERR_FAILED;
    }

    pctx->data = pdata;

    pctx->wwaiting++;
    if (pctx->rwaiting > 0)
    {
        // 激发读取.
        cond_signal(&pctx->rcond);
    }

    //阻塞直到数据被取出.
    cond_wait(&pctx->wcond, &pctx->mmutex);

    mutex_unlock(&pctx->mmutex);
    mutex_unlock(&pctx->wmutex);

    return ERR_OK;
}
int32_t _unbufferedrecv(struct chan_ctx *pctx, void **pdata)
{
    mutex_lock(&pctx->rmutex);
    mutex_lock(&pctx->mmutex);

    while (0 == pctx->closed
        && 0 == pctx->wwaiting)
    {
        //阻塞直到有数据.
        pctx->rwaiting++;
        cond_wait(&pctx->rcond, &pctx->mmutex);
        pctx->rwaiting--;
    }

    if (0 != pctx->closed)
    {
        mutex_unlock(&pctx->mmutex);
        mutex_unlock(&pctx->rmutex);
        return ERR_FAILED;
    }

    *pdata = pctx->data;

    pctx->wwaiting--;
    //通知可写.
    cond_signal(&pctx->wcond);

    mutex_unlock(&pctx->mmutex);
    mutex_unlock(&pctx->rmutex);

    return ERR_OK;
}
int32_t chan_select(struct chan_ctx *precv[], const int32_t irecv_count, void **precv_out,
    struct chan_ctx *psend[], const int32_t isend_count, void *psend_msgs[])
{
    struct select_ctx *pselect = MALLOC(sizeof(struct select_ctx) * (irecv_count + isend_count));
    if (NULL == pselect)
    {
        return ERR_FAILED;
    }

    int32_t i;
    int32_t icount = 0;
    for (i = 0; i < irecv_count; i++)
    {
        struct chan_ctx *pchan = precv[i];
        if (ERR_OK == chan_canrecv(pchan))
        {
            pselect[icount].recv = 1;
            pselect[icount].chan = pchan;
            pselect[icount].pmsg_in = NULL;
            pselect[icount].index = i;
            icount++;
        }
    }
    for (i = 0; i < isend_count; i++)
    {
        struct chan_ctx *pchan = psend[i];
        if (ERR_OK == chan_cansend(pchan))
        {
            pselect[icount].recv = 0;
            pselect[icount].chan = pchan;
            pselect[icount].pmsg_in = psend_msgs[i];
            pselect[icount].index = i + irecv_count;
            icount++;
        }
    }
    if (0 == icount)
    {
        SAFE_FREE(pselect);
        return ERR_FAILED;
    }

    struct select_ctx *pselected = &pselect[rand() % icount];
    if (1 == pselected->recv)
    {
        if (ERR_OK != chan_recv(pselected->chan, precv_out))
        {
            SAFE_FREE(pselect);
            return ERR_FAILED;
        }
    }
    else
    {
        if (ERR_OK != chan_send(pselected->chan, pselected->pmsg_in))
        {
            SAFE_FREE(pselect);
            return ERR_FAILED;
        }
    }

    int32_t idex = pselected->index;
    SAFE_FREE(pselect);

    return idex;
}
