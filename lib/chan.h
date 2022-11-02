#ifndef CHAN_H_
#define CHAN_H_

#include "queue.h"
#include "mutex.h"
#include "cond.h"
#include "structs.h"

QUEUE_DECL(struct message_ctx, qu_chanmsg);
struct chan_ctx
{
    int32_t closed;
    int32_t rwaiting;
    int32_t wwaiting;
    mutex_ctx mmutex;//读写信号锁
    cond_ctx rcond;
    cond_ctx wcond;
    qu_chanmsg queue;
};
/*
* \brief             初始化
* \param icapacity   队列容量
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
* \brief          写入数据
* \param pdata    待写入的数据
* \return         ERR_OK 成功
*/
int32_t chan_send(struct chan_ctx *pctx, struct message_ctx *pdata);
int32_t chan_trysend(struct chan_ctx *pctx, struct message_ctx *pdata);
/*
* \brief          读取数据
* \param pdata    读取到的数据
* \return         ERR_OK 成功
*/
int32_t chan_recv(struct chan_ctx *pctx, struct message_ctx *pdata);
int32_t chan_tryrecv(struct chan_ctx *pctx, struct message_ctx *pdata);

/*
* \brief          数据总数
* \return         数据总数
*/
int32_t chan_size(struct chan_ctx *pctx);
/*
* \brief          channel是否关闭
* \return         0 未关闭
*/
int32_t chan_closed(struct chan_ctx *pctx);

#endif//CHAN_H_
