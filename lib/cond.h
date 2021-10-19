#ifndef COND_H_
#define COND_H_

#include "mutex.h"

#if defined(OS_WIN)
typedef CONDITION_VARIABLE cond_ctx;
#else
typedef pthread_cond_t cond_ctx;
#endif
/*
* \brief          初始化
*/
static inline void cond_init(cond_ctx *pctx)
{
#if defined(OS_WIN)
    InitializeConditionVariable(pctx);
#else
    ASSERTAB(ERR_OK == pthread_cond_init(pctx, (const pthread_condattr_t*)NULL),
        ERRORSTR(ERRNO));
#endif
};
/*
* \brief          释放
*/
static inline void cond_free(cond_ctx *pctx)
{
#if defined(OS_WIN)
#else
    (void)pthread_cond_destroy(pctx);
#endif
};
/*
* \brief          等待信号量
*/
static inline void cond_wait(cond_ctx *pctx, mutex_ctx *pmutex)
{
#if defined(OS_WIN)
    ASSERTAB(SleepConditionVariableCS(pctx, pmutex, INFINITE),
        ERRORSTR(ERRNO));
#else
    ASSERTAB(ERR_OK == pthread_cond_wait(pctx, pmutex),
        ERRORSTR(ERRNO));
#endif
};
/*
* \brief          等待信号量
* \param uims     等待时间  毫秒
*/
static inline void cond_timedwait(cond_ctx *pctx, mutex_ctx *pmutex, const uint32_t uims)
{
#if defined(OS_WIN)
    if (!SleepConditionVariableCS(pctx, pmutex, (DWORD)uims))
    {
        int32_t ierror = ERRNO;
        ASSERTAB(ERROR_TIMEOUT == ierror, ERRORSTR(ierror));
    }
#else
    long seconds = uims / 1000;
    long nanoseconds = (uims - seconds * 1000) * 1000000;
    struct timeval now;

    gettimeofday(&now, NULL);

    struct timespec timewait;
    timewait.tv_sec = now.tv_sec + seconds;
    timewait.tv_nsec = now.tv_usec * 1000 + nanoseconds;
    if (timewait.tv_nsec >= 1000000000)
    {
        timewait.tv_nsec -= 1000000000;
        timewait.tv_sec++;
    }

    int32_t irtn = pthread_cond_timedwait(pctx, pmutex, &timewait);
    ASSERTAB((ERR_OK == irtn || ETIMEDOUT == irtn), ERRORSTR(ERRNO));
#endif
};
/*
* \brief          激活信号
*/
static inline void cond_signal(cond_ctx *pctx)
{
#if defined(OS_WIN)
    WakeConditionVariable(pctx);
#else
    ASSERTAB(ERR_OK == pthread_cond_signal(pctx), ERRORSTR(ERRNO));
#endif
};
/*
* \brief          激活所有信号
*/
static inline void cond_broadcast(cond_ctx *pctx)
{
#if defined(OS_WIN)
    WakeAllConditionVariable(pctx);
#else
    ASSERTAB(ERR_OK == pthread_cond_broadcast(pctx), ERRORSTR(ERRNO));
#endif
};

#endif//COND_H_
