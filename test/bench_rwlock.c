#include "bench_rwlock.h"
#include "lib.h"
#include "thread/thread.h"
#include "thread/rwlock.h"
#include "thread/rwlock_distr.h"

// 并发读线程数
#define BR_THREADS 8
// 每线程读锁循环次数
#define BR_ITERS 1000000

typedef struct br_arg {
    int32_t type;             // 0=rwlock_ctx, 1=rwlock_distr_ctx
    int32_t iters;            // 读循环次数
    void *lock;               // rwlock_ctx* 或 rwlock_distr_ctx*
    volatile int64_t *shared; // 临界区读取的共享数据
    int64_t sink;             // 累加结果,防整个循环被优化掉
}br_arg;

// 读线程:distr 先注册 slot;循环 rdlock → 读共享 → 解锁
static void _br_thread(void *ud) {
    br_arg *a = (br_arg *)ud;
    if (1 == a->type) {
        rwlock_distr_register((rwlock_distr_ctx *)a->lock);
    }
    int64_t local = 0;
    for (int32_t i = 0; i < a->iters; i++) {
        if (1 == a->type) {
            rwlock_distr_rdlock((rwlock_distr_ctx *)a->lock);
            local += *a->shared;
            rwlock_distr_runlock((rwlock_distr_ctx *)a->lock);
        } else {
            rwlock_rdlock((rwlock_ctx *)a->lock);
            local += *a->shared;
            rwlock_unlock((rwlock_ctx *)a->lock);
        }
    }
    if (1 == a->type) {
        rwlock_distr_unregister((rwlock_distr_ctx *)a->lock);
    }
    a->sink = local;
}
// 起 nthreads 个读线程并发跑 + join,返回总耗时 ms
static uint64_t _br_run(int32_t type, void *lock, int32_t nthreads, volatile int64_t *shared) {
    br_arg args[BR_THREADS];
    pthread_t ths[BR_THREADS];
    for (int32_t i = 0; i < nthreads; i++) {
        args[i].type = type;
        args[i].iters = BR_ITERS;
        args[i].lock = lock;
        args[i].shared = shared;
        args[i].sink = 0;
    }
    uint64_t t0 = nowms();
    for (int32_t i = 0; i < nthreads; i++) {
        ths[i] = thread_creat(_br_thread, &args[i]);
    }
    for (int32_t i = 0; i < nthreads; i++) {
        thread_join(ths[i]);
    }
    uint64_t cost = nowms() - t0;
    int64_t sink = 0;
    for (int32_t i = 0; i < nthreads; i++) {
        sink += args[i].sink;
    }
    if (sink < 0) {   // 恒不成立(shared>0),仅为防止上面整段循环被优化掉
        PRINT("%lld", (long long)sink);
    }
    return cost;
}
void bench_rwlock(void) {
    volatile int64_t shared = 42;
    int32_t nthreads = BR_THREADS;

    rwlock_distr_ctx dctx;
    rwlock_distr_init(&dctx, (uint32_t)nthreads + 4);
    uint64_t distr_ms = _br_run(1, &dctx, nthreads, &shared);
    rwlock_distr_free(&dctx);

    rwlock_ctx rctx;
    rwlock_init(&rctx);
    uint64_t rwlock_ms = _br_run(0, &rctx, nthreads, &shared);
    rwlock_free(&rctx);

    double speedup = (distr_ms > 0) ? (double)rwlock_ms / (double)distr_ms : 0.0;
    LOG_INFO("[bench_rwlock] threads=%d iters/thread=%d (read-only, short crit)", nthreads, BR_ITERS);
    LOG_INFO("[bench_rwlock] rwlock_distr=%llums rwlock=%llums distr-speedup=%.2fx",
             (unsigned long long)distr_ms, (unsigned long long)rwlock_ms, speedup);
}
