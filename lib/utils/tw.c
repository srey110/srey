#include "utils/tw.h"

// 从节点池无锁出队；池为空时退化为 MALLOC
static tw_node_ctx *_node_alloc(tw_ctx *ctx) {
    tw_node_ctx *node = (tw_node_ctx *)mspc_pop(&ctx->node_pool);
    if (NULL == node) {
        MALLOC(node, sizeof(tw_node_ctx));
    }
    return node;
}
// 将节点无锁归还到池；池满时直接 FREE
static void _node_release(tw_ctx *ctx, tw_node_ctx *node) {
    if (ERR_OK != mspc_push(&ctx->node_pool, node)) {
        FREE(node);
    }
}
// 释放时间轮槽位数组中所有节点，并调用各节点的 _freecb 释放用户数据
static void _free_slot(tw_slot_ctx *slot, const size_t len) {
    tw_node_ctx *pnode, *pdel;
    for (size_t i = 0; i < len; i++) {
        pnode = slot[i].head;
        while (NULL != pnode) {
            pdel = pnode;
            pnode = pnode->next;
            if (NULL != pdel->_freecb) {
                pdel->_freecb(&pdel->ud);
            }
            FREE(pdel);
        }
    }
}
void tw_free(tw_ctx *ctx) {
    ctx->exit = 1;
    /* 唤醒轮线程，让它检查 exit 标志并退出 */
    mutex_lock(&ctx->mu);
    cond_signal(&ctx->cond);
    mutex_unlock(&ctx->mu);
    thread_join(ctx->thtw);
    _free_slot(ctx->tv1, TVR_SIZE);
    _free_slot(ctx->tv2, TVN_SIZE);
    _free_slot(ctx->tv3, TVN_SIZE);
    _free_slot(ctx->tv4, TVN_SIZE);
    _free_slot(ctx->tv5, TVN_SIZE);
    /* 排空 reqadd 无锁队列并释放节点 */
    tw_node_ctx *node;
    while (NULL != (node = (tw_node_ctx *)mspc_pop(&ctx->reqadd))) {
        if (NULL != node->_freecb) {
            node->_freecb(&node->ud);
        }
        FREE(node);
    }
    mspc_free(&ctx->reqadd);
    /* 排空节点池并释放 */
    while (NULL != (node = (tw_node_ctx *)mspc_pop(&ctx->node_pool))) {
        FREE(node);
    }
    mspc_free(&ctx->node_pool);
    cond_free(&ctx->cond);
    mutex_free(&ctx->mu);
}
// 将节点追加到指定槽位的链表尾部
static void _insert(tw_slot_ctx *slot, tw_node_ctx *node) {
    if (NULL == slot->head) {
        slot->head = slot->tail = node;
        return;
    }
    slot->tail->next = node;
    slot->tail = node;
}
void tw_add(tw_ctx *ctx, const uint32_t timeout, tw_cb _cb, free_cb _freecb, ud_cxt *ud) {
    tw_node_ctx *node = _node_alloc(ctx);
    COPY_UD(node->ud, ud);
    node->expires = timer_cur_ms(&ctx->timer) + timeout;
    node->_cb = _cb;
    node->_freecb = _freecb;
    node->next = NULL;
    if (ERR_OK != mspc_push(&ctx->reqadd, node)) {
        /* 队列满（容量 TW_REQADD_CAP）：释放用户数据后归还节点，不挂起调用方 */
        LOG_ERROR("%s", "tw_add: reqadd queue full, timer dropped.");
        if (NULL != node->_freecb) {
            node->_freecb(&node->ud);
        }
        _node_release(ctx, node);
        return;
    }
    /* 仅当标志由 0→1 时（首批新任务）才唤醒轮线程，批量入队后续节点不重复 signal */
    if (ATOMIC_CAS(&ctx->reqadd_pending, 0, 1)) {
        mutex_lock(&ctx->mu);
        cond_signal(&ctx->cond);
        mutex_unlock(&ctx->mu);
    }
}
// 根据节点的到期时间计算应放入 tv1～tv5 中的哪个槽位
static tw_slot_ctx *_getslot(tw_ctx *ctx, tw_node_ctx *node) {
    tw_slot_ctx *slot;
    if (node->expires <= ctx->jiffies) {
        // 已过期：放入当前 jiffies 对应的 tv1 槽，下一次 _run 立即触发
        return &ctx->tv1[(ctx->jiffies & TVR_MASK)];
    }
    uint64_t idx = node->expires - ctx->jiffies;
    if (idx < TVR_SIZE) {
        // tv1：超时在 [1, 255] ms 内，直接按到期时间低 8 位定槽
        slot = &ctx->tv1[(node->expires & TVR_MASK)];
    } else if (idx < 1UL << (TVR_BITS + TVN_BITS)) {
        // tv2：超时在 [256, 16383] ms 内，取到期时间第 8~13 位定槽
        slot = &ctx->tv2[((node->expires >> TVR_BITS) & TVN_MASK)];
    } else if (idx < 1UL << (TVR_BITS + 2 * TVN_BITS)) {
        // tv3：超时在 [16384, 1048575] ms (~17 min) 内，取到期时间第 14~19 位定槽
        slot = &ctx->tv3[((node->expires >> (TVR_BITS + TVN_BITS)) & TVN_MASK)];
    } else if (idx < 1UL << (TVR_BITS + 3 * TVN_BITS)) {
        // tv4：超时在 [1048576, 67108863] ms (~18.6 h) 内，取到期时间第 20~25 位定槽
        slot = &ctx->tv4[((node->expires >> (TVR_BITS + 2 * TVN_BITS)) & TVN_MASK)];
    } else {
        // tv5：超时在 [67108864, 0xffffffff] ms (~49.7 d) 内，超出上限则截断到最大值
        if (idx > 0xffffffffUL) {
            node->expires = ctx->jiffies + 0xffffffffUL;
        }
        slot = &ctx->tv5[((node->expires >> (TVR_BITS + 3 * TVN_BITS)) & TVN_MASK)];
    }
    return slot;
}
// 将高精度槽位中的节点重新分配到低精度槽位（时间轮进位）
static uint32_t _cascade(tw_ctx *ctx, tw_slot_ctx *slot, const uint32_t index) {
    tw_node_ctx *pnext, *pnode = slot[index].head;
    while (NULL != pnode) {
        pnext = pnode->next;
        pnode->next = NULL;
        _insert(_getslot(ctx, pnode), pnode);
        pnode = pnext;
    }
    slot[index].head = slot[index].tail = NULL;
    return index;
}
// 推进一个 jiffie：先做进位 cascade，再执行当前槽位所有到期节点的回调
static void _run(tw_ctx *ctx) {
    //调整
    uint32_t ulidx = (uint32_t)(ctx->jiffies & TVR_MASK);
    if (!ulidx
        && (!_cascade(ctx, ctx->tv2, INDEX(0)))
        && (!_cascade(ctx, ctx->tv3, INDEX(1)))
        && (!_cascade(ctx, ctx->tv4, INDEX(2)))) {
        _cascade(ctx, ctx->tv5, INDEX(3));
    }
    ++ctx->jiffies;
    //执行
    tw_node_ctx *pnext, *pnode = ctx->tv1[ulidx].head;
    while (NULL != pnode) {
        pnext = pnode->next;
        pnode->_cb(&pnode->ud);
        _node_release(ctx, pnode);
        pnode = pnext;
    }
    ctx->tv1[ulidx].head = ctx->tv1[ulidx].tail = NULL;
}
// 时间轮工作线程入口：分发新任务、推进 jiffies、精确睡眠等待下一个到期
static void _loop(void *arg) {
    uint64_t curtick;
    uint32_t sleep_ms;
    tw_node_ctx *node;
    tw_ctx *ctx = (tw_ctx *)arg;
    ctx->jiffies = timer_cur_ms(&ctx->timer);
    while (0 == ctx->exit) {
        /* 1. 将外部通过 tw_add 提交的节点分发到对应槽位（无锁出队）
         *    先排空，再清标志，再二次排空：避免清标志与生产者入队之间的竞态导致漏唤醒 */
        while (NULL != (node = (tw_node_ctx *)mspc_pop(&ctx->reqadd))) {
            _insert(_getslot(ctx, node), node);
        }
        ATOMIC_SET(&ctx->reqadd_pending, 0);
        while (NULL != (node = (tw_node_ctx *)mspc_pop(&ctx->reqadd))) {
            _insert(_getslot(ctx, node), node);
        }
        /* 2. 处理所有已到期的 jiffies */
        curtick = timer_cur_ms(&ctx->timer);
        while (ctx->jiffies <= curtick) {
            _run(ctx);
        }
        /* 3. 精确睡眠直到下一个 jiffy 到期，而非固定 1ms 空转。
         *    sleep_ms = max(1, min(next_jiffy - now, 10))
         *    上限 10ms 保证不会因时钟偏差或系统负载导致长时间错过到期。
         *    tw_add / tw_free 会提前 cond_signal 唤醒。 */
        curtick = timer_cur_ms(&ctx->timer);
        sleep_ms = (ctx->jiffies > curtick) ? (uint32_t)(ctx->jiffies - curtick) : 1;
        if (sleep_ms > 10) {
            sleep_ms = 10;
        }
        mutex_lock(&ctx->mu);
        if (0 == ctx->exit) {
            cond_timedwait(&ctx->cond, &ctx->mu, sleep_ms);
        }
        mutex_unlock(&ctx->mu);
    }
    LOG_INFO("%s", "timewheel thread exited.");
}
void tw_init(tw_ctx *ctx) {
    ctx->exit = 0;
    ctx->jiffies = 0;
    ATOMIC_SET(&ctx->reqadd_pending, 0);
    mutex_init(&ctx->mu);
    cond_init(&ctx->cond);
    timer_init(&ctx->timer);
    mspc_init(&ctx->reqadd, TW_REQADD_CAP);
    mspc_init(&ctx->node_pool, TW_NODE_POOL_MAX);
    ZERO(ctx->tv1, sizeof(ctx->tv1));
    ZERO(ctx->tv2, sizeof(ctx->tv2));
    ZERO(ctx->tv3, sizeof(ctx->tv3));
    ZERO(ctx->tv4, sizeof(ctx->tv4));
    ZERO(ctx->tv5, sizeof(ctx->tv5));
    ctx->thtw = thread_creat(_loop, ctx);
}
