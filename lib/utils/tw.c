#include "utils/tw.h"

#define TW_NODE_POOL_MAX    (4 * ONEK) // 节点池上限：超出后直接释放，避免无界增长
#define TW_REQADD_BATCH     128 // reqadd 单次批量出队上限

// 释放时间轮槽位数组中所有节点，并调用各节点的 _freecb 释放用户数据
static void _tw_free_slot(list_ctx *slot, const size_t len) {
    tw_node_ctx *pdel;
    for (size_t i = 0; i < len; i++) {
        list_foreach_safe(&slot[i], ln, tmp) {
            pdel = UPCAST(ln, tw_node_ctx, node);
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
/*  注意：reqadd 容量 4096，常规负载下不会触底 —— 排空是信号驱动而非 tick 驱动：
 *  pending 由 0→1 的那次 push 立刻唤醒轮线程，队列不会随轮线程睡得久而积压。
 *  极端突发或时间轮主线程被严重抢占时触底，fsqu_push 降级到无界溢出层，不阻塞调用方。*/
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
    fsqu_push(&ctx->reqadd, &node);
    /* 仅当标志由 0→1 时（首批新任务）才唤醒轮线程，批量入队后续节点不重复 signal */
    if (ATOMIC_CAS(&ctx->reqadd_pending, 0, 1)) {
        mutex_lock(&ctx->mu);
        cond_signal(&ctx->cond);
        mutex_unlock(&ctx->mu);
    }
}
// 根据节点的到期时间计算应放入 tv1～tv5 中的哪个槽位
static list_ctx *_tw_getslot(tw_ctx *ctx, tw_node_ctx *node) {
    list_ctx *slot;
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
static uint32_t _tw_cascade(tw_ctx *ctx, list_ctx *slot, const uint32_t index) {
    tw_node_ctx *pnode;
    list_foreach_safe(&slot[index], ln, tmp) {
        pnode = UPCAST(ln, tw_node_ctx, node);
        list_remove(&slot[index], ln);// 从源槽摘除
        list_push_tail(_tw_getslot(ctx, pnode), &pnode->node);// 改投目标槽（cascade 必降级,目标≠源槽）
    }
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
    tw_cb cb;
    tw_node_ctx *pnode;
    list_foreach_safe(&ctx->tv1[ulidx], ln, tmp) {
        pnode = UPCAST(ln, tw_node_ctx, node);
        // 顺序不能变：先把 ud 拷到栈，再还节点，再回调，否则回调拿到的是已被池复用的内存。
        // 回调契约：不得把 &ud 存进生命周期超出本次调用的结构体
        ud = pnode->ud;
        cb = pnode->_cb;
        pool_push(&ctx->node_pool, pnode, 0);
        cb(&ud);
    }
    list_init(&ctx->tv1[ulidx]);
}
// 批量排空 reqadd 队列，将节点分发到对应时间轮槽位
static void _tw_insert_all(tw_ctx *ctx, tw_node_ctx **nodes) {
    uint32_t n, i;
    while ((n = fsqu_pop_sc_batch(&ctx->reqadd, nodes, TW_REQADD_BATCH)) > 0) {
        for (i = 0; i < n; i++) {
            list_push_tail(_tw_getslot(ctx, nodes[i]), &nodes[i]->node);
        }
    }
}
// 距下一次必须醒来的 jiffy 有多少毫秒，取值 [0, TVR_MASK]。
// tv1 的 256 个槽恰好覆盖 [jiffies, jiffies+255]（_tw_getslot 对 expires-jiffies < 256 的
// 节点按 expires 低 8 位定槽，已过期节点落当前槽），故由近及远扫到第一个非空槽即最近到期。
// 只需扫到 cascade 边界：tv2~tv5 的节点只能经 cascade 进 tv1, 而 cascade 只发生在
// jiffies & TVR_MASK == 0 的 tick 上, 睡到边界必不过头。到期越密扫得越浅, 开销自限
static uint32_t _tw_next_delta(tw_ctx *ctx) {
    uint32_t d;
    uint32_t bound = (uint32_t)((TVR_SIZE - (ctx->jiffies & TVR_MASK)) & TVR_MASK);
    for (d = 0; d < bound; d++) {
        if (!list_empty(&ctx->tv1[(ctx->jiffies + d) & TVR_MASK])) {
            return d;
        }
    }
    return bound;
}
// 时间轮工作线程入口：分发新任务、推进 jiffies、精确睡眠等待下一个到期
static void _tw_loop(void *arg) {
    uint64_t curtick;
    uint64_t wake_at;
    uint32_t sleep_ms;
    tw_ctx *ctx = (tw_ctx *)arg;
    tw_node_ctx *nodes[TW_REQADD_BATCH];
    ctx->jiffies = timer_cur_ms(&ctx->timer);
    uint64_t shrink_start = ctx->jiffies;
    while (0 == ATOMIC_GET(&ctx->exit)) {
        /*  将外部通过 tw_add 提交的节点分发到对应槽位（reqadd 仅 tw 主线程独占消费，走 pop_sc_batch）。
         *  节点链接由 list_push_tail 设置，无需 tw_add 预置。
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
            pool_shrink(&ctx->node_pool);
        }
        /* 睡到下一个必须醒的 jiffy: 最近的 tv1 到期或下一个 cascade 边界, 上界由
         * _tw_next_delta 保证不超过 256ms, 不用钳位。wake_at 已过说明本轮耗时超过了
         * 下一到期, 不睡直接回追赶循环; tw_add / tw_free 会提前 cond_signal 唤醒。*/
        curtick = timer_cur_ms(&ctx->timer);
        wake_at = ctx->jiffies + _tw_next_delta(ctx);
        if (wake_at <= curtick) {
            continue;
        }
        sleep_ms = (uint32_t)(wake_at - curtick);
        mutex_lock(&ctx->mu);
        /*  必须在锁内复查 reqadd_pending：生产者 CAS 置位后要拿 ctx->mu 才能 signal，
         *  若它落在本线程加锁之前，signal 打空 —— 睡 1ms 时最多晚 1ms 无害，
         *  睡到 256ms 就是定时器迟到。查到已置位就跳过等待，回去排空即可 */
        if (0 == ATOMIC_GET(&ctx->exit)
            && 0 == ATOMIC_GET(&ctx->reqadd_pending)) {
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
