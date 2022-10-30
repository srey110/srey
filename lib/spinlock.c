#include "spinlock.h"

void spin_init(spin_ctx *pctx, const uint32_t uispcnt)
{
#if defined(OS_WIN)
    ASSERTAB(InitializeCriticalSectionAndSpinCount(pctx, uispcnt), ERRORSTR(ERRNO));
#elif defined(OS_DARWIN)
    *pctx = OS_UNFAIR_LOCK_INIT;
#else
    ASSERTAB(ERR_OK == pthread_spin_init(pctx, PTHREAD_PROCESS_PRIVATE), ERRORSTR(ERRNO));
#endif
}
void spin_free(spin_ctx *pctx)
{
#if defined(OS_WIN)
    DeleteCriticalSection(pctx);
#elif defined(OS_DARWIN)
#else
    (void)pthread_spin_destroy(pctx);
#endif
}
void spin_lock(spin_ctx *pctx)
{
#if defined(OS_WIN)
    EnterCriticalSection(pctx);
#elif defined(OS_DARWIN)
    os_unfair_lock_lock(pctx);
#else
    ASSERTAB(ERR_OK == pthread_spin_lock(pctx), ERRORSTR(ERRNO));
#endif
}
int32_t spin_trylock(spin_ctx *pctx)
{
#if defined(OS_WIN)
    return TRUE == TryEnterCriticalSection(pctx) ? ERR_OK : ERR_FAILED;
#elif defined(OS_DARWIN)
    return os_unfair_lock_trylock(pctx) ? ERR_OK : ERR_FAILED;
#else
    return pthread_spin_trylock(pctx);
#endif
}
void spin_unlock(spin_ctx *pctx)
{
#if defined(OS_WIN)
    LeaveCriticalSection(pctx);
#elif defined(OS_DARWIN)
    os_unfair_lock_unlock(pctx);
#else
    ASSERTAB(ERR_OK == pthread_spin_unlock(pctx), ERRORSTR(ERRNO));
#endif
}
