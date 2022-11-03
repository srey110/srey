#ifndef TW_H_
#define TW_H_

#include "mutex.h"
#include "thread.h"
#include "timer.h"

#define TVN_BITS (6)
#define TVR_BITS (8)
#define TVN_SIZE (1 << TVN_BITS)
#define TVR_SIZE (1 << TVR_BITS)
#define TVN_MASK (TVN_SIZE - 1)
#define TVR_MASK (TVR_SIZE - 1)
#define INDEX(N) ((ctx->jiffies >> (TVR_BITS + (N) * TVN_BITS)) & TVN_MASK)

typedef struct tw_node_ctx
{
    struct tw_node_ctx *next;
    void(*tw_cb)(void *);//回调函数
    uint32_t expires;
    void *ud;//用户数据
}tw_node_ctx;
typedef struct tw_slot_ctx
{
    tw_node_ctx *head;
    tw_node_ctx *tail;
}tw_slot_ctx;
typedef struct tw_ctx
{
    volatile int32_t exit;
    uint32_t jiffies;
    mutex_ctx lockreq;
    thread_ctx thread;
    timer_ctx timer;
    tw_slot_ctx reqadd;
    tw_slot_ctx tv1[TVR_SIZE];
    tw_slot_ctx tv2[TVN_SIZE];
    tw_slot_ctx tv3[TVN_SIZE];
    tw_slot_ctx tv4[TVN_SIZE];
    tw_slot_ctx tv5[TVN_SIZE];
}tw_ctx;

void tw_init(tw_ctx *ctx);
void tw_free(tw_ctx *ctx);
void tw_add(tw_ctx *ctx, const uint32_t timeout, void(*tw_cb)(void *), void *ud);

#endif//TW_H_
