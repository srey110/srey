#include "chan.h"

typedef struct select_ctx
{
    int32_t recv;
    int32_t index;
    void *pmsgin;
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

    srand((uint32_t)time(NULL));
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
int32_t chan_isclosed(struct chan_ctx *pctx)
{
    mutex_lock(&pctx->mmutex);
    int32_t iclosed = pctx->closed;
    mutex_unlock(&pctx->mmutex);

    return iclosed;
}
int32_t chan_send(struct chan_ctx *pctx, void *pdata)
{
    if (0 != chan_isclosed(pctx))
    {
        return ERR_FAILED;
    }
    return NULL != pctx->queue ? _bufferedsend(pctx, pdata) : _unbufferedsend(pctx, pdata);
};
int32_t chan_recv(struct chan_ctx *pctx, void **pdata)
{
    return NULL != pctx->queue ? _bufferedrecv(pctx, pdata) : _unbufferedrecv(pctx, pdata);
}
int32_t chan_size(struct chan_ctx *pctx)
{
    int32_t isize = 0;
    if (NULL != pctx->queue)
    {
        mutex_lock(&pctx->mmutex);
        isize = queue_size(pctx->queue);
        mutex_unlock(&pctx->mmutex);
    }
    return isize;
}
int32_t chan_canrecv(struct chan_ctx *pctx)
{
    if (NULL != pctx->queue)
    {
        return  chan_size(pctx) > 0 ? ERR_OK : ERR_FAILED;
    }

    mutex_lock(&pctx->mmutex);
    int32_t icanrecv = pctx->wwaiting > 0 ? ERR_OK : ERR_FAILED;
    mutex_unlock(&pctx->mmutex);

    return icanrecv;
}
int32_t chan_cansend(struct chan_ctx *pctx)
{
    int32_t isend;
    if (NULL != pctx->queue)
    {
        mutex_lock(&pctx->mmutex);
        isend = queue_size(pctx->queue) < queue_cap(pctx->queue) ? ERR_OK : ERR_FAILED;
        mutex_unlock(&pctx->mmutex);
    }
    else
    {
        mutex_lock(&pctx->mmutex);
        isend = pctx->rwaiting > 0 ? ERR_OK : ERR_FAILED;
        mutex_unlock(&pctx->mmutex);
    }

    return isend;
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
            struct select_ctx stselect;
            stselect.recv = 1;
            stselect.chan = pchan;
            stselect.pmsgin = NULL;
            stselect.index = i;
            pselect[icount++] = stselect;
        }
    }
    for (i = 0; i < isend_count; i++)
    {
        struct chan_ctx *pchan = psend[i];
        if (ERR_OK == chan_cansend(pchan))
        {
            struct select_ctx stselect;
            stselect.recv = 0;
            stselect.chan = pchan;
            stselect.pmsgin = psend_msgs[i];
            stselect.index = i + irecv_count;
            pselect[icount++] = stselect;
        }
    }

    if (0 == icount)
    {
        SAFE_FREE(pselect);
        return ERR_FAILED;
    }

    struct select_ctx stselect = pselect[rand() % icount];
    if (1 == stselect.recv)
    {
        if (ERR_OK != chan_recv(stselect.chan, precv_out))
        {
            SAFE_FREE(pselect);
            return ERR_FAILED;
        }
    }
    else
    {
        if (ERR_OK != chan_send(stselect.chan, stselect.pmsgin))
        {
            SAFE_FREE(pselect);
            return ERR_FAILED;
        }
    }

    int32_t idex = stselect.index;
    SAFE_FREE(pselect);

    return idex;
}
