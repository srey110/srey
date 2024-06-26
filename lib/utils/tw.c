#include "utils/tw.h"

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
    thread_join(ctx->thtw);
    _free_slot(ctx->tv1, TVR_SIZE);
    _free_slot(ctx->tv2, TVN_SIZE);
    _free_slot(ctx->tv3, TVN_SIZE);
    _free_slot(ctx->tv4, TVN_SIZE);
    _free_slot(ctx->tv5, TVN_SIZE);
    _free_slot(&ctx->reqadd, 1);
    spin_free(&ctx->spin);
}
static void _insert(tw_slot_ctx *slot, tw_node_ctx *node) {
    if (NULL == slot->head) {
        slot->head = slot->tail = node;
        return;
    }
    slot->tail->next = node;
    slot->tail = node;
}
void tw_add(tw_ctx *ctx, const uint32_t timeout, tw_cb _cb, free_cb _freecb, ud_cxt *ud) {
    tw_node_ctx *node;
    MALLOC(node, sizeof(tw_node_ctx));
    COPY_UD(node->ud, ud);
    node->expires = timer_cur_ms(&ctx->timer) + timeout;
    node->_cb = _cb;
    node->_freecb = _freecb;
    node->next = NULL;
    spin_lock(&ctx->spin);
    _insert(&ctx->reqadd, node);
    spin_unlock(&ctx->spin);
}
static tw_slot_ctx *_getslot(tw_ctx *ctx, tw_node_ctx *node) {
    tw_slot_ctx *slot;
    uint32_t idx = (uint32_t)(node->expires - ctx->jiffies);
    if ((int32_t)idx < 0) {
        slot = &ctx->tv1[(ctx->jiffies & TVR_MASK)];
    } else if (idx < TVR_SIZE) {
        slot = &ctx->tv1[(node->expires & TVR_MASK)];
    } else if (idx < 1 << (TVR_BITS + TVN_BITS)) {
        slot = &ctx->tv2[((node->expires >> TVR_BITS) & TVN_MASK)];
    } else if (idx < 1 << (TVR_BITS + 2 * TVN_BITS)) {
        slot = &ctx->tv3[((node->expires >> (TVR_BITS + TVN_BITS)) & TVN_MASK)];
    } else if (idx < 1 << (TVR_BITS + 3 * TVN_BITS)) {
        slot = &ctx->tv4[((node->expires >> (TVR_BITS + 2 * TVN_BITS)) & TVN_MASK)];
    } else {
        if (idx > 0xffffffffUL) {
            idx = 0xffffffffUL;
            node->expires = idx + ctx->jiffies;
        }
        slot = &ctx->tv5[((node->expires >> (TVR_BITS + 3 * TVN_BITS)) & TVN_MASK)];
    }
    return slot;
}
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
static void _run(tw_ctx *ctx) {
    //����
    uint32_t ulidx = (uint32_t)(ctx->jiffies & TVR_MASK);
    if (!ulidx
        && (!_cascade(ctx, ctx->tv2, INDEX(0)))
        && (!_cascade(ctx, ctx->tv3, INDEX(1)))
        && (!_cascade(ctx, ctx->tv4, INDEX(2)))) {
        _cascade(ctx, ctx->tv5, INDEX(3));
    }
    ++ctx->jiffies;
    //ִ��
    tw_node_ctx *pnext, *pnode = ctx->tv1[ulidx].head;
    while (NULL != pnode) {
        pnext = pnode->next;
        pnode->_cb(&pnode->ud);
        FREE(pnode);
        pnode = pnext;
    }
    ctx->tv1[ulidx].head = ctx->tv1[ulidx].tail = NULL;
}
static void _loop(void *arg) {
    uint64_t curtick;
    tw_node_ctx *next, *node;
    tw_ctx *ctx = (tw_ctx *)arg;
    ctx->jiffies = timer_cur_ms(&ctx->timer);
    while (0 == ctx->exit) {
        spin_lock(&ctx->spin);
        node = ctx->reqadd.head;
        while (NULL != node) {
            next = node->next;
            node->next = NULL;
            _insert(_getslot(ctx, node), node);
            node = next;
        }
        ctx->reqadd.head = ctx->reqadd.tail = NULL;
        spin_unlock(&ctx->spin);
        curtick = timer_cur_ms(&ctx->timer);
        while (ctx->jiffies <= curtick) {
            _run(ctx);
        }
        USLEEP(1000);
    }
    LOG_INFO("%s", "timewheel thread exited.");
}
void tw_init(tw_ctx *ctx) {
    ctx->exit = 0;
    ctx->jiffies = 0;
    spin_init(&ctx->spin, SPIN_CNT_TIMEWHEEL);
    timer_init(&ctx->timer);
    ctx->reqadd.head = ctx->reqadd.tail = NULL;
    ZERO(ctx->tv1, sizeof(ctx->tv1));
    ZERO(ctx->tv2, sizeof(ctx->tv2));
    ZERO(ctx->tv3, sizeof(ctx->tv3));
    ZERO(ctx->tv4, sizeof(ctx->tv4));
    ZERO(ctx->tv5, sizeof(ctx->tv5));
    ctx->thtw = thread_creat(_loop, ctx);
}
