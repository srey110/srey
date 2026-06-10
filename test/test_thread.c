#include "test_thread.h"
#include "lib.h"

#define _NTHREADS   8       /* 并发线程数 */
#define _NITER      10000   /* 每线程迭代次数 */

/* =======================================================================
 * mutex —— 互斥锁保护计数器
 * ======================================================================= */

typedef struct {
    mutex_ctx mu;
    int       counter;
} _mu_shared;

static void _mu_worker(void *arg) {
    _mu_shared *s = (_mu_shared *)arg;
    for (int i = 0; i < _NITER; i++) {
        mutex_lock(&s->mu);
        s->counter++;
        mutex_unlock(&s->mu);
    }
}

static void test_mutex(CuTest *tc) {
    _mu_shared s;
    mutex_init(&s.mu);
    s.counter = 0;

    pthread_t ths[_NTHREADS];
    for (int i = 0; i < _NTHREADS; i++) {
        ths[i] = thread_creat(_mu_worker, &s);
    }
    for (int i = 0; i < _NTHREADS; i++) {
        thread_join(ths[i]);
    }

    /* 所有线程累加后，结果必须精确 */
    CuAssertIntEquals(tc, _NTHREADS * _NITER, s.counter);

    /* trylock：未被锁时应成功，锁定后释放 */
    CuAssertTrue(tc, ERR_OK == mutex_trylock(&s.mu));
    mutex_unlock(&s.mu);

    mutex_free(&s.mu);
}

/* =======================================================================
 * spinlock —— 自旋锁保护计数器
 * ======================================================================= */

typedef struct {
    spin_ctx spin;
    int      counter;
} _spin_shared;

static void _spin_worker(void *arg) {
    _spin_shared *s = (_spin_shared *)arg;
    for (int i = 0; i < _NITER; i++) {
        spin_lock(&s->spin);
        s->counter++;
        spin_unlock(&s->spin);
    }
}

static void test_spinlock(CuTest *tc) {
    _spin_shared s;
    spin_init(&s.spin, 4000);
    s.counter = 0;

    pthread_t ths[_NTHREADS];
    for (int i = 0; i < _NTHREADS; i++) {
        ths[i] = thread_creat(_spin_worker, &s);
    }
    for (int i = 0; i < _NTHREADS; i++) {
        thread_join(ths[i]);
    }

    CuAssertIntEquals(tc, _NTHREADS * _NITER, s.counter);

    /* trylock */
    CuAssertTrue(tc, ERR_OK == spin_trylock(&s.spin));
    spin_unlock(&s.spin);

    spin_free(&s.spin);
}

/* =======================================================================
 * rwlock —— 读写锁：多读者并发，写者独占
 * ======================================================================= */

typedef struct {
    rwlock_ctx rw;
    int        value;       /* 受写锁保护的共享值 */
    atomic_t   readers;     /* 并发读者峰值 */
    atomic_t   max_readers;
} _rw_shared;

static void _rw_reader(void *arg) {
    _rw_shared *s = (_rw_shared *)arg;
    for (int i = 0; i < 200; i++) {
        rwlock_rdlock(&s->rw);
        /* 记录并发读者数量 */
        atomic_t r = ATOMIC_ADD(&s->readers, 1) + 1;
        /* 更新最大并发数 */
        atomic_t cur_max;
        do {
            cur_max = ATOMIC_GET(&s->max_readers);
            if (r <= cur_max) break;
        } while (!ATOMIC_CAS(&s->max_readers, cur_max, r));
        (void)s->value;           /* 读取值（测试无竞争）*/
        ATOMIC_ADD(&s->readers, -1);
        rwlock_unlock(&s->rw);
    }
}

static void _rw_writer(void *arg) {
    _rw_shared *s = (_rw_shared *)arg;
    for (int i = 0; i < 50; i++) {
        rwlock_wrlock(&s->rw);
        s->value++;
        rwlock_unlock(&s->rw);
    }
}

static void test_rwlock(CuTest *tc) {
    _rw_shared s;
    rwlock_init(&s.rw);
    s.value      = 0;
    s.readers    = 0;
    s.max_readers = 0;

    pthread_t rths[_NTHREADS];
    pthread_t wths[2];
    for (int i = 0; i < _NTHREADS; i++) {
        rths[i] = thread_creat(_rw_reader, &s);
    }
    for (int i = 0; i < 2; i++) {
        wths[i] = thread_creat(_rw_writer, &s);
    }
    for (int i = 0; i < _NTHREADS; i++) thread_join(rths[i]);
    for (int i = 0; i < 2; i++)         thread_join(wths[i]);

    /* 写者累计执行 2×50 次自增 */
    CuAssertIntEquals(tc, 2 * 50, s.value);

    /* trylock：读锁未被持有时可成功 */
    CuAssertTrue(tc, ERR_OK == rwlock_tryrdlock(&s.rw));
    rwlock_unlock(&s.rw);
    CuAssertTrue(tc, ERR_OK == rwlock_trywrlock(&s.rw));
    rwlock_unlock(&s.rw);

    rwlock_free(&s.rw);
}

/* =======================================================================
 * cond —— 条件变量：生产者-消费者
 * ======================================================================= */

#define _COND_ITEMS  1000

typedef struct {
    mutex_ctx mu;
    cond_ctx  cond;
    int       queue[_COND_ITEMS];
    int       head;
    int       tail;
    int       done;   /* 生产者结束标志 */
} _cond_shared;

static void _cond_producer(void *arg) {
    _cond_shared *s = (_cond_shared *)arg;
    for (int i = 0; i < _COND_ITEMS; i++) {
        mutex_lock(&s->mu);
        s->queue[s->tail % _COND_ITEMS] = i + 1;
        s->tail++;
        cond_signal(&s->cond);
        mutex_unlock(&s->mu);
    }
    mutex_lock(&s->mu);
    s->done = 1;
    cond_broadcast(&s->cond);
    mutex_unlock(&s->mu);
}

static int _sum_consumed;

static void _cond_consumer(void *arg) {
    _cond_shared *s = (_cond_shared *)arg;
    for (;;) {
        mutex_lock(&s->mu);
        while (s->head == s->tail && !s->done) {
            cond_wait(&s->cond, &s->mu);
        }
        while (s->head < s->tail) {
            _sum_consumed += s->queue[s->head % _COND_ITEMS];
            s->head++;
        }
        int finished = s->done && (s->head == s->tail);
        mutex_unlock(&s->mu);
        if (finished) break;
    }
}

static void test_cond(CuTest *tc) {
    _cond_shared s;
    mutex_init(&s.mu);
    cond_init(&s.cond);
    s.head = s.tail = s.done = 0;
    _sum_consumed = 0;

    /* 期望总和：1+2+...+_COND_ITEMS */
    int expected = _COND_ITEMS * (_COND_ITEMS + 1) / 2;

    pthread_t producer = thread_creat(_cond_producer, &s);
    pthread_t consumer = thread_creat(_cond_consumer, &s);
    thread_join(producer);
    thread_join(consumer);

    CuAssertIntEquals(tc, expected, _sum_consumed);

    /* timedwait 超时验证 */
    mutex_lock(&s.mu);
    int ret = cond_timedwait(&s.cond, &s.mu, 10); /* 等待 10ms，必然超时 */
    mutex_unlock(&s.mu);
    CuAssertTrue(tc, 1 == ret); /* 1 表示超时 */

    cond_free(&s.cond);
    mutex_free(&s.mu);
}

/* =======================================================================
 * thread —— 线程创建与等待
 * ======================================================================= */

static atomic_t _thread_counter;

static void _count_worker(void *arg) {
    int n = *(int *)arg;
    for (int i = 0; i < n; i++) {
        ATOMIC_ADD(&_thread_counter, 1);
    }
}

static void test_thread_basic(CuTest *tc) {
    _thread_counter = 0;

    int n = 5000;
    pthread_t ths[_NTHREADS];
    for (int i = 0; i < _NTHREADS; i++) {
        ths[i] = thread_creat(_count_worker, &n);
    }
    for (int i = 0; i < _NTHREADS; i++) {
        thread_join(ths[i]);
    }

    /* 原子累加，结果精确 */
    CuAssertTrue(tc, (uint32_t)(_NTHREADS * n) == ATOMIC_GET(&_thread_counter));
}

/* =======================================================================
 * rwlock_distr —— 分布式读锁:per-slot cache-line + fallback
 * ======================================================================= */

// 单线程基本路径:register → rdlock/runlock → wrlock/wrunlock → unregister
static void test_rwlock_distr_basic(CuTest *tc) {
    rwlock_distr_ctx ctx;
    rwlock_distr_init(&ctx, 4);
    CuAssertIntEquals(tc, ERR_OK, rwlock_distr_register(&ctx));
    rwlock_distr_rdlock(&ctx);
    rwlock_distr_runlock(&ctx);
    rwlock_distr_wrlock(&ctx);
    rwlock_distr_wrunlock(&ctx);
    rwlock_distr_unregister(&ctx);
    rwlock_distr_free(&ctx);
}

// 幂等:同实例重复 register/unregister 安全
static void test_rwlock_distr_idempotent(CuTest *tc) {
    rwlock_distr_ctx ctx;
    rwlock_distr_init(&ctx, 4);
    CuAssertIntEquals(tc, ERR_OK, rwlock_distr_register(&ctx));
    CuAssertIntEquals(tc, ERR_OK, rwlock_distr_register(&ctx));
    rwlock_distr_unregister(&ctx);
    rwlock_distr_unregister(&ctx);
    // 重新 register 仍可成功
    CuAssertIntEquals(tc, ERR_OK, rwlock_distr_register(&ctx));
    rwlock_distr_unregister(&ctx);
    rwlock_distr_free(&ctx);
}

// 多实例并发持有:同线程可同时注册多个不同 ctx,各自走快路径互不串扰
static void test_rwlock_distr_multi_register(CuTest *tc) {
    rwlock_distr_ctx a, b;
    rwlock_distr_init(&a, 4);
    rwlock_distr_init(&b, 4);
    CuAssertIntEquals(tc, ERR_OK, rwlock_distr_register(&a));
    CuAssertIntEquals(tc, ERR_OK, rwlock_distr_register(&b));
    // 嵌套 rdlock:a 与 b 走各自快路径,锁字段独立
    rwlock_distr_rdlock(&a);
    rwlock_distr_rdlock(&b);
    rwlock_distr_runlock(&b);
    rwlock_distr_runlock(&a);
    rwlock_distr_unregister(&b);
    rwlock_distr_unregister(&a);
    // unregister 后重新 register 仍可
    CuAssertIntEquals(tc, ERR_OK, rwlock_distr_register(&a));
    rwlock_distr_unregister(&a);
    rwlock_distr_free(&a);
    rwlock_distr_free(&b);
}

// TLS 数组耗尽:超过 RWLOCK_DISTR_MAX_TLS 的 ctx 注册失败,rdlock 走 fallback 仍能工作;
// 释放一个槽位后,新 ctx 可补位注册成功
static void test_rwlock_distr_tls_exhaust(CuTest *tc) {
    rwlock_distr_ctx ctxs[RWLOCK_DISTR_MAX_TLS + 1];
    int32_t i;
    for (i = 0; i < RWLOCK_DISTR_MAX_TLS + 1; i++) {
        rwlock_distr_init(&ctxs[i], 4);
    }
    // 前 MAX_TLS 个注册全部成功
    for (i = 0; i < RWLOCK_DISTR_MAX_TLS; i++) {
        CuAssertIntEquals(tc, ERR_OK, rwlock_distr_register(&ctxs[i]));
    }
    // 第 MAX_TLS+1 个超出 TLS 容量,返回失败
    CuAssertIntEquals(tc, ERR_FAILED, rwlock_distr_register(&ctxs[RWLOCK_DISTR_MAX_TLS]));
    // 注册失败的 ctx,rdlock 仍能走 fallback 不卡死
    rwlock_distr_rdlock(&ctxs[RWLOCK_DISTR_MAX_TLS]);
    rwlock_distr_runlock(&ctxs[RWLOCK_DISTR_MAX_TLS]);
    // 释放一个槽位,溢出 ctx 可补位
    rwlock_distr_unregister(&ctxs[0]);
    CuAssertIntEquals(tc, ERR_OK, rwlock_distr_register(&ctxs[RWLOCK_DISTR_MAX_TLS]));
    for (i = 1; i < RWLOCK_DISTR_MAX_TLS + 1; i++) {
        rwlock_distr_unregister(&ctxs[i]);
    }
    for (i = 0; i < RWLOCK_DISTR_MAX_TLS + 1; i++) {
        rwlock_distr_free(&ctxs[i]);
    }
}

// slot 池耗尽:slot=1,A 占用后 B 注册必失败,走 fallback rdlock 仍能工作
typedef struct {
    rwlock_distr_ctx *ctx;
    atomic_t a_done;
    atomic_t b_done;
    int b_reg_result;
} _distr_pool_shared;

static void _distr_pool_worker_a(void *arg) {
    _distr_pool_shared *s = (_distr_pool_shared *)arg;
    rwlock_distr_register(s->ctx);
    ATOMIC_SET(&s->a_done, 1);
    while (!ATOMIC_GET(&s->b_done)) {
        /* spin wait B */
    }
    rwlock_distr_unregister(s->ctx);
}

static void _distr_pool_worker_b(void *arg) {
    _distr_pool_shared *s = (_distr_pool_shared *)arg;
    while (!ATOMIC_GET(&s->a_done)) {
        /* spin wait A 占走 slot */
    }
    s->b_reg_result = rwlock_distr_register(s->ctx);
    // 即使注册失败,rdlock 也应能走 fallback 不卡死
    rwlock_distr_rdlock(s->ctx);
    rwlock_distr_runlock(s->ctx);
    rwlock_distr_unregister(s->ctx);
    ATOMIC_SET(&s->b_done, 1);
}

static void test_rwlock_distr_pool_exhausted(CuTest *tc) {
    rwlock_distr_ctx ctx;
    rwlock_distr_init(&ctx, 1);
    _distr_pool_shared s;
    s.ctx = &ctx;
    s.a_done = 0;
    s.b_done = 0;
    s.b_reg_result = 999;
    pthread_t a = thread_creat(_distr_pool_worker_a, &s);
    pthread_t b = thread_creat(_distr_pool_worker_b, &s);
    thread_join(b);
    thread_join(a);
    CuAssertIntEquals(tc, ERR_FAILED, s.b_reg_result);
    rwlock_distr_free(&ctx);
}

// 未注册线程 rdlock 走 fallback,功能正确
static void _distr_fallback_worker(void *arg) {
    rwlock_distr_ctx *ctx = (rwlock_distr_ctx *)arg;
    // 故意不调 register
    int i;
    for (i = 0; i < 200; i++) {
        rwlock_distr_rdlock(ctx);
        rwlock_distr_runlock(ctx);
    }
}

static void test_rwlock_distr_fallback(CuTest *tc) {
    rwlock_distr_ctx ctx;
    rwlock_distr_init(&ctx, 4);
    pthread_t ths[4];
    int i;
    for (i = 0; i < 4; i++) {
        ths[i] = thread_creat(_distr_fallback_worker, &ctx);
    }
    for (i = 0; i < 4; i++) {
        thread_join(ths[i]);
    }
    // 主线程也跑一遍 fallback
    rwlock_distr_rdlock(&ctx);
    rwlock_distr_runlock(&ctx);
    rwlock_distr_free(&ctx);
    CuAssertTrue(tc, 1); // 跑通即通过
}

// 读写互斥:多 reader + writer 并发,验证 writer 持锁时无 reader,反之亦然
typedef struct {
    rwlock_distr_ctx ctx;
    atomic_t reader_count;
    atomic_t writer_active;
    atomic_t max_readers;
    atomic_t violation;
    atomic_t started;
} _distr_mu_shared;

static void _distr_mu_reader(void *arg) {
    _distr_mu_shared *s = (_distr_mu_shared *)arg;
    rwlock_distr_register(&s->ctx);
    // barrier: 等所有 reader 到齐再同时开读, 否则极短临界区 + 串行创建可能从不重叠
    ATOMIC_ADD(&s->started, 1);
    while (ATOMIC_GET(&s->started) < _NTHREADS) {
        THREAD_YIELD();
    }
    int i, spin;
    for (i = 0; i < 500; i++) {
        rwlock_distr_rdlock(&s->ctx);
        // 进入临界区:统计并发读者
        atomic_t r = ATOMIC_ADD(&s->reader_count, 1) + 1;
        atomic_t cur;
        do {
            cur = ATOMIC_GET(&s->max_readers);
            if (r <= cur) break;
        } while (!ATOMIC_CAS(&s->max_readers, cur, r));
        // 持读锁期间让出, 使其他 reader 也进临界区, 确定性形成 reader_count>=2
        for (spin = 0; spin < 64 && ATOMIC_GET(&s->reader_count) < 2; spin++) {
            THREAD_YIELD();
        }
        // 读时不应有 writer
        if (ATOMIC_GET(&s->writer_active)) {
            ATOMIC_ADD(&s->violation, 1);
        }
        ATOMIC_ADD(&s->reader_count, -1);
        rwlock_distr_runlock(&s->ctx);
    }
    rwlock_distr_unregister(&s->ctx);
}

static void _distr_mu_writer(void *arg) {
    _distr_mu_shared *s = (_distr_mu_shared *)arg;
    rwlock_distr_register(&s->ctx);
    int i;
    for (i = 0; i < 50; i++) {
        rwlock_distr_wrlock(&s->ctx);
        ATOMIC_SET(&s->writer_active, 1);
        // 写时不应有 reader
        if (ATOMIC_GET(&s->reader_count) > 0) {
            ATOMIC_ADD(&s->violation, 1);
        }
        ATOMIC_SET(&s->writer_active, 0);
        rwlock_distr_wrunlock(&s->ctx);
    }
    rwlock_distr_unregister(&s->ctx);
}

static void test_rwlock_distr_mutex_check(CuTest *tc) {
    _distr_mu_shared s;
    rwlock_distr_init(&s.ctx, _NTHREADS + 4);
    s.reader_count = 0;
    s.writer_active = 0;
    s.max_readers = 0;
    s.violation = 0;
    s.started = 0;
    pthread_t rths[_NTHREADS];
    pthread_t wths[2];
    int i;
    for (i = 0; i < _NTHREADS; i++) {
        rths[i] = thread_creat(_distr_mu_reader, &s);
    }
    for (i = 0; i < 2; i++) {
        wths[i] = thread_creat(_distr_mu_writer, &s);
    }
    for (i = 0; i < _NTHREADS; i++) thread_join(rths[i]);
    for (i = 0; i < 2; i++)         thread_join(wths[i]);
    // 读写互斥必须严格,违反计数为 0
    CuAssertIntEquals(tc, 0, (int)ATOMIC_GET(&s.violation));
    // 应观察到并发 reader > 1
    CuAssertTrue(tc, ATOMIC_GET(&s.max_readers) > 1);
    rwlock_distr_free(&s.ctx);
}

// slot 复用:slot=2,串行起 10 个线程,unregister 后 slot 必须能被新线程拿到
static atomic_t _distr_reuse_ok;

static void _distr_reuse_worker(void *arg) {
    rwlock_distr_ctx *ctx = (rwlock_distr_ctx *)arg;
    if (ERR_OK == rwlock_distr_register(ctx)) {
        rwlock_distr_rdlock(ctx);
        rwlock_distr_runlock(ctx);
        rwlock_distr_unregister(ctx);
        ATOMIC_ADD(&_distr_reuse_ok, 1);
    }
}

static void test_rwlock_distr_slot_reuse(CuTest *tc) {
    rwlock_distr_ctx ctx;
    rwlock_distr_init(&ctx, 2);
    _distr_reuse_ok = 0;
    int i;
    for (i = 0; i < 10; i++) {
        pthread_t th = thread_creat(_distr_reuse_worker, &ctx);
        thread_join(th);
    }
    // 10 个串行线程都应注册成功(slot 反复复用)
    CuAssertIntEquals(tc, 10, (int)ATOMIC_GET(&_distr_reuse_ok));
    rwlock_distr_free(&ctx);
}

/* ======================================================================= */

void test_thread(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_mutex);
    SUITE_ADD_TEST(suite, test_spinlock);
    SUITE_ADD_TEST(suite, test_rwlock);
    SUITE_ADD_TEST(suite, test_cond);
    SUITE_ADD_TEST(suite, test_thread_basic);
    SUITE_ADD_TEST(suite, test_rwlock_distr_basic);
    SUITE_ADD_TEST(suite, test_rwlock_distr_idempotent);
    SUITE_ADD_TEST(suite, test_rwlock_distr_multi_register);
    SUITE_ADD_TEST(suite, test_rwlock_distr_tls_exhaust);
    SUITE_ADD_TEST(suite, test_rwlock_distr_pool_exhausted);
    SUITE_ADD_TEST(suite, test_rwlock_distr_fallback);
    SUITE_ADD_TEST(suite, test_rwlock_distr_mutex_check);
    SUITE_ADD_TEST(suite, test_rwlock_distr_slot_reuse);
}
