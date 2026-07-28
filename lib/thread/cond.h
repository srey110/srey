#ifndef COND_H_
#define COND_H_

#include "thread/mutex.h"

#if defined(OS_WIN)
typedef CONDITION_VARIABLE cond_ctx;
#else
typedef pthread_cond_t cond_ctx;
#endif
// 条件变量能否绑定 CLOCK_MONOTONIC:需要 pthread_condattr_setclock(macOS 没有)
// 与 CLOCK_MONOTONIC 本身(HP-UX 11i v3 起有、11.23 及更早没有,故按宏存在性判而非按 OS 排除)
// 两个能力同时具备。cond_init 与 cond_timedwait
// 必须由同一个宏分流——两者对时钟的选择一旦不一致,截止时间就落在另一个时基上,
// 每次调用立即 ETIMEDOUT,等待方变成静默忙等
#if !defined(OS_WIN) && !defined(OS_DARWIN) && defined(CLOCK_MONOTONIC)
    #define COND_MONOTONIC
#endif
/// <summary>
/// 信号量初始化。具备 pthread_condattr_setclock 与 CLOCK_MONOTONIC 的 POSIX 平台强制
/// 把条件变量绑定到 CLOCK_MONOTONIC，绑定失败直接 abort：降级留在 CLOCK_REALTIME 会让
/// cond_timedwait 按 MONOTONIC 算出的截止时间永远处于过去，每次调用立即 ETIMEDOUT，
/// 把等待方变成静默忙等。缺任一能力的平台（macOS / 老 HP-UX）两端一致地用 CLOCK_REALTIME
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
    ASSERTAB(SleepConditionVariableCS(ctx, mu, INFINITE), ERRORSTR(ERRNO));
#else
    ASSERTAB_CODE(pthread_cond_wait(ctx, mu));
#endif
};
// cond_timedwait 的错误上报只能写 stderr，不能走 LOG_*：它是持调用方互斥量返回的，
// 而当那个互斥量正是 log.c 的 _mtx 时，slog 在消费者 _sleeping 置位期间会去锁同一个
// 互斥量，NORMAL 互斥量重锁即自死锁(glibc 永久阻塞并连带堵住所有写日志的线程，
// macOS 返 EDEADLK 触发 mutex_lock 的断言，Windows 因 CRITICAL_SECTION 可重入而无事)。
// 与 log.c 自身队列满/格式化失败时改写 stderr 的兜底同一策略
static inline void _cond_timedwait_erro(int32_t code) {
    fprintf(stderr, "[ERROR][cond.h cond_timedwait] code %d, %s\n", code, ERRORSTR(code));
    fflush(stderr);
}
/// <summary>
/// 等待信号。出错时错误码写 stderr 而非日志系统（详见 _cond_timedwait_erro）
/// </summary>
/// <param name="ctx">cond_ctx</param>
/// <param name="mu">mutex_ctx</param>
/// <param name="ms">毫秒</param>
/// <returns>ERR_OK成功 ERR_FAILED 错误 1 超时 </returns>
static inline int32_t cond_timedwait(cond_ctx *ctx, mutex_ctx *mu, const uint32_t ms) {
#if defined(OS_WIN)
    if (!SleepConditionVariableCS(ctx, mu, (DWORD)ms)) {//如果函数成功，则返回值为非零 如果函数失败或超时间隔已过，则返回值为零
        int32_t err = ERRNO;
        if (ERROR_TIMEOUT == err) {
            return 1;
        }
        _cond_timedwait_erro(err);
        return ERR_FAILED;
    }
    return ERR_OK;
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
    int32_t rtn = pthread_cond_timedwait(ctx, mu, &timewait);
    if (0 == rtn) {
        return ERR_OK;
    }
    if (ETIMEDOUT == rtn) {
        return 1;
    }
    _cond_timedwait_erro(rtn);
    return ERR_FAILED;
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
    ASSERTAB_CODE(pthread_cond_signal(ctx));
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
    ASSERTAB_CODE(pthread_cond_broadcast(ctx));
#endif
};

#endif//COND_H_
