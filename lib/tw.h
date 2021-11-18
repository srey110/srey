#ifndef TW_H_
#define TW_H_

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
    void *data;//用户数据
    void(*tw_cb)(struct tw_node_ctx *, void *);//回调函数
    int32_t removed;
    int32_t repeat;//循环次数  -1 永远 其他  次数
    u_long timeout;//超时时间
    u_long expires;
    struct tw_node_ctx *next;
};
struct tw_slot_ctx
{
    struct tw_node_ctx *head;
    struct tw_node_ctx *tail;
};
struct tw_ctx
{
    u_long jiffies;
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
static inline void tw_init(struct tw_ctx *pctx, const u_long ulcurtick)
{
    ZERO(pctx->tv1, sizeof(pctx->tv1));
    ZERO(pctx->tv2, sizeof(pctx->tv2));
    ZERO(pctx->tv3, sizeof(pctx->tv3));
    ZERO(pctx->tv4, sizeof(pctx->tv4));
    ZERO(pctx->tv5, sizeof(pctx->tv5));
    pctx->jiffies = ulcurtick;
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
* \param  ultimeout    超时时间
* \param  irepeat      循环次数  -1：永远 其他：次数,
* \param  tw_cb        回调函数
* \param  free_data    用户数据 释放函数,用于程序退出时
* \param  pdata        用户数据 
*/
static inline struct tw_node_ctx *tw_add(struct tw_ctx *pctx, const u_long ultimeout, const int32_t irepeat,
    void(*tw_cb)(struct tw_node_ctx *, void *), void *pdata)
{
    ASSERTAB(-1 == irepeat || irepeat > 0, "param repeat error.");
    struct tw_node_ctx *pnode = (struct tw_node_ctx *)MALLOC(sizeof(struct tw_node_ctx));
    ASSERTAB(NULL != pnode, ERRSTR_MEMORY);
    pnode->data = pdata;
    pnode->timeout = ultimeout;
    pnode->repeat = irepeat;
    pnode->removed = 0;
    pnode->tw_cb = tw_cb;
    pnode->next = NULL;
    
    mutex_lock(&pctx->lockreq);
    _insert(&pctx->reqadd, pnode);
    mutex_unlock(&pctx->lockreq);

    return pnode;
};
/*
* \brief               设置循环次数
* \param  irepeat      循环次数
*/
static inline void tw_repeat(struct tw_node_ctx *pnode, const int32_t irepeat)
{
    pnode->repeat = irepeat + 1;
};
/*
* \brief               删除
*/
static inline void tw_remove(struct tw_node_ctx *pnode)
{
    pnode->removed = 1;
};
/*
* \brief               设置用户数据
*/
static inline void tw_udata(struct tw_node_ctx *pnode, void *pdata)
{
    pnode->data = pdata;
};
/*
* \brief               超时
*/
static inline u_long tw_timeout_get(struct tw_node_ctx *pnode)
{
    return pnode->timeout;
};
static inline void tw_timeout_set(struct tw_node_ctx *pnode, const u_long ultimeout)
{
    pnode->timeout = ultimeout;
};
static inline struct tw_slot_ctx *_getslot(struct tw_ctx *pctx, struct tw_node_ctx *pnode)
{
    struct tw_slot_ctx *pslot;
    u_long ulidx = pnode->expires - pctx->jiffies;
    if ((long)ulidx < 0)
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
static inline u_long _cascade(struct tw_ctx *pctx, struct tw_slot_ctx *pslot, const u_long ulindex)
{
    struct tw_node_ctx *pnext, *pnode = pslot[ulindex].head;
    while (NULL != pnode)
    {
        pnext = pnode->next;
        pnode->next = NULL;
        _insert(_getslot(pctx, pnode), pnode);
        pnode = pnext;
    }
    _clear(&pslot[ulindex]);

    return ulindex;
};
static inline void _run(struct tw_ctx *pctx)
{
    //调整
    u_long ulidx = pctx->jiffies & TVR_MASK;
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
        pnode->next = NULL;
        if (0 != pnode->removed)
        {
            FREE(pnode);
            pnode = pnext;
            continue;
        }

        pnode->tw_cb(pnode, pnode->data);
        if (0 != pnode->removed)
        {
            FREE(pnode);
            pnode = pnext;
            continue;
        }
        if (pnode->repeat > 0)
        {
            pnode->repeat--;
        }
        if (0 == pnode->repeat)
        {
            FREE(pnode);
            pnode = pnext;
            continue;
        }
        pnode->expires = pctx->jiffies + pnode->timeout;
        _insert(_getslot(pctx, pnode), pnode);
        pnode = pnext;
    }
    _clear(&pctx->tv1[ulidx]);
};
/*
* \brief          执行时间轮
*/
static inline void tw_run(struct tw_ctx *pctx, const u_long ulcurtick)
{
    struct tw_node_ctx *pnext, *pnode;
    mutex_lock(&pctx->lockreq);
    pnode = pctx->reqadd.head;
    while (NULL != pnode)
    {
        pnext = pnode->next;
        pnode->next = NULL;
        pnode->expires = ulcurtick + pnode->timeout;
        _insert(_getslot(pctx, pnode), pnode);
        pnode = pnext;
    }
    _clear(&pctx->reqadd);
    mutex_unlock(&pctx->lockreq);

    while (pctx->jiffies <= ulcurtick)
    {
        _run(pctx);
    }
};

#endif//TW_H_
