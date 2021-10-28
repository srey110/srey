#ifndef SPINLOCK_H_
#define SPINLOCK_H_

#include "macro.h"

#if defined(OS_WIN)
typedef CRITICAL_SECTION spin_ctx;
#elif defined(OS_DARWIN)
typedef OSSpinLock spin_ctx;
#else
typedef pthread_spinlock_t spin_ctx;
#endif
/*
* \brief          初始化  ONEK
*/
static inline void spin_init(spin_ctx *pctx, const uint32_t uispcnt)
{
#if defined(OS_WIN)
    ASSERTAB(InitializeCriticalSectionAndSpinCount(pctx, uispcnt), ERRORSTR(ERRNO));
#elif defined(OS_DARWIN)
#else
    ASSERTAB(ERR_OK == pthread_spin_init(pctx, PTHREAD_PROCESS_PRIVATE), ERRORSTR(ERRNO));
#endif
};
/*
* \brief          释放
*/
static inline void spin_free(spin_ctx *pctx)
{
#if defined(OS_WIN)
    DeleteCriticalSection(pctx);
#elif defined(OS_DARWIN)
#else
    (void)pthread_spin_destroy(pctx);
#endif
};
/*
* \brief          锁定
*/
static inline void spin_lock(spin_ctx *pctx)
{
#if defined(OS_WIN)
    EnterCriticalSection(pctx);
#elif defined(OS_DARWIN)
    OSSpinLockLock(pctx);
#else
    ASSERTAB(ERR_OK == pthread_spin_lock(pctx), ERRORSTR(ERRNO));
#endif
};
/*
* \brief          非阻塞锁定
* \return         ERR_OK 成功
*/
static inline int32_t spin_trylock(spin_ctx *pctx)
{
#if defined(OS_WIN)
    return TRUE == TryEnterCriticalSection(pctx) ? ERR_OK : ERR_FAILED;
#elif defined(OS_DARWIN)
    return OSSpinLockTry(pctx) ? ERR_OK : ERR_FAILED;
#else
    return pthread_spin_trylock(pctx);
#endif
};
/*
* \brief          解锁
*/
static inline void spin_unlock(spin_ctx *pctx)
{
#if defined(OS_WIN)
    LeaveCriticalSection(pctx);
#elif defined(OS_DARWIN)
    OSSpinLockUnlock(pctx)
#else
    ASSERTAB(ERR_OK == pthread_spin_unlock(pctx), ERRORSTR(ERRNO));
#endif
};

#endif//SPINLOCK_H_
