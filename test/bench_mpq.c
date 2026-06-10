#include "bench_mpq.h"
#include "lib.h"
#include "thread/thread.h"
#include "thread/spinlock.h"
#include "containers/mpq.h"
#include "containers/queue.h"

// 每个场景固定的总入队元素数(便于横向比较)
#define BQ_TOTAL 4000000
// 线程数组上限(最大生产者数)
#define BQ_MAXPROD 8

typedef struct bq_arg {
    int32_t type;       // 0=queue+spinlock, 1=mpq
    int32_t n;          // 生产者:push 数;消费者:pop 数
    void *q;            // mpq_ctx* 或 queue_ctx*
    spin_ctx *lock;     // queue 用的自旋锁(mpq 为 NULL)
}bq_arg;

// 生产者:push n 个 int32_t;mpq 无锁,queue 走 spinlock
static void _bq_producer(void *ud) {
    bq_arg *a = (bq_arg *)ud;
    for (int32_t i = 0; i < a->n; i++) {
        if (1 == a->type) {
            mpq_push((mpq_ctx *)a->q, &i);
        } else {
            spin_lock(a->lock);
            queue_push((queue_ctx *)a->q, &i);
            spin_unlock(a->lock);
        }
    }
}
// 消费者:pop 直到取满 n 个;mpq 走单消费者无锁 pop_sc,queue 走 spinlock
static void _bq_consumer(void *ud) {
    bq_arg *a = (bq_arg *)ud;
    int32_t got = 0;
    int32_t val;
    void *p;
    while (got < a->n) {
        if (1 == a->type) {
            if (ERR_OK == mpq_pop_sc((mpq_ctx *)a->q, &val)) {
                got++;
            }
        } else {
            spin_lock(a->lock);
            p = queue_pop((queue_ctx *)a->q);
            spin_unlock(a->lock);
            if (NULL != p) {
                got++;
            }
        }
    }
}
// 纯 push:nprod 个生产者各 push per,join,返回耗时
static uint64_t _bq_push(int32_t type, void *q, spin_ctx *lock, int32_t nprod, int32_t per) {
    bq_arg args[BQ_MAXPROD];
    pthread_t ths[BQ_MAXPROD];
    for (int32_t i = 0; i < nprod; i++) {
        args[i].type = type; args[i].n = per; args[i].q = q; args[i].lock = lock;
    }
    uint64_t t0 = nowms();
    for (int32_t i = 0; i < nprod; i++) {
        ths[i] = thread_creat(_bq_producer, &args[i]);
    }
    for (int32_t i = 0; i < nprod; i++) {
        thread_join(ths[i]);
    }
    return nowms() - t0;
}
// MPSC:nprod 个生产者各 push per + 1 个消费者 pop (nprod*per),全部并发,返回耗时
static uint64_t _bq_mpsc(int32_t type, void *q, spin_ctx *lock, int32_t nprod, int32_t per) {
    bq_arg pargs[BQ_MAXPROD];
    pthread_t pths[BQ_MAXPROD];
    bq_arg carg;
    pthread_t cth;
    carg.type = type; carg.n = nprod * per; carg.q = q; carg.lock = lock;
    for (int32_t i = 0; i < nprod; i++) {
        pargs[i].type = type; pargs[i].n = per; pargs[i].q = q; pargs[i].lock = lock;
    }
    uint64_t t0 = nowms();
    cth = thread_creat(_bq_consumer, &carg);
    for (int32_t i = 0; i < nprod; i++) {
        pths[i] = thread_creat(_bq_producer, &pargs[i]);
    }
    for (int32_t i = 0; i < nprod; i++) {
        thread_join(pths[i]);
    }
    thread_join(cth);
    return nowms() - t0;
}
// 跑一组(指定生产者数)的 mpq 与 queue+spin 对比,经回调选择 push / mpsc
static void _bq_one(const char *tag, int32_t nprod,
                    uint64_t(*run)(int32_t, void *, spin_ctx *, int32_t, int32_t)) {
    int32_t per = BQ_TOTAL / nprod;
    mpq_ctx mq;
    mpq_init(&mq, sizeof(int32_t), BQ_TOTAL);
    uint64_t mpq_ms = run(1, &mq, NULL, nprod, per);
    mpq_free(&mq);
    queue_ctx qu;
    queue_init(&qu, sizeof(int32_t), BQ_TOTAL);
    spin_ctx lock;
    spin_init(&lock, 0);
    uint64_t qu_ms = run(0, &qu, &lock, nprod, per);
    spin_free(&lock);
    queue_free(&qu);
    double sp = (mpq_ms > 0) ? (double)qu_ms / (double)mpq_ms : 0.0;
    LOG_INFO("[bench_mpq] %s producers=%d mpq=%llums queue+spin=%llums mpq-speedup=%.2fx",
             tag, nprod, (unsigned long long)mpq_ms, (unsigned long long)qu_ms, sp);
}
void bench_mpq(void) {
    int32_t prods[] = { 1, 2, 4, 8 };
    int32_t np = (int32_t)(sizeof(prods) / sizeof(prods[0]));
    int32_t k;
    // 维度 1:纯多生产者 push(总量固定,变生产者数看争用曲线)
    LOG_INFO("[bench_mpq] === push-only, total=%d (vary producers) ===", BQ_TOTAL);
    for (k = 0; k < np; k++) {
        _bq_one("push", prods[k], _bq_push);
    }
    // 维度 2:MPSC(N 生产者 + 1 消费者并发;mpq push/pop 均无锁,queue 全程争同一锁)
    LOG_INFO("[bench_mpq] === MPSC, total=%d (N producers + 1 consumer) ===", BQ_TOTAL);
    for (k = 0; k < np; k++) {
        _bq_one("mpsc", prods[k], _bq_mpsc);
    }
}
