#ifndef WOT_H_
#define WOT_H_

#include "evtype.h"
#include "chan.h"

#define TVN_BITS (6)
#define TVR_BITS (8)
#define TVN_SIZE (1 << TVN_BITS)
#define TVR_SIZE (1 << TVR_BITS)
#define TVN_MASK (TVN_SIZE - 1)
#define TVR_MASK (TVR_SIZE - 1)
typedef struct twnode_ctx
{
    struct ev_ctx ev;
    u_long expires;         //超时时间
    struct chan_ctx *chan;  //接收消息的chan
    struct twnode_ctx *next;
}twnode_ctx;
typedef struct twslot_ctx
{
    struct twnode_ctx *head;
    struct twnode_ctx *tail;
}twslot_ctx;
struct wot_ctx
{
    u_long jiffies;
    struct chan_ctx chan;
    struct twslot_ctx tv1[TVR_SIZE];
    struct twslot_ctx tv2[TVN_SIZE];
    struct twslot_ctx tv3[TVN_SIZE];
    struct twslot_ctx tv4[TVN_SIZE];
    struct twslot_ctx tv5[TVN_SIZE];
}wot_ctx;
/*
* \brief               初始化
* \param  uicurtick    当前tick
*/
static inline void wot_init(struct wot_ctx *pctx, const u_long ulcurtick)
{
    ZERO(pctx->tv1, sizeof(pctx->tv1));
    ZERO(pctx->tv2, sizeof(pctx->tv2));
    ZERO(pctx->tv3, sizeof(pctx->tv3));
    ZERO(pctx->tv4, sizeof(pctx->tv4));
    ZERO(pctx->tv5, sizeof(pctx->tv5));
    pctx->jiffies = ulcurtick;
    chan_init(&pctx->chan, ONEK);
};
static inline void _free(struct twslot_ctx *pslot, const size_t uilens)
{
    struct twnode_ctx *pnode, *pdel;
    for (size_t i = 0; i < uilens; i++)
    {
        pnode = pslot[i].head;
        while (NULL != pnode)
        {
            pdel = pnode;
            pnode = pnode->next;
            SAFE_FREE(pdel);
        }
    }
};
/*
* \brief          释放
*/
static inline void wot_free(struct wot_ctx *pctx)
{
    _free(pctx->tv1, TVR_SIZE);
    _free(pctx->tv2, TVN_SIZE);
    _free(pctx->tv3, TVN_SIZE);
    _free(pctx->tv4, TVN_SIZE);
    _free(pctx->tv5, TVN_SIZE);
    chan_free(&pctx->chan);
};
/*
* \brief               添加一超时事件
* \param  pchan        接收超时消息
* \param  ulcurtick    当前tick
* \param  uitick       超时时间  多少个tick
* \param  pdata        用户数据
* \return              ERR_OK 成功
*/
static inline int32_t wot_add(struct wot_ctx *pctx, struct chan_ctx *pchan,
    const u_long ulcurtick, const uint32_t uitick, const void *pdata)
{
    struct twnode_ctx *pnode = (struct twnode_ctx *)MALLOC(sizeof(struct twnode_ctx));
    ASSERTAB(NULL != pnode, ERRSTR_MEMORY);
    pnode->ev.data = (void*)pdata;
    pnode->ev.code = ERR_OK;
    pnode->ev.evtype = EV_TIME;
    pnode->chan = pchan;
    pnode->expires = ulcurtick + uitick;
    pnode->next = NULL;

    return chan_send(&pctx->chan, (void *)pnode);
};
static inline void _insert(struct twslot_ctx *pslot, struct twnode_ctx *pnode)
{
    if (NULL == pslot->head)
    {
        pslot->head = pslot->tail = pnode;
        return;
    }

    pslot->tail->next = pnode;
    pslot->tail = pnode;
};
static inline struct twslot_ctx *_getslot(struct wot_ctx *pctx, struct twnode_ctx *pnode)
{
    struct twslot_ctx *pslot;
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
static inline void _clear(struct twslot_ctx *pslot)
{
    pslot->head = pslot->tail = NULL;
};
static inline u_long _cascade(struct wot_ctx *pctx, struct twslot_ctx *pslot, const u_long ulindex)
{
    struct twnode_ctx *pnext;
    struct twnode_ctx *pnode = pslot[ulindex].head;
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
#define INDEX(N) ((pctx->jiffies >> (TVR_BITS + (N) * TVN_BITS)) & TVN_MASK)
static inline void _run(struct wot_ctx *pctx)
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
    struct twnode_ctx *pnext;
    struct twnode_ctx *pnode = pctx->tv1[ulidx].head;
    while (NULL != pnode)
    {
        pnext = pnode->next;
        //将超时信息发出去,发送出去后pnode有可能已经被释放，所以先取的next
        if (ERR_OK != chan_send(pnode->chan, (void*)&pnode->ev))
        {
            SAFE_FREE(pnode);
            PRINTF("%s", "time wheel chan send failed.");
        }
        pnode = pnext;
    }

    _clear(&pctx->tv1[ulidx]);
};
/*
* \brief          执行时间轮
*/
static inline void wot_run(struct wot_ctx *pctx, const u_long ulcurtick)
{
    void *ptmp;
    struct twnode_ctx *pnode;
    while (ERR_OK == chan_canrecv(&pctx->chan))
    {
        ptmp = NULL;
        if (ERR_OK == chan_recv(&pctx->chan, &ptmp))
        {
            pnode = (struct twnode_ctx *)ptmp;
            _insert(_getslot(pctx, pnode), pnode);
        }
    }

    while (pctx->jiffies <= ulcurtick)
    {
        _run(pctx);
    }
};

#endif//WOT_H_
