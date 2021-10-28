#ifndef CHAN_H_
#define CHAN_H_

#include "queue.h"
#include "mutex.h"
#include "cond.h"

typedef struct chan_ctx
{
    mutex_ctx rmutex;//读锁
    mutex_ctx wmutex;//写锁    
    mutex_ctx mmutex;//读写信号锁
    cond_ctx rcond;
    cond_ctx wcond;
    int32_t closed;
    int32_t rwaiting;
    int32_t wwaiting;
    struct queue_ctx *queue;
    void *data;
}chan_ctx;
/*
* \brief   初始化
* \param   icapacity 大于0 带缓存非阻塞
*/
void chan_init(struct chan_ctx *pctx, const int32_t icapacity);
/*
* \brief   释放
*/
void chan_free(struct chan_ctx *pctx);

/*
* \brief  关闭channel，关闭后不能写入
*/
void chan_close(struct chan_ctx *pctx);
/*
* \brief          channel是否关闭
* \return         0 未关闭
*/
static inline int32_t chan_isclosed(struct chan_ctx *pctx)
{
    mutex_lock(&pctx->mmutex);
    int32_t iclosed = pctx->closed;
    mutex_unlock(&pctx->mmutex);

    return iclosed;
};
int32_t _bufferedsend(struct chan_ctx *pctx, void *pdata);
int32_t _bufferedrecv(struct chan_ctx *pctx, void **pdata);
int32_t _unbufferedsend(struct chan_ctx *pctx, void* pdata);
int32_t _unbufferedrecv(struct chan_ctx *pctx, void **pdata);
/*
* \brief          写入数据
* \param pdata    待写入的数据
* \return         ERR_OK 成功
*/
static inline int32_t chan_send(struct chan_ctx *pctx, void *pdata)
{
    if (0 != chan_isclosed(pctx))
    {
        return ERR_FAILED;
    }
    return NULL != pctx->queue ? _bufferedsend(pctx, pdata) : _unbufferedsend(pctx, pdata);
};
/*
* \brief          读取数据
* \param pdata    读取到的数据
* \return         ERR_OK 成功
*/
static inline int32_t chan_recv(struct chan_ctx *pctx, void **pdata)
{
    return NULL != pctx->queue ? _bufferedrecv(pctx, pdata) : _unbufferedrecv(pctx, pdata);
};
/*
* \brief          数据总数
* \return         数据总数
*/
static inline int32_t chan_size(struct chan_ctx *pctx)
{
    int32_t isize = 0;
    if (NULL != pctx->queue)
    {
        mutex_lock(&pctx->mmutex);
        isize = queue_size(pctx->queue);
        mutex_unlock(&pctx->mmutex);
    }
    return isize;
};
/*
* \brief          是否可读
* \return         ERR_OK 有数据
*/
static inline int32_t chan_canrecv(struct chan_ctx *pctx)
{
    if (NULL != pctx->queue)
    {
        return  chan_size(pctx) > 0 ? ERR_OK : ERR_FAILED;
    }

    mutex_lock(&pctx->mmutex);
    int32_t icanrecv = pctx->wwaiting > 0 ? ERR_OK : ERR_FAILED;
    mutex_unlock(&pctx->mmutex);

    return icanrecv;
};
/*
* \brief          是否可写
* \return         ERR_OK 可写
*/
static inline int32_t chan_cansend(struct chan_ctx *pctx)
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
};
/*
* \brief             随机选取一可读写的channel来执行读写,阻塞的不支持
* \param precv       读cchan
* \param irecvcnt    读cchan数量
* \param precv_out   读到的数据
* \param psend       写cchan
* \param isendcnt    写cchan数量
* \param psend_msgs  每个cchan需要发送的数据
* \return            ERR_FAILED 失败
* \return            cchan 下标
*/
int32_t chan_select(struct chan_ctx *precv[], const int32_t irecvcnt, void **precv_out,
    struct chan_ctx *psend[], const int32_t isendcnt, void *psend_msgs[]);

#endif//CHAN_H_
