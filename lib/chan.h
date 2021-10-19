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
* \param   icapacity 大于0 带缓存非阻塞
*/
void chan_init(struct chan_ctx *pctx, const int32_t icapacity);
/*
* \param   释放
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
int32_t chan_isclosed(struct chan_ctx *pctx);
/*
* \brief          写入数据
* \param pdata    待写入的数据
* \return         ERR_OK 成功
*/
int32_t chan_send(struct chan_ctx *pctx, void *pdata);
/*
* \brief          读取数据
* \param pdata    读取到的数据
* \return         ERR_OK 成功
*/
int32_t chan_recv(struct chan_ctx *pctx, void **pdata);
/*
* \brief          数据总数
* \return         数据总数
*/
int32_t chan_size(struct chan_ctx *pctx);
/*
* \brief          是否可读
* \return         ERR_OK 有数据
*/
int32_t chan_canrecv(struct chan_ctx *pctx);
/*
* \brief          是否可写
* \return         ERR_OK 可写
*/
int32_t chan_cansend(struct chan_ctx *pctx);
/*
* \brief             随机选取一可读写的channel来执行读写,阻塞的不支持
* \param precv       读cchan
* \param irecv_count 读cchan数量
* \param precv_out   读到的数据
* \param psend       写cchan
* \param isend_count 写cchan数量
* \param psend_msgs  每个cchan需要发送的数据
* \return            ERR_FAILED 失败
* \return            cchan 下标
*/
int32_t chan_select(struct chan_ctx *precv[], const int32_t irecv_count, void **precv_out,
    struct chan_ctx *psend[], const int32_t isend_count, void *psend_msgs[]);

#endif//CHAN_H_
