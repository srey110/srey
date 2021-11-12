#ifndef QUEUE_H_
#define QUEUE_H_

#include "macro.h"

struct queue_ctx
{
    int32_t size;
    int32_t next;
    int32_t capacity;
    void **data;
};
/*
* \brief          初始化
*/
static inline void queue_init(struct queue_ctx *pctx, const int32_t icapacity)
{
    ASSERTAB((icapacity > 0 && icapacity <= INT_MAX / (int32_t)sizeof(void*)), "capacity too large");
    pctx->size = 0;
    pctx->next = 0;
    pctx->capacity = ROUND_UP(icapacity, 2);
    pctx->data = MALLOC(sizeof(void*) * pctx->capacity);
    ASSERTAB(NULL != pctx->data, ERRSTR_MEMORY);
};
/*
* \brief          释放
*/
static inline void queue_free(struct queue_ctx *pctx)
{
    FREE(pctx->data);
};
/*
* \brief          扩容
*/
static inline void queue_expand(struct queue_ctx *pctx)
{
    int32_t inewcap = pctx->capacity * 2;
    ASSERTAB(inewcap <= INT_MAX / (int32_t)sizeof(void*), "capacity too large");
    void **pnew = MALLOC(sizeof(void*) * inewcap);
    if (NULL == pnew)
    {
        PRINTF("%s", ERRSTR_MEMORY);
        return;
    }
    if (0 == pctx->size)
    {
        FREE(pctx->data);
        pctx->next = 0;
        pctx->data = pnew;
        pctx->capacity = inewcap;
        return;
    }
    if (pctx->next + pctx->size <= pctx->capacity)
    {
        memcpy(pnew, pctx->data + pctx->next, pctx->size * sizeof(void*));
    }
    else
    {
        memcpy(pnew, pctx->data + pctx->next, (pctx->capacity - pctx->next) * sizeof(void*));
        memcpy(pnew + (pctx->capacity - pctx->next), pctx->data, 
            (pctx->size - (pctx->capacity - pctx->next)) * sizeof(void*));
    }
    FREE(pctx->data);
    pctx->next = 0;
    pctx->data = pnew;
    pctx->capacity = inewcap;
}
/*
* \brief          尝试扩容
*/
static inline void queue_tryexpand(struct queue_ctx *pctx)
{
    if (pctx->size == pctx->capacity)
    {
        queue_expand(pctx);
    }
}
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
