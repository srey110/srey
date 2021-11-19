#ifndef TW_H_
#define TW_H_

#include "structs.h"
#include "mutex.h"

#define TVN_BITS (6)
#define TVR_BITS (8)
#define TVN_SIZE (1 << TVN_BITS)
#define TVR_SIZE (1 << TVR_BITS)
#define TVN_MASK (TVN_SIZE - 1)
#define TVR_MASK (TVR_SIZE - 1)
#define INDEX(N) ((pctx->jiffies >> (TVR_BITS + (N) * TVN_BITS)) & TVN_MASK)
struct tw_node_ctx
{
    struct tw_node_ctx *next;
    void(*tw_cb)(struct ud_ctx *);//回调函数
    uint32_t timeout;//超时时间
    uint32_t expires;
    struct ud_ctx ud;//用户数据
};
struct tw_slot_ctx
{
    struct tw_node_ctx *head;
    struct tw_node_ctx *tail;
};
struct tw_ctx
{
    uint32_t jiffies;
    mutex_ctx lockreq;
    struct tw_slot_ctx reqadd;
    struct tw_slot_ctx tv1[TVR_SIZE];
    struct tw_slot_ctx tv2[TVN_SIZE];
    struct tw_slot_ctx tv3[TVN_SIZE];
    struct tw_slot_ctx tv4[TVN_SIZE];
    struct tw_slot_ctx tv5[TVN_SIZE];
};
/*
* \brief               初始化
* \param  pchan        wot_add用
* \param  uicurtick    当前tick
*/
static inline void tw_init(struct tw_ctx *pctx, const uint32_t uicurtick)
{
    ZERO(pctx->tv1, sizeof(pctx->tv1));
    ZERO(pctx->tv2, sizeof(pctx->tv2));
    ZERO(pctx->tv3, sizeof(pctx->tv3));
    ZERO(pctx->tv4, sizeof(pctx->tv4));
    ZERO(pctx->tv5, sizeof(pctx->tv5));
    pctx->jiffies = uicurtick;
    pctx->reqadd.head = pctx->reqadd.tail = NULL;
    mutex_init(&pctx->lockreq);
};
static inline void _free_slot(struct tw_slot_ctx *pslot, const size_t uilens)
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
};
/*
* \brief          释放
*/
static inline void tw_free(struct tw_ctx *pctx)
{
    _free_slot(pctx->tv1, TVR_SIZE);
    _free_slot(pctx->tv2, TVN_SIZE);
    _free_slot(pctx->tv3, TVN_SIZE);
    _free_slot(pctx->tv4, TVN_SIZE);
    _free_slot(pctx->tv5, TVN_SIZE);
    _free_slot(&pctx->reqadd, 1);
    mutex_free(&pctx->lockreq);
};
static inline void _insert(struct tw_slot_ctx *pslot, struct tw_node_ctx *pnode)
{
    if (NULL == pslot->head)
    {
        pslot->head = pslot->tail = pnode;
        return;
    }
    pslot->tail->next = pnode;
    pslot->tail = pnode;
};
/*
* \brief               添加超时事件
* \param  uitimeout    超时时间
* \param  tw_cb        回调函数
* \param  pud          用户数据 
*/
static inline void tw_add(struct tw_ctx *pctx, const uint32_t uitimeout,
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
        pnode->ud = *pud;
    }
    else
    {
        pnode->ud.id = 0;
        pnode->ud.handle = 0;
    }
    pnode->timeout = uitimeout;
    pnode->tw_cb = tw_cb;
    pnode->next = NULL;    
    mutex_lock(&pctx->lockreq);
    _insert(&pctx->reqadd, pnode);
    mutex_unlock(&pctx->lockreq);
};
static inline struct tw_slot_ctx *_getslot(struct tw_ctx *pctx, struct tw_node_ctx *pnode)
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
};
static inline void _clear(struct tw_slot_ctx *pslot)
{
    pslot->head = pslot->tail = NULL;
};
static inline uint32_t _cascade(struct tw_ctx *pctx, struct tw_slot_ctx *pslot, const uint32_t uindex)
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
};
static inline void _run(struct tw_ctx *pctx)
{
    //调整
    uint32_t ulidx = pctx->jiffies & TVR_MASK;
    if (!ulidx
        && (!_cascade(pctx, pctx->tv2, INDEX(0)))
        && (!_cascade(pctx, pctx->tv3, INDEX(1)))
        && (!_cascade(pctx, pctx->tv4, INDEX(2))))
    {
        _cascade(pctx, pctx->tv5, INDEX(3));
    }

    ++pctx->jiffies;

    //执行
    struct tw_node_ctx *pnext, *pnode = pctx->tv1[ulidx].head;
    while (NULL != pnode)
    {
        pnext = pnode->next;
        pnode->tw_cb(&pnode->ud);
        FREE(pnode);
        pnode = pnext;
    }
    _clear(&pctx->tv1[ulidx]);
};
/*
* \brief          执行时间轮
*/
static inline void tw_run(struct tw_ctx *pctx, const uint32_t uicurtick)
{
    struct tw_node_ctx *pnext, *pnode;
    mutex_lock(&pctx->lockreq);
    pnode = pctx->reqadd.head;
    while (NULL != pnode)
    {
        pnext = pnode->next;
        pnode->next = NULL;
        pnode->expires = uicurtick + pnode->timeout;
        _insert(_getslot(pctx, pnode), pnode);
        pnode = pnext;
    }
    _clear(&pctx->reqadd);
    mutex_unlock(&pctx->lockreq);

    while (pctx->jiffies <= uicurtick)
    {
        _run(pctx);
    }
};

#endif//TW_H_
