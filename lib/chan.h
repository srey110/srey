#ifndef CHAN_H_
#define CHAN_H_

#include "queue.h"
#include "mutex.h"
#include "cond.h"

struct chan_ctx
{
    int32_t expand;
    int32_t closed;
    int32_t rwaiting;
    int32_t wwaiting;
    mutex_ctx mmutex;//读写信号锁
    cond_ctx rcond;
    cond_ctx wcond;
    struct queue_ctx queue;
};
/*
* \brief             初始化
* \param icapacity   队列容量
* \param iexpandqu   是的能自动扩充， 0 否
*/
static inline void chan_init(struct chan_ctx *pctx, const int32_t icapacity, const int32_t iexpandqu)
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
};
/*
* \brief   释放
*/
static inline void chan_free(struct chan_ctx *pctx)
{
    mutex_free(&pctx->mmutex);
    cond_free(&pctx->rcond);
    cond_free(&pctx->wcond);
    queue_free(&pctx->queue);
};
/*
* \brief  关闭channel，关闭后不能写入
*/
static inline void chan_close(struct chan_ctx *pctx)
{
    mutex_lock(&pctx->mmutex);
    if (0 == pctx->closed)
    {
        pctx->closed = 1;
        cond_broadcast(&pctx->rcond);
        cond_broadcast(&pctx->wcond);
    }
    mutex_unlock(&pctx->mmutex);
};
static inline int32_t _chan_send(struct chan_ctx *pctx, void *pdata)
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
};
static inline void *_chan_recv(struct chan_ctx *pctx)
{
    while (0 == queue_size(&pctx->queue))
    {
        if (0 != pctx->closed)
        {
            return NULL;
        }

        //阻塞直到有数据.
        pctx->rwaiting++;
        cond_wait(&pctx->rcond, &pctx->mmutex);
        pctx->rwaiting--;
    }
    void *pdata = queue_pop(&pctx->queue);
    if (pctx->wwaiting > 0)
    {
        //通知可写.
        cond_signal(&pctx->wcond);
    }
    return pdata;
};
/*
* \brief          写入数据
* \param pdata    待写入的数据
* \return         ERR_OK 成功
*/
static inline int32_t chan_send(struct chan_ctx *pctx, void *pdata)
{
    int32_t irtn = ERR_FAILED;
    mutex_lock(&pctx->mmutex);
    if (0 == pctx->closed)
    {
        irtn = _chan_send(pctx, pdata);
    }
    mutex_unlock(&pctx->mmutex);
    return irtn;
};
static inline int32_t chan_trysend(struct chan_ctx *pctx, void *pdata)
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
};
/*
* \brief          读取数据
* \param pdata    读取到的数据
* \return         ERR_OK 成功
*/
static inline void *chan_recv(struct chan_ctx *pctx)
{
    mutex_lock(&pctx->mmutex);
    void *pdata = _chan_recv(pctx);
    mutex_unlock(&pctx->mmutex);
    return pdata;
};
static inline void *chan_tryrecv(struct chan_ctx *pctx)
{
    void *pdata = NULL;
    mutex_lock(&pctx->mmutex);
    if (queue_size(&pctx->queue) > 0)
    {
        pdata = _chan_recv(pctx);
    }
    mutex_unlock(&pctx->mmutex);
    return pdata;
};
static inline void chan_lock(struct chan_ctx *pctx)
{
    mutex_lock(&pctx->mmutex);
};
static inline void chan_unlock(struct chan_ctx *pctx)
{
    mutex_unlock(&pctx->mmutex);
};
/*
* \brief          数据总数
* \return         数据总数
*/
static inline int32_t chan_size(struct chan_ctx *pctx)
{
    mutex_lock(&pctx->mmutex);
    int32_t isize = queue_size(&pctx->queue);
    mutex_unlock(&pctx->mmutex);
    return isize;
};
/*
* \brief          channel是否关闭
* \return         0 未关闭
*/
static inline int32_t chan_closed(struct chan_ctx *pctx)
{
    mutex_lock(&pctx->mmutex);
    int32_t iclosed = pctx->closed;
    mutex_unlock(&pctx->mmutex);
    return iclosed;
};

#endif//CHAN_H_
