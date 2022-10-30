#include "queue.h"

void queue_init(struct queue_ctx *pctx, const int32_t icapacity)
{
    ASSERTAB((icapacity > 0 && icapacity <= INT_MAX), "capacity too large");
    pctx->size = 0;
    pctx->next = 0;
    pctx->capacity = ROUND_UP(icapacity, 2);
    pctx->initcap = pctx->capacity;
    pctx->msg = (struct message_ctx *)MALLOC(sizeof(struct message_ctx) * pctx->capacity);
    ASSERTAB(NULL != pctx->msg, ERRSTR_MEMORY);
}
void queue_free(struct queue_ctx *pctx)
{
    FREE(pctx->msg);
}
void queue_expand(struct queue_ctx *pctx)
{
    if (pctx->size < pctx->capacity)
    {
        return;
    }
    //À©ÈÝ
    int32_t inewcap = pctx->capacity * 2;
    ASSERTAB((inewcap > 0 && inewcap <= INT_MAX), "capacity too large");
    struct message_ctx *pnew = (struct message_ctx *)MALLOC(sizeof(struct message_ctx) * inewcap);
    ASSERTAB(NULL != pnew, ERRSTR_MEMORY);

    for (int32_t i = 0; i < pctx->capacity; i++)
    {
        memcpy(&pnew[i], &pctx->msg[(pctx->next + i) % pctx->capacity], sizeof(struct message_ctx));
    }
    FREE(pctx->msg);
    pctx->next = 0;
    pctx->msg = pnew;
    pctx->capacity = inewcap;
}
int32_t queue_push(struct queue_ctx *pctx, struct message_ctx *pval)
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
    memcpy(&pctx->msg[ipos], pval, sizeof(struct message_ctx));
    pctx->size++;

    return ERR_OK;
}
int32_t queue_pop(struct queue_ctx *pctx, struct message_ctx *pval)
{
    if (0 == pctx->size)
    {
        return ERR_FAILED;
    }

    memcpy(pval, &pctx->msg[pctx->next], sizeof(struct message_ctx));
    pctx->next++;
    pctx->size--;
    if (pctx->next >= pctx->capacity)
    {
        pctx->next -= pctx->capacity;
    }

    return ERR_OK;
}
int32_t queue_size(struct queue_ctx *pctx)
{
    return pctx->size;
}
int32_t queue_cap(struct queue_ctx *pctx)
{
    return pctx->capacity;
}
