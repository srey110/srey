#include "utils/timer.h"
#if defined(OS_WIN)
#include <intrin.h>
#endif

#define NANOSEC 1000000000
#define NS_PER_MS 1000000 // 纳秒到毫秒的换算,不是可调精度
// 有 64x64→128 位乘法可用时，除不尽的换算走定点乘；没有(32 位 MSVC)就照旧两次除法
#if defined(__SIZEOF_INT128__) || defined(_M_ARM64) || defined(_M_X64)
    #define TIMER_MULSHIFT
#endif

#if defined(OS_WIN) || defined(OS_DARWIN)
// 预算 numer / denom 的定点乘数：mult = numer * 2^shift / denom 取整，逐位长除法免得要 128 位除法；
// shift 取到 mult 不溢出为止，误差不超过 ticks / 2^shift 纳秒
static void _timer_mult(timer_ctx *ctx, uint64_t numer, uint64_t denom) {
    uint64_t q = numer / denom;
    uint64_t rem = numer % denom;
    uint32_t shift = 0;
    while (shift < 63 && q < (1ULL << 62)) {
        q <<= 1;
        rem <<= 1;
        if (rem >= denom) {
            rem -= denom;
            q |= 1;
        }
        shift++;
    }
    ctx->mult = q;
    ctx->shift = shift;
}
#ifdef TIMER_MULSHIFT
// ticks * mult >> shift，乘积按 128 位算
static inline uint64_t _timer_mulshift(uint64_t ticks, uint64_t mult, uint32_t shift) {
#if defined(__SIZEOF_INT128__)
    return (uint64_t)(((unsigned __int128)ticks * mult) >> shift);
#elif defined(_M_ARM64)
    return (__umulh(ticks, mult) << (64 - shift)) | ((ticks * mult) >> shift);
#else
    uint64_t hi;
    uint64_t lo = _umul128(ticks, mult, &hi);
    return (hi << (64 - shift)) | (lo >> shift);
#endif
}
#endif
#endif
void timer_init(timer_ctx *ctx) {
#if defined(OS_WIN)
    LARGE_INTEGER freq;
    ASSERTAB(QueryPerformanceFrequency(&freq), ERRORSTR(ERRNO));
    ctx->freq = (uint64_t)freq.QuadPart;
    ctx->nsfactor = (0 == NANOSEC % ctx->freq) ? (NANOSEC / ctx->freq) : 0;
    _timer_mult(ctx, NANOSEC, ctx->freq);
#elif defined(OS_DARWIN)
    mach_timebase_info_data_t timebase;
    ASSERTAB(KERN_SUCCESS == mach_timebase_info(&timebase), ERRORSTR(ERRNO));
    ctx->numer = timebase.numer;
    ctx->denom = timebase.denom;
    ctx->nsfactor = (0 == ctx->numer % ctx->denom) ? (ctx->numer / ctx->denom) : 0;
    _timer_mult(ctx, ctx->numer, ctx->denom);
#endif
    timer_start(ctx);
}
uint64_t timer_cur(timer_ctx *ctx) {
#if defined(OS_WIN)
    LARGE_INTEGER now;
    ASSERTAB(QueryPerformanceCounter(&now), ERRORSTR(ERRNO));
    uint64_t ticks = (uint64_t)now.QuadPart;
    if (0 != ctx->nsfactor) {
        return ticks * ctx->nsfactor;
    }
#ifdef TIMER_MULSHIFT
    return _timer_mulshift(ticks, ctx->mult, ctx->shift);
#else
    return (ticks / ctx->freq) * NANOSEC + (ticks % ctx->freq) * NANOSEC / ctx->freq;
#endif
#elif defined(OS_AIX)
    (void)ctx;
    timebasestruct_t t;
    read_wall_time(&t, TIMEBASE_SZ);
    time_base_to_time(&t, TIMEBASE_SZ);
    return (((uint64_t)t.tb_high) * NANOSEC + t.tb_low);
#elif defined(OS_SUN)
    (void)ctx;
    return gethrtime();
#elif defined(OS_DARWIN)
    uint64_t ticks = mach_continuous_time();
    if (0 != ctx->nsfactor) {
        return ticks * ctx->nsfactor;
    }
#ifdef TIMER_MULSHIFT
    return _timer_mulshift(ticks, ctx->mult, ctx->shift);
#else
    return (ticks / ctx->denom) * ctx->numer + (ticks % ctx->denom) * ctx->numer / ctx->denom;
#endif
#else
    (void)ctx;
    struct timespec ts;
#if defined(CLOCK_MONOTONIC)
    clock_gettime(CLOCK_MONOTONIC, &ts);
#else
    clock_gettime(CLOCK_REALTIME, &ts);
#endif
    return (((uint64_t)ts.tv_sec) * NANOSEC + ts.tv_nsec);
#endif
}
uint64_t timer_cur_ms(timer_ctx *ctx) {
    return timer_cur(ctx) / NS_PER_MS;
}
uint64_t timer_thread_cpu_ns(void) {
#if defined(OS_WIN)
    FILETIME create, exit, kernel, user;
    GetThreadTimes(GetCurrentThread(), &create, &exit, &kernel, &user);
    uint64_t k = ((uint64_t)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime;
    uint64_t u = ((uint64_t)user.dwHighDateTime << 32) | user.dwLowDateTime;
    return (k + u) * 100;
#else
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (((uint64_t)ts.tv_sec) * NANOSEC + ts.tv_nsec);
#endif
}
void timer_start(timer_ctx *ctx) {
    ctx->starttick = timer_cur(ctx);
}
uint64_t timer_elapsed(timer_ctx *ctx) {
    return timer_cur(ctx) - ctx->starttick;
}
uint64_t timer_elapsed_ms(timer_ctx *ctx) {
    return (timer_cur(ctx) - ctx->starttick) / NS_PER_MS;
}
