#include "rwlock.h"

void rwlock_init(struct rwlock_ctx *pctx)
{
#if defined(OS_WIN)
    InitializeSRWLock(&pctx->rwlock);
    pctx->wlock = 0;
#else
    ASSERTAB((ERR_OK == pthread_rwlock_init(&pctx->rwlock, NULL)),
        ERRORSTR(ERRNO));
#endif
}
void rwlock_free(struct rwlock_ctx *pctx)
{
#if defined(OS_WIN)
#else
    (void)pthread_rwlock_destroy(&pctx->rwlock);
#endif
}
void rwlock_rdlock(struct rwlock_ctx *pctx)
{
#if defined(OS_WIN)
    AcquireSRWLockShared(&pctx->rwlock);
#else
    ASSERTAB(ERR_OK == pthread_rwlock_rdlock(&pctx->rwlock), ERRORSTR(ERRNO));
#endif
}
int32_t rwlock_tryrdlock(struct rwlock_ctx *pctx)
{
#if defined(OS_WIN)
    return 0 != TryAcquireSRWLockShared(&pctx->rwlock) ? ERR_OK : ERR_FAILED;
#else
    return pthread_rwlock_tryrdlock(&pctx->rwlock);
#endif
}
void rwlock_wrlock(struct rwlock_ctx *pctx)
{
#if defined(OS_WIN)
    AcquireSRWLockExclusive(&pctx->rwlock);
    pctx->wlock = 1;
#else
    ASSERTAB(ERR_OK == pthread_rwlock_wrlock(&pctx->rwlock), ERRORSTR(ERRNO));
#endif
}
int32_t rwlock_trywrlock(struct rwlock_ctx *pctx)
{
#if defined(OS_WIN)
    if (0 != TryAcquireSRWLockExclusive(&pctx->rwlock))
    {
        pctx->wlock = 1;
        return ERR_OK;
    }

    return ERR_FAILED;
#else
    return pthread_rwlock_trywrlock(&pctx->rwlock);
#endif
}
void rwlock_unlock(struct rwlock_ctx *pctx)
{
#if defined(OS_WIN)
    if (0 != pctx->wlock)
    {
        pctx->wlock = 0;
        ReleaseSRWLockExclusive(&pctx->rwlock);
    }
    else
    {
        ReleaseSRWLockShared(&pctx->rwlock);
    }
#else
    ASSERTAB(ERR_OK == pthread_rwlock_unlock(&pctx->rwlock), ERRORSTR(ERRNO));
#endif
}
