#include "mutex.h"

void mutex_init(mutex_ctx *pctx)
{
#if defined(OS_WIN)
    InitializeCriticalSection(pctx);
#else
    ASSERTAB(ERR_OK == pthread_mutex_init(pctx, (const pthread_mutexattr_t*)NULL),
        ERRORSTR(ERRNO));
#endif
}
void mutex_free(mutex_ctx *pctx)
{
#if defined(OS_WIN)
    DeleteCriticalSection(pctx);
#else
    (void)pthread_mutex_destroy(pctx);
#endif
}
void mutex_lock(mutex_ctx *pctx)
{
#if defined(OS_WIN)
    EnterCriticalSection(pctx);
#else
    ASSERTAB(ERR_OK == pthread_mutex_lock(pctx), ERRORSTR(ERRNO));
#endif
}
int32_t mutex_trylock(mutex_ctx *pctx)
{
#if defined(OS_WIN)
    return TRUE == TryEnterCriticalSection(pctx) ? ERR_OK : ERR_FAILED;
#else
    return pthread_mutex_trylock(pctx);
#endif
}
void mutex_unlock(mutex_ctx *pctx)
{
#if defined(OS_WIN)
    LeaveCriticalSection(pctx);
#else
    ASSERTAB(ERR_OK == pthread_mutex_unlock(pctx), ERRORSTR(ERRNO));
#endif
}
