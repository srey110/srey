#ifndef QUEUE_H_
#define QUEUE_H_

#include "structs.h"

struct queue_ctx
{
    int32_t size;
    int32_t next;
    int32_t capacity;
    int32_t initcap;
    struct message_ctx *msg;
};

/*
* \brief          初始化
*/
void queue_init(struct queue_ctx *pctx, const int32_t icapacity);
/*
* \brief          释放
*/
void queue_free(struct queue_ctx *pctx);
/*
* \brief          扩容
*/
void queue_expand(struct queue_ctx *pctx);
/*
* \brief          添加数据
* \param pval     需要添加的数据
* \return         ERR_OK 成功
*/
int32_t queue_push(struct queue_ctx *pctx, struct message_ctx *pval);
/*
* \brief          弹出一数据
* \return         NULL 无数据
* \return         ERR_OK 有数据
*/
int32_t queue_pop(struct queue_ctx *pctx, struct message_ctx *pval);
int32_t queue_size(struct queue_ctx *pctx);
int32_t queue_cap(struct queue_ctx *pctx);

#endif//QUEUE_H_
