#ifndef COND_H_
#define COND_H_

#include "thread/mutex.h"

#if defined(OS_WIN)
typedef CONDITION_VARIABLE cond_ctx;
#else
typedef pthread_cond_t cond_ctx;
#endif
/// <summary>
/// 信号量初始化
/// </summary>
/// <param name="ctx">cond_ctx</param>
static inline void cond_init(cond_ctx *ctx) {
#if defined(OS_WIN)
    InitializeConditionVariable(ctx);
#else
    ASSERTAB(ERR_OK == pthread_cond_init(ctx, (const pthread_condattr_t*)NULL),
        ERRORSTR(ERRNO));
#endif
};
/// <summary>
/// 信号量释放
/// </summary>
/// <param name="ctx">cond_ctx</param>
static inline void cond_free(cond_ctx *ctx) {
#if defined(OS_WIN)
#else
    (void)pthread_cond_destroy(ctx);
#endif
};
/// <summary>
/// 等待信号
/// </summary>
/// <param name="ctx">cond_ctx</param>
/// <param name="mu">mutex_ctx</param>
static inline void cond_wait(cond_ctx *ctx, mutex_ctx *mu) {
#if defined(OS_WIN)
    ASSERTAB(SleepConditionVariableCS(ctx, mu, INFINITE),
        ERRORSTR(ERRNO));
#else
    ASSERTAB(ERR_OK == pthread_cond_wait(ctx, mu),
        ERRORSTR(ERRNO));
#endif
};
/// <summary>
/// 等待信号
/// </summary>
/// <param name="ctx">cond_ctx</param>
/// <param name="mu">mutex_ctx</param>
/// <param name="ms">毫秒</param>
static inline void cond_timedwait(cond_ctx *ctx, mutex_ctx *mu, const uint32_t ms) {
#if defined(OS_WIN)
    if (!SleepConditionVariableCS(ctx, mu, (DWORD)ms)) {
        int32_t err = ERRNO;
        ASSERTAB(ERROR_TIMEOUT == err, ERRORSTR(err));
    }
#else
    long seconds = ms / 1000;
    long nanoseconds = (ms - seconds * 1000) * 1000000;
    struct timeval now;
    gettimeofday(&now, NULL);
    struct timespec timewait;
    timewait.tv_sec = now.tv_sec + seconds;
    timewait.tv_nsec = now.tv_usec * 1000 + nanoseconds;
    if (timewait.tv_nsec >= 1000000000) {
        timewait.tv_nsec -= 1000000000;
        timewait.tv_sec++;
    }
    int32_t rtn = pthread_cond_timedwait(ctx, mu, &timewait);
    ASSERTAB((ERR_OK == rtn || ETIMEDOUT == rtn), ERRORSTR(ERRNO));
#endif
};
/// <summary>
/// 激活信号
/// </summary>
/// <param name="ctx">cond_ctx</param>
static inline void cond_signal(cond_ctx *ctx) {
#if defined(OS_WIN)
    WakeConditionVariable(ctx);
#else
    ASSERTAB(ERR_OK == pthread_cond_signal(ctx), ERRORSTR(ERRNO));
#endif
};
/// <summary>
/// 激活全部信号
/// </summary>
/// <param name="ctx">cond_ctx</param>
static inline void cond_broadcast(cond_ctx *ctx) {
#if defined(OS_WIN)
    WakeAllConditionVariable(ctx);
#else
    ASSERTAB(ERR_OK == pthread_cond_broadcast(ctx), ERRORSTR(ERRNO));
#endif
};

#endif//COND_H_
