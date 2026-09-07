#include "test_thread.h"
#include "lib.h"

#define _NTHREADS   8/* 并发线程数 */
#define _NITER      10000/* 每线程迭代次数 */
// 兜底：锁真坏了要失败，不能挂死在等标志的自旋里
#define _DISTR_MAXMS 30000
/* cond 生产者-消费者的消息条数 */
#define _COND_ITEMS  1000

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
    atomic_t   readers;     /* 当前并发读者数 */
    atomic_t   max_readers; /* 并发读者峰值 */
    atomic_t   writer_in;   /* 写者正在临界区内 */
    atomic_t   violation;   /* 读者撞见写者 / 写者撞见读者的次数 */
} _rw_shared;

static void _rw_reader(void *arg) {
    _rw_shared *s = (_rw_shared *)arg;
    atomic_t r, cur_max;
    uint64_t deadline;
    int i;
    for (i = 0; i < 200; i++) {
        rwlock_rdlock(&s->rw);
        /* 持读锁期间不得有写者在里面。没有这条的话，把 rdlock/unlock 换成空函数也全过 */
        if (0 != ATOMIC_GET(&s->writer_in)) {
            ATOMIC_ADD(&s->violation, 1);
        }
        r = ATOMIC_ADD(&s->readers, 1) + 1;
        /* 第一轮在临界区里等一个同伴：读锁允许并发时必然等到，
           退化成排他锁时全都等到超时，max_readers 停在 1 —— 下面那条断言据此判 */
        if (0 == i) {
            deadline = nowms() + 200;
            while (ATOMIC_GET(&s->readers) < 2 && nowms() < deadline) {
                MSLEEP(1);
            }
        }
        r = ATOMIC_GET(&s->readers);
        do {
            cur_max = ATOMIC_GET(&s->max_readers);
            if (r <= cur_max) {
                break;
            }
        } while (!ATOMIC_CAS(&s->max_readers, cur_max, r));
        (void)s->value;/* 读取值（测试无竞争）*/
        ATOMIC_ADD(&s->readers, -1);
        rwlock_unlock(&s->rw);
    }
}

static void _rw_writer(void *arg) {
    _rw_shared *s = (_rw_shared *)arg;
    int i;
    for (i = 0; i < 50; i++) {
        rwlock_wrlock(&s->rw);
        ATOMIC_SET(&s->writer_in, 1);
        /* 写锁是排他的：此刻不该有读者在里面 */
        if (0 != ATOMIC_GET(&s->readers)) {
            ATOMIC_ADD(&s->violation, 1);
        }
        s->value++;
        ATOMIC_SET(&s->writer_in, 0);
        rwlock_unlock(&s->rw);
    }
}

static void test_rwlock(CuTest *tc) {
    _rw_shared s;
    rwlock_init(&s.rw);
    s.value = 0;
    s.readers = 0;
    s.max_readers = 0;
    s.writer_in = 0;
    s.violation = 0;

    pthread_t rths[_NTHREADS];
    pthread_t wths[2];
    for (int i = 0; i < _NTHREADS; i++) {
        rths[i] = thread_creat(_rw_reader, &s);
    }
    for (int i = 0; i < 2; i++) {
        wths[i] = thread_creat(_rw_writer, &s);
    }
    for (int i = 0; i < _NTHREADS; i++) {
        thread_join(rths[i]);
    }
    for (int i = 0; i < 2; i++) {
        thread_join(wths[i]);
    }

    /* 写者累计执行 2×50 次自增 */
    CuAssertIntEquals(tc, 2 * 50, s.value);
    /* 读写互斥一次都没破 */
    CuAssertIntEquals(tc, 0, ATOMIC_GET(&s.violation));
    /* 读者真的并发过：只允许一个读者进临界区的实现在这里挂掉 */
    CuAssertTrue(tc, ATOMIC_GET(&s.max_readers) > 1);

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
static int _cond_waitfail;/* cond_timedwait 非 ERR_OK 的次数，健康跑恒为 0 */

static void _cond_consumer(void *arg) {
    // 单次等待上限：健康跑一定在微秒级被唤醒，超时即判 signal/broadcast 失效
    const uint32_t waitms = 5000;
    _cond_shared *s = (_cond_shared *)arg;
    for (;;) {
        mutex_lock(&s->mu);
        while (s->head == s->tail && !s->done) {
            // 用 timedwait 而非 wait：signal/broadcast 失效时要能退出并报失败，不能挂死 join
            if (ERR_OK != cond_timedwait(&s->cond, &s->mu, waitms)) {
                _cond_waitfail++;
                break;
            }
        }
        while (s->head < s->tail) {
            _sum_consumed += s->queue[s->head % _COND_ITEMS];
            s->head++;
        }
        int finished = 0 != _cond_waitfail || (s->done && (s->head == s->tail));
        mutex_unlock(&s->mu);
        if (finished) {
            break;
        }
    }
}

static void test_cond(CuTest *tc) {
    _cond_shared s;
    mutex_init(&s.mu);
    cond_init(&s.cond);
    s.head = s.tail = s.done = 0;
    _sum_consumed = 0;
    _cond_waitfail = 0;

    /* 期望总和：1+2+...+_COND_ITEMS */
    int expected = _COND_ITEMS * (_COND_ITEMS + 1) / 2;

    pthread_t producer = thread_creat(_cond_producer, &s);
    pthread_t consumer = thread_creat(_cond_consumer, &s);
    thread_join(producer);
    thread_join(consumer);

    CuAssertIntEquals(tc, expected, _sum_consumed);
    /* 每次等待都必须被 signal/broadcast 唤醒，一次超时都不许有 */
    CuAssertIntEquals(tc, 0, _cond_waitfail);

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
    // 观测值先攒进局部量,收拾完再判(同 385 行那条规矩):CuAssert 失败走 longjmp,夹在中间
    // 不只漏掉 free 里的 slots_raw,还把 TLS 里的 owner 留成指向已销毁栈帧的地址
    int32_t reg = rwlock_distr_register(&ctx);
    rwlock_distr_rdlock(&ctx);
    rwlock_distr_runlock(&ctx);
    rwlock_distr_wrlock(&ctx);
    rwlock_distr_wrunlock(&ctx);
    rwlock_distr_unregister(&ctx);
    rwlock_distr_free(&ctx);
    CuAssertIntEquals(tc, ERR_OK, reg);
}

// 幂等:同实例重复 register/unregister 安全
static void test_rwlock_distr_idempotent(CuTest *tc) {
    rwlock_distr_ctx ctx;
    rwlock_distr_init(&ctx, 4);
    int32_t reg1 = rwlock_distr_register(&ctx);
    int32_t reg2 = rwlock_distr_register(&ctx);
    rwlock_distr_unregister(&ctx);
    rwlock_distr_unregister(&ctx);
    int32_t reg3 = rwlock_distr_register(&ctx);
    rwlock_distr_unregister(&ctx);
    rwlock_distr_free(&ctx);
    CuAssertIntEquals(tc, ERR_OK, reg1);
    CuAssertIntEquals(tc, ERR_OK, reg2);
    CuAssertIntEquals(tc, ERR_OK, reg3);// 重新 register 仍可成功
}

// 多实例并发持有:同线程可同时注册多个不同 ctx,各自走快路径互不串扰
static void test_rwlock_distr_multi_register(CuTest *tc) {
    rwlock_distr_ctx a, b;
    rwlock_distr_init(&a, 4);
    rwlock_distr_init(&b, 4);
    int32_t rega = rwlock_distr_register(&a);
    int32_t regb = rwlock_distr_register(&b);
    // 嵌套 rdlock:a 与 b 走各自快路径,锁字段独立
    rwlock_distr_rdlock(&a);
    rwlock_distr_rdlock(&b);
    rwlock_distr_runlock(&b);
    rwlock_distr_runlock(&a);
    rwlock_distr_unregister(&b);
    rwlock_distr_unregister(&a);
    int32_t rega2 = rwlock_distr_register(&a);
    rwlock_distr_unregister(&a);
    rwlock_distr_free(&a);
    rwlock_distr_free(&b);
    CuAssertIntEquals(tc, ERR_OK, rega);
    CuAssertIntEquals(tc, ERR_OK, regb);
    CuAssertIntEquals(tc, ERR_OK, rega2);// unregister 后重新 register 仍可
}

// 同一 ctx 上递归 rdlock:整个嵌套期间 writer 必须一直被挡在临界区外。
// 加 depth 计数之前 active 是布尔量,两条路径都会破坏互斥:
//   1) 嵌套 rdlock 见 write_flag 走让步分支,清 active 把 writer 放进来,而外层读区仍在跑
//   2) 内层 runlock 直接清 active,同样撤掉外层读区的保护
static atomic_t _distr_writer_in;
static void _distr_writer(void *arg) {
    rwlock_distr_ctx *ctx = (rwlock_distr_ctx *)arg;
    rwlock_distr_wrlock(ctx);
    ATOMIC_SET(&_distr_writer_in, 1);
    rwlock_distr_wrunlock(ctx);
}
static void test_rwlock_distr_recursive_rdlock(CuTest *tc) {
    rwlock_distr_ctx ctx;
    pthread_t th;

    rwlock_distr_init(&ctx, 4);
    int32_t reg = rwlock_distr_register(&ctx);
    ATOMIC_SET(&_distr_writer_in, 0);

    rwlock_distr_rdlock(&ctx);
    th = thread_creat(_distr_writer, &ctx);
    // writer 还活着、还阻塞在栈上这个 ctx 的 wrlock 上，此段内一律不断言：
    // CuTest 失败走 longjmp，会跳过下面的 join 与 free，把线程留在已销毁的栈帧上。
    // 观测值先攒进局部量，join 之后再判
    uint64_t deadline = nowms() + _DISTR_MAXMS;
    int32_t timeout = 0;
    while (0 == ATOMIC_GET(&ctx.write_flag)) {
        if (nowms() > deadline) {
            timeout = 1;
            break;
        }
        MSLEEP(1);
    }
    int32_t in_wait = (int32_t)ATOMIC_GET(&_distr_writer_in);
    rwlock_distr_rdlock(&ctx);
    int32_t in_recur = (int32_t)ATOMIC_GET(&_distr_writer_in);
    rwlock_distr_runlock(&ctx);
    int32_t in_inner_out = (int32_t)ATOMIC_GET(&_distr_writer_in);

    rwlock_distr_runlock(&ctx);
    thread_join(th);
    int32_t writer_in_end = (int32_t)ATOMIC_GET(&_distr_writer_in);

    rwlock_distr_rdlock(&ctx);
    rwlock_distr_runlock(&ctx);
    rwlock_distr_wrlock(&ctx);
    rwlock_distr_wrunlock(&ctx);
    rwlock_distr_unregister(&ctx);
    rwlock_distr_free(&ctx);
    CuAssertIntEquals(tc, ERR_OK, reg);
    CuAssertIntEquals(tc, 0, timeout);// 0=writer 按时置上 write_flag
    CuAssertIntEquals(tc, 0, in_wait);
    CuAssertIntEquals(tc, 0, in_recur);// 重入 rdlock 期间 writer 不得挤进来
    CuAssertIntEquals(tc, 0, in_inner_out);// 内层 runlock 后仍持外层读锁
    CuAssertIntEquals(tc, 1, writer_in_end);
}

// TLS 数组耗尽:超过 RWLOCK_DISTR_MAX_TLS 的 ctx 注册失败,rdlock 走 fallback 仍能工作;
// 释放一个槽位后,新 ctx 可补位注册成功
static void test_rwlock_distr_tls_exhaust(CuTest *tc) {
    rwlock_distr_ctx ctxs[RWLOCK_DISTR_MAX_TLS + 1];
    int32_t regs[RWLOCK_DISTR_MAX_TLS + 1];
    int32_t refill;
    int32_t i;
    for (i = 0; i < RWLOCK_DISTR_MAX_TLS + 1; i++) {
        rwlock_distr_init(&ctxs[i], 4);
    }
    for (i = 0; i < RWLOCK_DISTR_MAX_TLS + 1; i++) {
        regs[i] = rwlock_distr_register(&ctxs[i]);
    }
    // 注册失败的 ctx,rdlock 仍能走 fallback 不卡死
    rwlock_distr_rdlock(&ctxs[RWLOCK_DISTR_MAX_TLS]);
    rwlock_distr_runlock(&ctxs[RWLOCK_DISTR_MAX_TLS]);
    // 释放一个槽位,溢出 ctx 可补位
    rwlock_distr_unregister(&ctxs[0]);
    refill = rwlock_distr_register(&ctxs[RWLOCK_DISTR_MAX_TLS]);
    for (i = 1; i < RWLOCK_DISTR_MAX_TLS + 1; i++) {
        rwlock_distr_unregister(&ctxs[i]);
    }
    for (i = 0; i < RWLOCK_DISTR_MAX_TLS + 1; i++) {
        rwlock_distr_free(&ctxs[i]);
    }
    // 前 MAX_TLS 个注册全部成功
    for (i = 0; i < RWLOCK_DISTR_MAX_TLS; i++) {
        CuAssertIntEquals(tc, ERR_OK, regs[i]);
    }
    CuAssertIntEquals(tc, ERR_FAILED, regs[RWLOCK_DISTR_MAX_TLS]);// 超出 TLS 容量
    CuAssertIntEquals(tc, ERR_OK, refill);// 腾出槽位后可补位
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
    // B 卡住也要能走人，否则 join 挂死；到点后 b_reg_result 停在 999，末尾断言报失败
    uint64_t deadline = nowms() + _DISTR_MAXMS;
    while (!ATOMIC_GET(&s->b_done)) {
        if (nowms() > deadline) {
            break;
        }
        CPU_PAUSE();// 紧自旋抢总线，同核上会把对方饿到超时
    }
    rwlock_distr_unregister(s->ctx);
}

static void _distr_pool_worker_b(void *arg) {
    _distr_pool_shared *s = (_distr_pool_shared *)arg;
    // 等 A 占走 slot；A 卡住也要能走人，此时 register 会成功，末尾断言报失败
    uint64_t deadline = nowms() + _DISTR_MAXMS;
    while (!ATOMIC_GET(&s->a_done)) {
        if (nowms() > deadline) {
            break;
        }
        CPU_PAUSE();// 同上
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
    rwlock_distr_free(&ctx);
    CuAssertIntEquals(tc, ERR_FAILED, s.b_reg_result);
}

// 未注册线程 rdlock 走 fallback,功能正确。
// 每进出一次临界区就 +1，主线程据此确认 fallback 路径真的放行了每一轮
static atomic_t _distr_fallback_rounds;

static void _distr_fallback_worker(void *arg) {
    rwlock_distr_ctx *ctx = (rwlock_distr_ctx *)arg;
    // 故意不调 register
    int i;
    for (i = 0; i < 200; i++) {
        rwlock_distr_rdlock(ctx);
        ATOMIC_ADD(&_distr_fallback_rounds, 1);
        rwlock_distr_runlock(ctx);
    }
}

static void test_rwlock_distr_fallback(CuTest *tc) {
    rwlock_distr_ctx ctx;
    rwlock_distr_init(&ctx, 4);
    ATOMIC_SET(&_distr_fallback_rounds, 0);
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
    // fallback 卡住任何一轮都会在 join 前挂死；这条断言拦的是"提前退出/少跑几轮"
    CuAssertIntEquals(tc, 4 * 200, ATOMIC_GET(&_distr_fallback_rounds));
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
            if (r <= cur) {
                break;
            }
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
    for (i = 0; i < _NTHREADS; i++) {
        thread_join(rths[i]);
    }
    for (i = 0; i < 2; i++) {
        thread_join(wths[i]);
    }
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
    SUITE_ADD_TEST(suite, test_rwlock_distr_recursive_rdlock);
    SUITE_ADD_TEST(suite, test_rwlock_distr_tls_exhaust);
    SUITE_ADD_TEST(suite, test_rwlock_distr_pool_exhausted);
    SUITE_ADD_TEST(suite, test_rwlock_distr_fallback);
    SUITE_ADD_TEST(suite, test_rwlock_distr_mutex_check);
    SUITE_ADD_TEST(suite, test_rwlock_distr_slot_reuse);
}
