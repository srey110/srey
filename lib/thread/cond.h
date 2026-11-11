#ifndef COND_H_
#define COND_H_

#include "thread/mutex.h"

#if defined(OS_WIN)
typedef CONDITION_VARIABLE cond_ctx;
#else
typedef pthread_cond_t cond_ctx;
#endif
// 条件变量能否绑定 CLOCK_MONOTONIC:要同时有 pthread_condattr_setclock(macOS 没有)与 CLOCK_MONOTONIC
// (老 HP-UX 没有,故按宏存在性判而非按 OS 排除)。cond_init 与 cond_timedwait 必须由这同一个宏分流:
// 两边时钟不一致时每次调用都立即 ETIMEDOUT,等待方变成静默忙等
#if !defined(OS_WIN) && !defined(OS_DARWIN) && defined(CLOCK_MONOTONIC)
    #define COND_MONOTONIC
#endif

/// <summary>
/// 条件变量初始化。定义了 COND_MONOTONIC 的平台绑定 CLOCK_MONOTONIC，绑定失败直接 abort（理由见 COND_MONOTONIC）；
/// 其余平台（macOS / 老 HP-UX）保持默认 CLOCK_REALTIME
/// </summary>
/// <param name="ctx">cond_ctx</param>
static inline void cond_init(cond_ctx *ctx) {
#if defined(OS_WIN)
    InitializeConditionVariable(ctx);
#elif defined(COND_MONOTONIC)
    // 绑定 CLOCK_MONOTONIC，与 tw/timer 统一时钟，避免 NTP 跳变导致实际睡眠时间异常
    pthread_condattr_t attr;
    ASSERTAB_CODE(pthread_condattr_init(&attr));
    ASSERTAB_CODE(pthread_condattr_setclock(&attr, CLOCK_MONOTONIC));
    ASSERTAB_CODE(pthread_cond_init(ctx, &attr));
    (void)pthread_condattr_destroy(&attr);
#else
    // 无 pthread_condattr_setclock(macOS) 或无 CLOCK_MONOTONIC(老 HP-UX)：保持默认 CLOCK_REALTIME
    ASSERTAB_CODE(pthread_cond_init(ctx, (const pthread_condattr_t*)NULL));
#endif
}
/// <summary>
/// 条件变量释放
/// </summary>
/// <param name="ctx">cond_ctx</param>
static inline void cond_free(cond_ctx *ctx) {
#if defined(OS_WIN)
#else
    (void)pthread_cond_destroy(ctx);
#endif
}
/// <summary>
/// 等待信号。可能虚假唤醒，返回后须重查条件
/// </summary>
/// <param name="ctx">cond_ctx</param>
/// <param name="mu">mutex_ctx</param>
static inline void cond_wait(cond_ctx *ctx, mutex_ctx *mu) {
#if defined(OS_WIN)
    ASSERTAB(SleepConditionVariableCS(ctx, mu, INFINITE), ERRORSTR(ERRNO));
#else
    ASSERTAB_CODE(pthread_cond_wait(ctx, mu));
#endif
}
// cond_timedwait 的错误只写 stderr、不走 LOG_*：它持着调用方的锁返回，那把锁可能正是日志线程 _waiter 的锁
// (waiter 用 mutex + cond 实现时)，走 LOG_* 会同线程重锁而自死锁。同 log.c 队列满 / 格式化失败时改写 stderr 的兜底
static inline void _cond_timedwait_erro(int32_t code) {
    fprintf(stderr, "[ERROR][cond.h cond_timedwait] code %d, %s\n", code, ERRORSTR(code));
    fflush(stderr);
}
/// <summary>
/// 等待信号，最多等 ms 毫秒。可能虚假唤醒，返回后须重查条件。出错时错误码写 stderr 而非日志系统（理由见 _cond_timedwait_erro）。
/// macOS 直接按相对时长等，不先读时钟换算截止时刻
/// </summary>
/// <param name="ctx">cond_ctx</param>
/// <param name="mu">mutex_ctx</param>
/// <param name="ms">毫秒</param>
/// <returns>ERR_OK 被叫醒或虚假唤醒；1 超时；ERR_FAILED 出错</returns>
static inline int32_t cond_timedwait(cond_ctx *ctx, mutex_ctx *mu, const uint32_t ms) {
#if defined(OS_WIN)
    if (!SleepConditionVariableCS(ctx, mu, (DWORD)ms)) {
        int32_t err = ERRNO;
        if (ERROR_TIMEOUT == err) {
            return 1;
        }
        _cond_timedwait_erro(err);
        return ERR_FAILED;
    }
    return ERR_OK;
#else
    int32_t rtn;
#if defined(OS_DARWIN)
    struct timespec rel;
    rel.tv_sec = (time_t)(ms / 1000);
    rel.tv_nsec = (long)(ms % 1000) * 1000000;
    rtn = pthread_cond_timedwait_relative_np(ctx, mu, &rel);
#else
    long seconds = ms / 1000;
    long nanoseconds = (ms % 1000) * 1000000;
    struct timespec timewait;
#if defined(COND_MONOTONIC)
    // 条件变量已绑定 CLOCK_MONOTONIC（见 cond_init），截止时间须与之同源
    clock_gettime(CLOCK_MONOTONIC, &timewait);
    timewait.tv_sec += seconds;
    timewait.tv_nsec += nanoseconds;
#else
    // 条件变量是默认的 CLOCK_REALTIME，截止时间用 gettimeofday 同源计算
    struct timeval now;
    gettimeofday(&now, NULL);
    timewait.tv_sec = now.tv_sec + seconds;
    timewait.tv_nsec = now.tv_usec * 1000 + nanoseconds;
#endif
    timewait.tv_sec += timewait.tv_nsec / 1000000000;
    timewait.tv_nsec %= 1000000000;
    rtn = pthread_cond_timedwait(ctx, mu, &timewait);
#endif
    if (0 == rtn) {
        return ERR_OK;
    }
    if (ETIMEDOUT == rtn) {
        return 1;
    }
    _cond_timedwait_erro(rtn);
    return ERR_FAILED;
#endif
}
/// <summary>
/// 激活信号
/// </summary>
/// <param name="ctx">cond_ctx</param>
static inline void cond_signal(cond_ctx *ctx) {
#if defined(OS_WIN)
    WakeConditionVariable(ctx);
#else
    ASSERTAB_CODE(pthread_cond_signal(ctx));
#endif
}
/// <summary>
/// 激活全部信号
/// </summary>
/// <param name="ctx">cond_ctx</param>
static inline void cond_broadcast(cond_ctx *ctx) {
#if defined(OS_WIN)
    WakeAllConditionVariable(ctx);
#else
    ASSERTAB_CODE(pthread_cond_broadcast(ctx));
#endif
}

#endif//COND_H_
