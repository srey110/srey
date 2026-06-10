#ifndef RWLOCK_H_
#define RWLOCK_H_

#include "base/macro.h"

typedef struct rwlock_ctx {
#if defined(OS_WIN)
    uint32_t wlock;
    SRWLOCK rwlock;
#else
    pthread_rwlock_t rwlock;
#endif
}rwlock_ctx;
/// <summary>
/// 读写锁初始化
/// </summary>
/// <param name="ctx">rwlock_ctx</param>
static inline void rwlock_init(rwlock_ctx *ctx) {
#if defined(OS_WIN)
    InitializeSRWLock(&ctx->rwlock);
    ctx->wlock = 0;
#else
    ASSERTAB((ERR_OK == pthread_rwlock_init(&ctx->rwlock, NULL)),
        ERRORSTR(ERRNO));
#endif
};
/// <summary>
/// 读写锁释放
/// </summary>
/// <param name="ctx">rwlock_ctx</param>
static inline void rwlock_free(rwlock_ctx *ctx) {
#if defined(OS_WIN)
#else
    (void)pthread_rwlock_destroy(&ctx->rwlock);
#endif
};
/// <summary>
/// 读锁定
/// </summary>
/// <param name="ctx">rwlock_ctx</param>
static inline void rwlock_rdlock(rwlock_ctx *ctx) {
#if defined(OS_WIN)
    AcquireSRWLockShared(&ctx->rwlock);
#else
    ASSERTAB(ERR_OK == pthread_rwlock_rdlock(&ctx->rwlock), ERRORSTR(ERRNO));
#endif
};
/// <summary>
/// 尝试读锁定
/// </summary>
/// <param name="ctx">rwlock_ctx</param>
/// <returns>ERR_OK 成功</returns>
static inline int32_t rwlock_tryrdlock(rwlock_ctx *ctx) {
#if defined(OS_WIN)
    return 0 != TryAcquireSRWLockShared(&ctx->rwlock) ? ERR_OK : ERR_FAILED;
#else
    return pthread_rwlock_tryrdlock(&ctx->rwlock);
#endif
};
/// <summary>
/// 写锁定
/// </summary>
/// <param name="ctx">rwlock_ctx</param>
static inline void rwlock_wrlock(rwlock_ctx *ctx) {
#if defined(OS_WIN)
    AcquireSRWLockExclusive(&ctx->rwlock); /* acquire barrier */
    ctx->wlock = 1; /* 写锁独占，此处无并发写者；AcquireSRWLockExclusive 阻止编译器跨调用缓存 */
#else
    ASSERTAB(ERR_OK == pthread_rwlock_wrlock(&ctx->rwlock), ERRORSTR(ERRNO));
#endif
};
/// <summary>
/// 尝试写锁定
/// </summary>
/// <param name="ctx">rwlock_ctx</param>
/// <returns>ERR_OK 成功</returns>
static inline int32_t rwlock_trywrlock(rwlock_ctx *ctx) {
#if defined(OS_WIN)
    if (0 != TryAcquireSRWLockExclusive(&ctx->rwlock)) {
        ctx->wlock = 1;
        return ERR_OK;
    }
    return ERR_FAILED;
#else
    return pthread_rwlock_trywrlock(&ctx->rwlock);
#endif
};
/// <summary>
/// 解锁
/// </summary>
/// <param name="ctx">rwlock_ctx</param>
static inline void rwlock_unlock(rwlock_ctx *ctx) {
#if defined(OS_WIN)
    /* wlock 无需原子操作：写锁排他，读写均发生在同一线程持锁期间，无并发修改。
     * SRW release/acquire 语义保证写锁释放前的 wlock=0 对后续锁持有者可见；
     * SRW API 为不透明外部调用，编译器不会跨调用缓存 wlock 的值。*/
    if (0 != ctx->wlock) {
        ctx->wlock = 0;
        ReleaseSRWLockExclusive(&ctx->rwlock); /* release barrier，wlock=0 对后续 acquire 可见 */
    } else {
        ReleaseSRWLockShared(&ctx->rwlock);
    }
#else
    ASSERTAB(ERR_OK == pthread_rwlock_unlock(&ctx->rwlock), ERRORSTR(ERRNO));
#endif
};

#endif//RWLOCK_H_
