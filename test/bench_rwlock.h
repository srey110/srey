#ifndef BENCH_RWLOCK_H_
#define BENCH_RWLOCK_H_

/// <summary>
/// rwlock_distr_ctx 与 rwlock_ctx 的多线程并发读性能基准对比。
/// 多线程短临界区只读场景下,distr 的 per-slot 读锁消除 cache-line 争用,
/// 普通 rwlock 因共享计数器产生争用。结果经 LOG_INFO 输出。
/// </summary>
void bench_rwlock(void);

#endif//BENCH_RWLOCK_H_
