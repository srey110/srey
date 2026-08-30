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
    (void)spcnt;
    *ctx = OS_UNFAIR_LOCK_INIT;
#else
    (void)spcnt;
    ASSERTAB_CODE(pthread_spin_init(ctx, PTHREAD_PROCESS_PRIVATE));
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
    (void)ctx;
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
    ASSERTAB_CODE(pthread_spin_lock(ctx));
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
    return ERR_OK == pthread_spin_trylock(ctx) ? ERR_OK : ERR_FAILED;
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
    ASSERTAB_CODE(pthread_spin_unlock(ctx));
#endif
};
/// <summary>
/// 自旋等待时的退避：先 CPU_PAUSE 空转，累计到 SPIN_YIELD_CNT 次仍等不到就 THREAD_YIELD
/// 让出 CPU。对端被抢占时继续空转是纯烧核，让出才给得了它跑完的机会
/// </summary>
/// <param name="spins">自旋计数，调用方持有并初始化为 0；让出时函数自己归零。一次等待从头到尾
///   共用一个计数——分段各起一个会把让出前的预算按段数放大；等到了(或不再等了)之后调用方要清零，
///   不清则下一次等待带着上回用剩的预算开始，可能第一次就让出 CPU</param>
static inline void spin_backoff(uint32_t *spins) {
    if (++(*spins) < SPIN_YIELD_CNT) {
        CPU_PAUSE();
        return;
    }
    *spins = 0;
    THREAD_YIELD();
}

#endif//SPINLOCK_H_
