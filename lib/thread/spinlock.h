#ifndef SPINLOCK_H_
#define SPINLOCK_H_

#include "base/macro.h"

#if defined(OS_WIN)
typedef CRITICAL_SECTION spin_ctx;
#elif defined(OS_DARWIN)
typedef os_unfair_lock spin_ctx;
#else
typedef pthread_spinlock_t spin_ctx;
#endif
/// <summary>
/// 自旋锁初始化
/// </summary>
/// <param name="ctx">spin_ctx</param>
/// <param name="spcnt">次数</param>
static inline void spin_init(spin_ctx *ctx, const uint32_t spcnt) {
#if defined(OS_WIN)
    ASSERTAB(InitializeCriticalSectionAndSpinCount(ctx, spcnt), ERRORSTR(ERRNO));
#elif defined(OS_DARWIN)
    *ctx = OS_UNFAIR_LOCK_INIT;
#else
    ASSERTAB(ERR_OK == pthread_spin_init(ctx, PTHREAD_PROCESS_PRIVATE), ERRORSTR(ERRNO));
#endif
};
/// <summary>
/// 自旋锁释放
/// </summary>
/// <param name="ctx">spin_ctx</param>
static inline void spin_free(spin_ctx *ctx) {
#if defined(OS_WIN)
    DeleteCriticalSection(ctx);
#elif defined(OS_DARWIN)
#else
    (void)pthread_spin_destroy(ctx);
#endif
};
/// <summary>
/// 锁定
/// </summary>
/// <param name="ctx">spin_ctx</param>
static inline void spin_lock(spin_ctx *ctx) {
#if defined(OS_WIN)
    EnterCriticalSection(ctx);
#elif defined(OS_DARWIN)
    os_unfair_lock_lock(ctx);
#else
    ASSERTAB(ERR_OK == pthread_spin_lock(ctx), ERRORSTR(ERRNO));
#endif
};
/// <summary>
/// 尝试锁定
/// </summary>
/// <param name="ctx">spin_ctx</param>
/// <returns>ERR_OK 成功</returns>
static inline int32_t spin_trylock(spin_ctx *ctx) {
#if defined(OS_WIN)
    return TRUE == TryEnterCriticalSection(ctx) ? ERR_OK : ERR_FAILED;
#elif defined(OS_DARWIN)
    return os_unfair_lock_trylock(ctx) ? ERR_OK : ERR_FAILED;
#else
    return pthread_spin_trylock(ctx);
#endif
};
/// <summary>
/// 解锁
/// </summary>
/// <param name="ctx">spin_ctx</param>
static inline void spin_unlock(spin_ctx *ctx) {
#if defined(OS_WIN)
    LeaveCriticalSection(ctx);
#elif defined(OS_DARWIN)
    os_unfair_lock_unlock(ctx);
#else
    ASSERTAB(ERR_OK == pthread_spin_unlock(ctx), ERRORSTR(ERRNO));
#endif
};

#endif//SPINLOCK_H_
