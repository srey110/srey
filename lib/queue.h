#ifndef QUEUE_H_
#define QUEUE_H_

#include "macro.h"

typedef struct queue_ctx
{
    int32_t size;
    int32_t next;
    int32_t capacity;
    void **data;
}queue_ctx;
/*
* \brief          初始化
*/
static inline void queue_init(struct queue_ctx *pctx, const int32_t icapacity)
{
    ASSERTAB((icapacity > 0 && icapacity <= INT_MAX / (int32_t)sizeof(void*)), "capacity too large");
    pctx->size = 0;
    pctx->next = 0;
    pctx->capacity = icapacity;
    pctx->data = (void **)MALLOC(sizeof(void*) * pctx->capacity);
    ASSERTAB(NULL != pctx->data, ERRSTR_MEMORY);
};
/*
* \brief          释放
*/
static inline void queue_free(struct queue_ctx *pctx)
{
    SAFE_FREE(pctx->data);
};
/*
* \brief          添加数据
* \param pval     需要添加的数据
* \return         ERR_OK 成功
*/
static inline int32_t queue_push(struct queue_ctx *pctx, void *pval)
{
    if (pctx->size >= pctx->capacity)
    {
        return ERR_FAILED;
    }

    int32_t ipos = pctx->next + pctx->size;
    if (ipos >= pctx->capacity)
    {
        ipos -= pctx->capacity;
    }
    pctx->data[ipos] = pval;
    pctx->size++;

    return ERR_OK;
};
/*
* \brief          弹出一数据
* \return         NULL 无数据
* \return         数据
*/
static inline void *queue_pop(struct queue_ctx *pctx)
{
    void *pval = NULL;

    if (pctx->size > 0)
    {
        pval = pctx->data[pctx->next];
        pctx->next++;
        pctx->size--;
        if (pctx->next >= pctx->capacity)
        {
            pctx->next -= pctx->capacity;
        }
    }

    return pval;
};
/*
* \brief          获取第一个数据
* \return         NULL 无数据
* \return         数据
*/
static inline void *queue_peek(struct queue_ctx *pctx)
{
    return pctx->size > 0 ? pctx->data[pctx->next] : NULL;
};
static inline int32_t queue_size(struct queue_ctx *pctx)
{
    return pctx->size;
};
static inline int32_t queue_cap(struct queue_ctx *pctx)
{
    return pctx->capacity;
};

#endif//QUEUE_H_
