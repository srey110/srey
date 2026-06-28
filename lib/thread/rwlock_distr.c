#include "thread/rwlock_distr.h"

// 同一线程同时持有的 (ctx, slot) 槽位;owner=NULL 表示该槽位空闲
// 通过线性扫描查找 ctx 对应的 slot,容量上限 RWLOCK_DISTR_MAX_TLS
typedef struct rwlock_distr_tls_entry {
    int32_t slot;             // owner->slots 中的索引;owner=NULL 时无意义
    rwlock_distr_ctx *owner;  // 注册到的 ctx;NULL=空闲
} rwlock_distr_tls_entry;
static THREAD_LOCAL rwlock_distr_tls_entry _tls[RWLOCK_DISTR_MAX_TLS];

// 线性扫 _tls 返回 ctx 对应槽位下标;未注册返回 -1
static int32_t _rwlock_distr_tls_find(rwlock_distr_ctx *ctx) {
    for (int32_t i = 0; i < RWLOCK_DISTR_MAX_TLS; i++) {
        if (_tls[i].owner == ctx) {
            return i;
        }
    }
    return -1;
}
void rwlock_distr_init(rwlock_distr_ctx *ctx, uint32_t slot_count) {
    ASSERTAB(slot_count > 0, "rwlock_distr_init: slot_count must be > 0");
    rwlock_init(&ctx->fallback);
    ATOMIC_SET(&ctx->write_flag, 0);
    ctx->slot_count = slot_count;
    // 多分配一个 cache line 用于手动对齐,保证首个 slot 在 cache line 边界
    size_t total = (size_t)slot_count * sizeof(rwlock_distr_slot) + CACHELINE_SIZE;
    MALLOC(ctx->slots_raw, total);
    uintptr_t addr = (uintptr_t)ctx->slots_raw;
    addr = ROUND_UP(addr, CACHELINE_SIZE);
    ctx->slots = (rwlock_distr_slot *)addr;
    ZERO(ctx->slots, (size_t)slot_count * sizeof(rwlock_distr_slot));
}
void rwlock_distr_free(rwlock_distr_ctx *ctx) {
    rwlock_free(&ctx->fallback);
    FREE(ctx->slots_raw);
    ctx->slots = NULL;
    ctx->slot_count = 0;
}
int32_t rwlock_distr_register(rwlock_distr_ctx *ctx) {
    // 扫 _tls:已注册到本 ctx 则幂等返回;同时记录第一个空闲槽位以备分配
    int32_t free_idx = -1;
    for (int32_t i = 0; i < RWLOCK_DISTR_MAX_TLS; i++) {
        if (_tls[i].owner == ctx) {
            return ERR_OK;
        }
        if (NULL == _tls[i].owner && -1 == free_idx) {
            free_idx = i;
        }
    }
    if (-1 == free_idx) {
        // TLS 数组已满,该线程对本 ctx 走 fallback
        return ERR_FAILED;
    }
    // GET-then-CAS 扫描空 slot;先 GET 降低热槽 CAS 失败带来的 cache 弹跳
    for (uint32_t i = 0; i < ctx->slot_count; i++) {
        if (0 == ATOMIC_GET(&ctx->slots[i].in_use)
            && ATOMIC_CAS(&ctx->slots[i].in_use, 0, 1)) {
            _tls[free_idx].owner = ctx;
            _tls[free_idx].slot = (int32_t)i;
            return ERR_OK;
        }
    }
    return ERR_FAILED;
}
void rwlock_distr_unregister(rwlock_distr_ctx *ctx) {
    int32_t i = _rwlock_distr_tls_find(ctx);
    if (-1 == i) {
        return;
    }
    int32_t idx = _tls[i].slot;
    _tls[i].owner = NULL;
    _tls[i].slot = 0;
    // 兜底清 active,即使调用方违反契约也不让 writer 卡死
    ATOMIC_SET(&ctx->slots[idx].active, 0);
    ATOMIC_SET(&ctx->slots[idx].in_use, 0);
}
void rwlock_distr_rdlock(rwlock_distr_ctx *ctx) {
    int32_t i = _rwlock_distr_tls_find(ctx);
    if (-1 != i) {
        int32_t slot = _tls[i].slot;
        // store active=1 与 load write_flag 之间靠 ATOMIC_SET 的 seq_cst 屏障保证顺序
        // writer 在 wrlock 中先 store write_flag=1 再扫所有 slot 的 active
        // 两侧握手:任一方先标记,另一方必能看见
        for (;;) {
            ATOMIC_SET(&ctx->slots[slot].active, 1);
            if (!ATOMIC_GET(&ctx->write_flag)) {
                return;
            }
            // 检测到 writer,让步避免死锁
            ATOMIC_SET(&ctx->slots[slot].active, 0);
            while (ATOMIC_GET(&ctx->write_flag)) {
                CPU_PAUSE();
            }
        }
    }
    // 未注册到本 ctx(TLS 满 / slot 满 / 未 register)走 fallback
    rwlock_rdlock(&ctx->fallback);
}
void rwlock_distr_runlock(rwlock_distr_ctx *ctx) {
    int32_t i = _rwlock_distr_tls_find(ctx);
    if (-1 != i) {
        ATOMIC_SET(&ctx->slots[_tls[i].slot].active, 0);
        return;
    }
    rwlock_unlock(&ctx->fallback);
}
void rwlock_distr_wrlock(rwlock_distr_ctx *ctx) {
    // 先拿 fallback 写锁:阻塞未注册 reader 与并发 writer
    rwlock_wrlock(&ctx->fallback);
    // 再置 write_flag:阻塞新 slot reader
    ATOMIC_SET(&ctx->write_flag, 1);
    // 等所有已进入 slot reader 退出
    for (uint32_t i = 0; i < ctx->slot_count; i++) {
        while (ATOMIC_GET(&ctx->slots[i].active)) {
            CPU_PAUSE();
        }
    }
}
void rwlock_distr_wrunlock(rwlock_distr_ctx *ctx) {
    ATOMIC_SET(&ctx->write_flag, 0);
    rwlock_unlock(&ctx->fallback);
}
