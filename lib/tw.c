#include "tw.h"

void _free_slot(struct tw_slot_ctx *pslot, const size_t uilens)
{
    struct tw_node_ctx *pnode, *pdel;
    for (size_t i = 0; i < uilens; i++)
    {
        pnode = pslot[i].head;
        while (NULL != pnode)
        {
            pdel = pnode;
            pnode = pnode->next;
            FREE(pdel);
        }
    }
}
void tw_free(struct tw_ctx *pctx)
{
    pctx->exit = 1;
    thread_join(&pctx->thread);
    _free_slot(pctx->tv1, TVR_SIZE);
    _free_slot(pctx->tv2, TVN_SIZE);
    _free_slot(pctx->tv3, TVN_SIZE);
    _free_slot(pctx->tv4, TVN_SIZE);
    _free_slot(pctx->tv5, TVN_SIZE);
    _free_slot(&pctx->reqadd, 1);
    mutex_free(&pctx->lockreq);
}
void _insert(struct tw_slot_ctx *pslot, struct tw_node_ctx *pnode)
{
    if (NULL == pslot->head)
    {
        pslot->head = pslot->tail = pnode;
        return;
    }
    pslot->tail->next = pnode;
    pslot->tail = pnode;
}
void tw_add(struct tw_ctx *pctx, const uint32_t uitimeout,
    void(*tw_cb)(struct ud_ctx *), struct ud_ctx *pud)
{
    if (0 == uitimeout)
    {
        tw_cb(pud);
        return;
    }
    struct tw_node_ctx *pnode = (struct tw_node_ctx *)MALLOC(sizeof(struct tw_node_ctx));
    if (NULL == pnode)
    {
        PRINTF("%s", ERRSTR_MEMORY);
        return;
    }
    if (NULL != pud)
    {
        memcpy(&pnode->ud, pud, sizeof(struct ud_ctx));
    }
    else
    {
        ZERO(&pnode->ud, sizeof(struct ud_ctx));
    }
    pnode->timeout = uitimeout;
    pnode->tw_cb = tw_cb;
    pnode->next = NULL;
    mutex_lock(&pctx->lockreq);
    _insert(&pctx->reqadd, pnode);
    mutex_unlock(&pctx->lockreq);
}
struct tw_slot_ctx *_getslot(struct tw_ctx *pctx, struct tw_node_ctx *pnode)
{
    struct tw_slot_ctx *pslot;
    uint32_t ulidx = pnode->expires - pctx->jiffies;
    if ((int32_t)ulidx < 0)
    {
        pslot = &pctx->tv1[(pctx->jiffies & TVR_MASK)];
    }
    else if (ulidx < TVR_SIZE)
    {
        pslot = &pctx->tv1[(pnode->expires & TVR_MASK)];
    }
    else if (ulidx < 1 << (TVR_BITS + TVN_BITS))
    {
        pslot = &pctx->tv2[((pnode->expires >> TVR_BITS) & TVN_MASK)];
    }
    else if (ulidx < 1 << (TVR_BITS + 2 * TVN_BITS))
    {
        pslot = &pctx->tv3[((pnode->expires >> (TVR_BITS + TVN_BITS)) & TVN_MASK)];
    }
    else if (ulidx < 1 << (TVR_BITS + 3 * TVN_BITS))
    {
        pslot = &pctx->tv4[((pnode->expires >> (TVR_BITS + 2 * TVN_BITS)) & TVN_MASK)];
    }
    else
    {
        if (ulidx > 0xffffffffUL)
        {
            ulidx = 0xffffffffUL;
            pnode->expires = ulidx + pctx->jiffies;
        }
        pslot = &pctx->tv5[((pnode->expires >> (TVR_BITS + 3 * TVN_BITS)) & TVN_MASK)];
    }

    return pslot;
}
void _clear(struct tw_slot_ctx *pslot)
{
    pslot->head = pslot->tail = NULL;
}
uint32_t _cascade(struct tw_ctx *pctx, struct tw_slot_ctx *pslot, const uint32_t uindex)
{
    struct tw_node_ctx *pnext, *pnode = pslot[uindex].head;
    while (NULL != pnode)
    {
        pnext = pnode->next;
        pnode->next = NULL;
        _insert(_getslot(pctx, pnode), pnode);
        pnode = pnext;
    }
    _clear(&pslot[uindex]);

    return uindex;
}
void _run(struct tw_ctx *pctx)
{
    //µ÷Õû
    uint32_t ulidx = pctx->jiffies & TVR_MASK;
    if (!ulidx
        && (!_cascade(pctx, pctx->tv2, INDEX(0)))
        && (!_cascade(pctx, pctx->tv3, INDEX(1)))
        && (!_cascade(pctx, pctx->tv4, INDEX(2))))
    {
        _cascade(pctx, pctx->tv5, INDEX(3));
    }

    ++pctx->jiffies;

    //Ö´ÐÐ
    struct tw_node_ctx *pnext, *pnode = pctx->tv1[ulidx].head;
    while (NULL != pnode)
    {
        pnext = pnode->next;
        pnode->tw_cb(&pnode->ud);
        FREE(pnode);
        pnode = pnext;
    }
    _clear(&pctx->tv1[ulidx]);
}
void _loop(void *pparam)
{
    uint32_t curtick = 0;
    const uint32_t accuracy = 1000 * 1000;
    struct tw_node_ctx *pnext, *pnode;
    struct tw_ctx *pctx = (struct tw_ctx *)pparam;
    pctx->jiffies = (uint32_t)(timer_nanosec(&pctx->timer) / accuracy);
    while (0 == pctx->exit)
    {
        curtick = (uint32_t)(timer_nanosec(&pctx->timer) / accuracy);
        mutex_lock(&pctx->lockreq);
        pnode = pctx->reqadd.head;
        while (NULL != pnode)
        {
            pnext = pnode->next;
            pnode->next = NULL;
            pnode->expires = curtick + pnode->timeout;
            _insert(_getslot(pctx, pnode), pnode);
            pnode = pnext;
        }
        _clear(&pctx->reqadd);
        mutex_unlock(&pctx->lockreq);

        while (pctx->jiffies <= curtick)
        {
            _run(pctx);
        }
        USLEEP(10);
    }
}
void tw_init(struct tw_ctx *pctx)
{
    pctx->exit = 0;
    pctx->jiffies = 0;
    mutex_init(&pctx->lockreq);
    thread_init(&pctx->thread);
    timer_init(&pctx->timer);
    pctx->reqadd.head = pctx->reqadd.tail = NULL;
    ZERO(pctx->tv1, sizeof(pctx->tv1));
    ZERO(pctx->tv2, sizeof(pctx->tv2));
    ZERO(pctx->tv3, sizeof(pctx->tv3));
    ZERO(pctx->tv4, sizeof(pctx->tv4));
    ZERO(pctx->tv5, sizeof(pctx->tv5));

    thread_creat(&pctx->thread, _loop, pctx);
}
