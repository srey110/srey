#ifndef TW_H_
#define TW_H_

#include "mutex.h"
#include "thread.h"
#include "structs.h"
#include "timer.h"

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
    volatile int32_t exit;
    uint32_t jiffies;
    mutex_ctx lockreq;
    struct thread_ctx thread;
    struct timer_ctx timer;
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
void tw_init(struct tw_ctx *pctx);
/*
* \brief          释放
*/
void tw_free(struct tw_ctx *pctx);

/*
* \brief               添加超时事件
* \param  uitimeout    超时时间
* \param  tw_cb        回调函数
* \param  pud          用户数据 
*/
void tw_add(struct tw_ctx *pctx, const uint32_t uitimeout,
    void(*tw_cb)(struct ud_ctx *), struct ud_ctx *pud);

#endif//TW_H_
