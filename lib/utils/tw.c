#include "utils/tw.h"

#define TW_NODE_POOL_MAX    4 * ONEK    // 节点池上限：超出后直接释放，避免无界增长
#define TW_REQADD_BATCH     128         // reqadd 单次批量出队上限

// 释放时间轮槽位数组中所有节点，并调用各节点的 _freecb 释放用户数据
static void _tw_free_slot(tw_slot_ctx *slot, const size_t len) {
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
    ATOMIC_SET(&ctx->exit, 1);
    /* 唤醒轮线程，让它检查 exit 标志并退出 */
    mutex_lock(&ctx->mu);
    cond_signal(&ctx->cond);
    mutex_unlock(&ctx->mu);
    thread_join(ctx->thtw);
    _tw_free_slot(ctx->tv1, TVR_SIZE);
    _tw_free_slot(ctx->tv2, TVN_SIZE);
    _tw_free_slot(ctx->tv3, TVN_SIZE);
    _tw_free_slot(ctx->tv4, TVN_SIZE);
    _tw_free_slot(ctx->tv5, TVN_SIZE);
    /* 排空 reqadd 队列并释放节点（tw 主线程已 join，单消费者，批量出队） */
    tw_node_ctx *nodes[TW_REQADD_BATCH];
    uint32_t n, i;
    while ((n = fsqu_pop_sc_batch(&ctx->reqadd, nodes, TW_REQADD_BATCH)) > 0) {
        for (i = 0; i < n; i++) {
            if (NULL != nodes[i]->_freecb) {
                nodes[i]->_freecb(&nodes[i]->ud);
            }
            FREE(nodes[i]);
        }
    }
    fsqu_free(&ctx->reqadd);
    /* 释放节点池 */
    pool_free(&ctx->node_pool);
    cond_free(&ctx->cond);
    mutex_free(&ctx->mu);
}
/*  注意：reqadd 在 mpq 侧（非 macOS）容量满时 fsqu_push 会自旋阻塞调用方业务线程，
 *  在 queue 侧（macOS）则自动扩容。常规负载下不会触发（容量 4096，主线程 ≤5ms 排空一轮）。
 *  仅在极端突发或时间轮主线程被严重抢占时才会触底。*/
void tw_add(tw_ctx *ctx, const uint32_t timeout, tw_cb _cb, free_cb _freecb, ud_cxt *ud) {
    if (0 == timeout) {
        _cb(ud);
        return;
    }
    tw_node_ctx *node = (tw_node_ctx *)pool_pop(&ctx->node_pool, NULL, 0);
    COPY_UD(node->ud, ud);
    node->expires = timer_cur_ms(&ctx->timer) + timeout;
    node->_cb = _cb;
    node->_freecb = _freecb;
    node->next = NULL;
    fsqu_push(&ctx->reqadd, &node);
    /* 仅当标志由 0→1 时（首批新任务）才唤醒轮线程，批量入队后续节点不重复 signal */
    if (ATOMIC_CAS(&ctx->reqadd_pending, 0, 1)) {
        mutex_lock(&ctx->mu);
        cond_signal(&ctx->cond);
        mutex_unlock(&ctx->mu);
    }
}
// 将节点追加到指定槽位的链表尾部
static void _tw_insert(tw_slot_ctx *slot, tw_node_ctx *node) {
    if (NULL == slot->head) {
        slot->head = slot->tail = node;
        return;
    }
    slot->tail->next = node;
    slot->tail = node;
}
// 根据节点的到期时间计算应放入 tv1～tv5 中的哪个槽位
static tw_slot_ctx *_tw_getslot(tw_ctx *ctx, tw_node_ctx *node) {
    tw_slot_ctx *slot;
    if (node->expires <= ctx->jiffies) {
        // 已过期：放入当前 jiffies 对应的 tv1 槽，下一次 _tw_run 立即触发
        return &ctx->tv1[(ctx->jiffies & TVR_MASK)];
    }
    uint64_t idx = node->expires - ctx->jiffies;
    if (idx < TVR_SIZE) {
        // tv1：超时在 [1, 255] ms 内，直接按到期时间低 8 位定槽
        slot = &ctx->tv1[(node->expires & TVR_MASK)];
    } else if (idx < 1ULL << (TVR_BITS + TVN_BITS)) {
        // tv2：超时在 [256, 16383] ms 内，取到期时间第 8~13 位定槽
        slot = &ctx->tv2[((node->expires >> TVR_BITS) & TVN_MASK)];
    } else if (idx < 1ULL << (TVR_BITS + 2 * TVN_BITS)) {
        // tv3：超时在 [16384, 1048575] ms (~17 min) 内，取到期时间第 14~19 位定槽
        slot = &ctx->tv3[((node->expires >> (TVR_BITS + TVN_BITS)) & TVN_MASK)];
    } else if (idx < 1ULL << (TVR_BITS + 3 * TVN_BITS)) {
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
static uint32_t _tw_cascade(tw_ctx *ctx, tw_slot_ctx *slot, const uint32_t index) {
    tw_node_ctx *pnext, *pnode = slot[index].head;
    while (NULL != pnode) {
        pnext = pnode->next;
        pnode->next = NULL;
        _tw_insert(_tw_getslot(ctx, pnode), pnode);
        pnode = pnext;
    }
    slot[index].head = slot[index].tail = NULL;
    return index;
}
// 推进一个 jiffie：先做进位 cascade，再执行当前槽位所有到期节点的回调
static void _tw_run(tw_ctx *ctx) {
    //调整
    uint32_t ulidx = (uint32_t)(ctx->jiffies & TVR_MASK);
    if (!ulidx
        && (!_tw_cascade(ctx, ctx->tv2, INDEX(0)))
        && (!_tw_cascade(ctx, ctx->tv3, INDEX(1)))
        && (!_tw_cascade(ctx, ctx->tv4, INDEX(2)))) {
        _tw_cascade(ctx, ctx->tv5, INDEX(3));
    }
    ++ctx->jiffies;
    //执行
    ud_cxt ud;
    tw_cb  cb;
    tw_node_ctx *pnext, *pnode = ctx->tv1[ulidx].head;
    while (NULL != pnode) {
        pnext  = pnode->next;
        // 先将 ud 拷贝到栈，再释放节点，再调用回调：
        // 确保回调持有的指针不指向已被池复用的节点内存（防 UAF）。
        // 回调契约：不得将 &ud 存入生命周期超出本次调用的结构体。
        ud = pnode->ud;
        cb = pnode->_cb;
        pool_push(&ctx->node_pool, pnode, 0);
        cb(&ud);
        pnode = pnext;
    }
    ctx->tv1[ulidx].head = ctx->tv1[ulidx].tail = NULL;
}
// 批量排空 reqadd 队列，将节点分发到对应时间轮槽位
static void _tw_insert_all(tw_ctx *ctx, tw_node_ctx **nodes) {
    uint32_t n, i;
    while ((n = fsqu_pop_sc_batch(&ctx->reqadd, nodes, TW_REQADD_BATCH)) > 0) {
        for (i = 0; i < n; i++) {
            _tw_insert(_tw_getslot(ctx, nodes[i]), nodes[i]);
        }
    }
}
// 时间轮工作线程入口：分发新任务、推进 jiffies、精确睡眠等待下一个到期
static void _tw_loop(void *arg) {
    uint64_t curtick;
    uint32_t sleep_ms;
    tw_ctx *ctx = (tw_ctx *)arg;
    tw_node_ctx *nodes[TW_REQADD_BATCH];
    ctx->jiffies = timer_cur_ms(&ctx->timer);
    uint64_t shrink_start = ctx->jiffies;
    while (0 == ATOMIC_GET(&ctx->exit)) {
        /*  将外部通过 tw_add 提交的节点分发到对应槽位（reqadd 仅 tw 主线程独占消费，走 pop_sc_batch）。
         *  tw_add 已设置 node->next = NULL。
         *  先排空，再清标志，再二次排空：避免清标志与生产者入队之间的竞态导致漏唤醒 */
        _tw_insert_all(ctx, nodes);
        ATOMIC_SET(&ctx->reqadd_pending, 0);
        _tw_insert_all(ctx, nodes);
        /* 2. 处理所有已到期的 jiffies */
        curtick = timer_cur_ms(&ctx->timer);
        while (ctx->jiffies <= curtick) {
            _tw_run(ctx);
        }
        // 空闲时按 SHRINK_TIME 门控回落节点池
        if (curtick - shrink_start >= SHRINK_TIME) {
            shrink_start = curtick;
            pool_shrink(&ctx->node_pool, SHRINK_NKEEP(pool_size(&ctx->node_pool)), SHRINK_BUSY);
        }
        /*  精确睡眠直到下一个 jiffy 到期，而非固定 1ms 空转。
         *  sleep_ms = max(1, min(next_jiffy - now, 5))
         *  上限 5ms：单次 OS 调度抖动最多影响 5ms，降低大延迟的概率。
         *  tw_add / tw_free 会提前 cond_signal 唤醒。 */
        curtick = timer_cur_ms(&ctx->timer);
        sleep_ms = (ctx->jiffies > curtick) ? (uint32_t)(ctx->jiffies - curtick) : 1;
        if (sleep_ms > 5) {
            sleep_ms = 5;
        }
        mutex_lock(&ctx->mu);
        if (0 == ATOMIC_GET(&ctx->exit)) {
            cond_timedwait(&ctx->cond, &ctx->mu, sleep_ms);
        }
        mutex_unlock(&ctx->mu);
    }
    LOG_INFO("%s", "timewheel thread exited.");
}
void tw_init(tw_ctx *ctx, uint32_t capacity, const thread_hooks *hooks) {
    ATOMIC_SET(&ctx->exit, 0);
    ctx->jiffies = 0;
    ATOMIC_SET(&ctx->reqadd_pending, 0);
    mutex_init(&ctx->mu);
    cond_init(&ctx->cond);
    timer_init(&ctx->timer);
    fsqu_init(&ctx->reqadd, sizeof(tw_node_ctx *), 0 == capacity ? 4 * ONEK : capacity);
    pool_init(&ctx->node_pool, sizeof(tw_node_ctx), TW_NODE_POOL_MAX, TW_NODE_POOL_MAX / 4, 1, NULL);
    ZERO(ctx->tv1, sizeof(ctx->tv1));
    ZERO(ctx->tv2, sizeof(ctx->tv2));
    ZERO(ctx->tv3, sizeof(ctx->tv3));
    ZERO(ctx->tv4, sizeof(ctx->tv4));
    ZERO(ctx->tv5, sizeof(ctx->tv5));
    if (NULL != hooks) {
        ctx->thtw = thread_creat_hooks(_tw_loop, hooks->init, hooks->exit, ctx, hooks->assist);
    } else {
        ctx->thtw = thread_creat(_tw_loop, ctx);
    }
}
