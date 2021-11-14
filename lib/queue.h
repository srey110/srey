#ifndef QUEUE_H_
#define QUEUE_H_

#include "macro.h"

struct queue_ctx
{
    int32_t size;
    int32_t next;
    int32_t capacity;
    int32_t initcap;
    void **data;
};
/*
* \brief          初始化
*/
static inline void queue_init(struct queue_ctx *pctx, const int32_t icapacity)
{
    ASSERTAB((icapacity > 0 && icapacity <= INT_MAX), "capacity too large");
    pctx->size = 0;
    pctx->next = 0;
    pctx->capacity = ROUND_UP(icapacity, 2);
    pctx->initcap = pctx->capacity;
    pctx->data = (void **)MALLOC(sizeof(void*) * pctx->capacity);
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
* \brief          扩容,收缩
*/
static inline void queue_expand(struct queue_ctx *pctx)
{
    //收缩
    if (0 == pctx->size
        && pctx->capacity > pctx->initcap)
    {
        FREE(pctx->data);
        pctx->data = (void **)MALLOC(sizeof(void*) * pctx->initcap);
        ASSERTAB(NULL != pctx->data, ERRSTR_MEMORY);
        pctx->capacity = pctx->initcap;
        return;
    }
    if (pctx->size < pctx->capacity)
    {
        return;
    }

    int32_t inewcap = pctx->capacity * 2;
    ASSERTAB((inewcap > 0 && inewcap <= INT_MAX), "capacity too large");
    void **pnew = (void **)MALLOC(sizeof(void*) * inewcap);
    ASSERTAB(NULL != pnew, ERRSTR_MEMORY);

    for (int32_t i = 0; i < pctx->capacity; i++)
    {
        pnew[i] = pctx->data[(pctx->next + i) % pctx->capacity];
    }
    FREE(pctx->data);
    pctx->next = 0;
    pctx->data = pnew;
    pctx->capacity = inewcap;
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
static inline int32_t queue_size(struct queue_ctx *pctx)
{
    return pctx->size;
};
static inline int32_t queue_cap(struct queue_ctx *pctx)
{
    return pctx->capacity;
};

#endif//QUEUE_H_
