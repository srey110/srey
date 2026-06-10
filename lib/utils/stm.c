#include "utils/stm.h"

// 创建 stm_data: 持有 data + sz, ref 初始为 1 (writer 那一票)
// copy=1 时内部 MALLOC 拷贝 data, 调用方仍持有原 data; copy=0 时直接接管 data 所有权
static stm_data *_stm_new_data(void *data, size_t sz, int32_t copy) {
    stm_data *snap;
    MALLOC(snap, sizeof(stm_data));
    ATOMIC_SET(&snap->ref, 1);
    snap->sz = sz;
    if (0 == copy) {
        snap->data = data;
    } else {
        MALLOC(snap->data, sz);
        memcpy(snap->data, data, sz);
    }
    return snap;
}
// 释放 stm_data 引用; ref 归 0 时 FREE data + FREE 自身; snap=NULL 安全 noop
static void _stm_free_data(stm_data *snap) {
    if (NULL == snap) {
        return;
    }
    if (1 == ATOMIC_ADD(&snap->ref, -1)) {
        FREE(snap->data);
        FREE(snap);
    }
}
stm_ctx *stm_new(void *data, size_t sz, int32_t copy) {
    stm_ctx *ctx;
    MALLOC(ctx, sizeof(stm_ctx));
    rwlock_init(&ctx->lock);
    ATOMIC_SET(&ctx->ref, 1);
    ctx->data = _stm_new_data(data, sz, copy);
    return ctx;
}
void stm_free(stm_ctx *ctx) {
    // wrlock 内清当前快照: 保证此后 stm_grab_data 要么看到旧快照 (已 inc 自己 ref), 要么看到 NULL
    rwlock_wrlock(&ctx->lock);
    _stm_free_data(ctx->data);
    ctx->data = NULL;
    rwlock_unlock(&ctx->lock);
    // 必须在 unlock 之后才减引: 同步 release 的 reader 若把 ref 减到 0 会触发 rwlock_free + FREE(ctx),
    // 而 pthread_rwlock_destroy 在锁被持有时为 UB. unlock 后再减保证 reader free 时 writer 已离开锁.
    if (1 == ATOMIC_ADD(&ctx->ref, -1)) {
        rwlock_free(&ctx->lock);
        FREE(ctx);
    }
}
void stm_update(stm_ctx *ctx, void *data, size_t sz, int32_t copy) {
    // 先在锁外构造新快照, 缩短 wrlock 持有时间
    stm_data *snap = _stm_new_data(data, sz, copy);
    rwlock_wrlock(&ctx->lock);
    stm_data *old = ctx->data;
    ctx->data = snap;
    rwlock_unlock(&ctx->lock);
    // 锁外释放旧快照: 已 stm_grab_data 的 reader 持自己的引用, 这里只减 writer 的 1 票
    _stm_free_data(old);
}
stm_ctx *stm_grab(stm_ctx *ctx) {
    ATOMIC_ADD(&ctx->ref, 1);
    return ctx;
}
void stm_ungrab(stm_ctx *ctx) {
    if (1 == ATOMIC_ADD(&ctx->ref, -1)) {
        // 自己是最后一个引用 (writer 此前已走 stm_free, ctx->data 必为 NULL)
        rwlock_free(&ctx->lock);
        FREE(ctx);
    }
}
stm_data *stm_grab_data(stm_ctx *ctx) {
    rwlock_rdlock(&ctx->lock);
    stm_data *snap = ctx->data;
    if (NULL != snap) {
        ATOMIC_ADD(&snap->ref, 1);
    }
    rwlock_unlock(&ctx->lock);
    return snap;
}
void stm_ungrab_data(stm_data *data) {
    _stm_free_data(data);
}
