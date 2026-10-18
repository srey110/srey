#include "test_thread.h"
#include "lib.h"

#define _NTHREADS   8/* 并发线程数 */
#define _NITER      10000/* 每线程迭代次数 */
// 兜底：锁真坏了要失败，不能挂死在等标志的自旋里
#define _DISTR_MAXMS 30000
/* cond 生产者-消费者的消息条数 */
#define _COND_ITEMS  1000

// _waiter_until_waker 的叫法
typedef enum waiter_until_mode {
    UNTIL_SPURIOUS = 0, // 只把对方从内核等待里叫出来、不算数，即虚假唤醒
    UNTIL_WAKE,         // waiter_wake
    UNTIL_WAKE_HELD,    // waiter_wake_held
    UNTIL_TRYWAKE       // waiter_trywake
}waiter_until_mode;
typedef struct waiter_until_arg {
    atomic_t done;      // 主线程已从 waiter_commit_until 回来，叫醒线程收手
    int32_t mode;       // waiter_until_mode
    waiter_ctx *w;
}waiter_until_arg;
// test_trylock_busy：另一线程上四种 try 的结果
typedef struct {
    mutex_ctx  mu;
    spin_ctx   spin;
    rwlock_ctx rw;
    int32_t    mu_rtn;
    int32_t    spin_rtn;
    int32_t    rd_rtn;
    int32_t    wr_rtn;
} _try_shared;

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
 * trylock —— 别的线程持锁时 try 必须失败
 * ======================================================================= */

// 在另一线程上把四种 try 各试一次，记下结果；拿到了就当场放掉
static void _try_worker(void *arg) {
    _try_shared *s = (_try_shared *)arg;
    s->mu_rtn = mutex_trylock(&s->mu);
    if (ERR_OK == s->mu_rtn) {
        mutex_unlock(&s->mu);
    }
    s->spin_rtn = spin_trylock(&s->spin);
    if (ERR_OK == s->spin_rtn) {
        spin_unlock(&s->spin);
    }
    s->rd_rtn = rwlock_tryrdlock(&s->rw);
    if (ERR_OK == s->rd_rtn) {
        rwlock_unlock(&s->rw);
    }
    s->wr_rtn = rwlock_trywrlock(&s->rw);
    if (ERR_OK == s->wr_rtn) {
        rwlock_unlock(&s->rw);
    }
}

// 主线程持互斥锁、自旋锁、写锁时另一线程四种 try 全失败；主线程只持读锁时另一线程能再读、不能写，
// 互斥锁与自旋锁已放开、能拿到。Windows 上 trywrlock 失败时不得误置 wlock，否则下面的读锁会按写锁释放
static void test_trylock_busy(CuTest *tc) {
    _try_shared s;
    pthread_t th;
    mutex_init(&s.mu);
    spin_init(&s.spin, SPIN_CNT);
    rwlock_init(&s.rw);

    mutex_lock(&s.mu);
    spin_lock(&s.spin);
    rwlock_wrlock(&s.rw);
    th = thread_creat(_try_worker, &s);
    thread_join(th);
    rwlock_unlock(&s.rw);
    spin_unlock(&s.spin);
    mutex_unlock(&s.mu);
    int32_t mu_busy = s.mu_rtn;
    int32_t spin_busy = s.spin_rtn;
    int32_t rd_busy = s.rd_rtn;
    int32_t wr_busy = s.wr_rtn;

    rwlock_rdlock(&s.rw);
    th = thread_creat(_try_worker, &s);
    thread_join(th);
    rwlock_unlock(&s.rw);
    // 读锁真的放干净了：此刻本线程能拿到写锁
    int32_t wr_after = rwlock_trywrlock(&s.rw);
    if (ERR_OK == wr_after) {
        rwlock_unlock(&s.rw);
    }

    rwlock_free(&s.rw);
    spin_free(&s.spin);
    mutex_free(&s.mu);
    CuAssertIntEquals(tc, ERR_FAILED, mu_busy);
    CuAssertIntEquals(tc, ERR_FAILED, spin_busy);
    CuAssertIntEquals(tc, ERR_FAILED, rd_busy);
    CuAssertIntEquals(tc, ERR_FAILED, wr_busy);
    CuAssertIntEquals(tc, ERR_OK, s.mu_rtn);
    CuAssertIntEquals(tc, ERR_OK, s.spin_rtn);
    CuAssertIntEquals(tc, ERR_OK, s.rd_rtn);
    CuAssertIntEquals(tc, ERR_FAILED, s.wr_rtn);
    CuAssertIntEquals(tc, ERR_OK, wr_after);
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

    /* timedwait 超时验证：没人 signal 就该报超时(1)。条件变量允许虚假唤醒(报 ERR_OK)，重试到不再是它为止 */
    int ret = ERR_OK;
    mutex_lock(&s.mu);
    for (int i = 0; i < 10 && ERR_OK == ret; i++) {
        ret = cond_timedwait(&s.cond, &s.mu, 10);
    }
    mutex_unlock(&s.mu);
    CuAssertTrue(tc, 1 == ret);

    cond_free(&s.cond);
    mutex_free(&s.mu);
}

/* =======================================================================
 * futex —— 按地址等待
 * ======================================================================= */
#if WITH_FUTEX
typedef struct {
    atomic_t word;
    atomic_t woke;// 已醒的等待者数
    atomic_t calls;// 一直等的那个等待者调了几次 futex_wait
    atomic_t ntimeout;// 等待者被报超时的次数：叫醒都在 2s 内，报了就是把叫醒误报成超时
} _futex_shared;

// 等 word 离开 0：每次最多睡 2s，醒来重读。靠超时才醒的话总耗时会超过 1s，由主线程卡住
static void _futex_waiter(void *arg) {
    _futex_shared *s = (_futex_shared *)arg;
    while (0 == ATOMIC_GET(&s->word)) {
        if (1 == futex_wait(&s->word, 0, 2000)) {
            ATOMIC_ADD(&s->ntimeout, 1);
        }
    }
    ATOMIC_ADD(&s->woke, 1);
}
// 同 _futex_waiter，但一直等(ms=0)并数调用次数：一直等若退化成立即返回，会转出成千上万次
static void _futex_waiter_forever(void *arg) {
    _futex_shared *s = (_futex_shared *)arg;
    while (0 == ATOMIC_GET(&s->word)) {
        ATOMIC_ADD(&s->calls, 1);
        (void)futex_wait(&s->word, 0, 0);
    }
}
// 值已变立即返回、不报超时；值没变等到超时、报超时；另一线程改值后 futex_wake 能叫醒；futex_wake_all 叫醒全部等待者，
// 被叫醒都不报超时；一直等(ms=0)不空转。一直等那步放最后：前面叫醒若失效已断言失败退出，不会挂在这里
static void test_futex(CuTest *tc) {
    _futex_shared s;
    pthread_t a, b;
    uint64_t t0, cost;
    int32_t rtn, i;
    ATOMIC_SET(&s.word, 1);
    ATOMIC_SET(&s.woke, 0);
    ATOMIC_SET(&s.ntimeout, 0);
    t0 = nowms();
    CuAssertIntEquals(tc, ERR_OK, futex_wait(&s.word, 0, 2000));
    CuAssertTrue(tc, nowms() - t0 < 1000);
    // 值没变等到超时、报超时。Windows / macOS 都允许无故提前返回(报的是 ERR_OK)，重试到报超时为止
    t0 = nowms();
    rtn = ERR_OK;
    for (i = 0; i < 10 && 1 != rtn; i++) {
        rtn = futex_wait(&s.word, 1, 20);
    }
    cost = nowms() - t0;
    CuAssertIntEquals(tc, 1, rtn);
    CuAssertTrue(tc, cost >= 10 && cost < 1000);
    // 一个等待者：先改值再叫
    ATOMIC_SET(&s.word, 0);
    a = thread_creat(_futex_waiter, &s);
    MSLEEP(20);
    t0 = nowms();
    ATOMIC_SET(&s.word, 1);
    futex_wake(&s.word);
    thread_join(a);
    CuAssertTrue(tc, nowms() - t0 < 1000 && 1 == ATOMIC_GET(&s.woke));
    // 两个等待者：一次 wake_all 都要醒
    ATOMIC_SET(&s.word, 0);
    ATOMIC_SET(&s.woke, 0);
    a = thread_creat(_futex_waiter, &s);
    b = thread_creat(_futex_waiter, &s);
    MSLEEP(20);
    t0 = nowms();
    ATOMIC_SET(&s.word, 1);
    futex_wake_all(&s.word);
    thread_join(a);
    thread_join(b);
    CuAssertTrue(tc, nowms() - t0 < 1000 && 2 == ATOMIC_GET(&s.woke));
    CuAssertIntEquals(tc, 0, (int32_t)ATOMIC_GET(&s.ntimeout));
    ATOMIC_SET(&s.word, 0);
    ATOMIC_SET(&s.calls, 0);
    a = thread_creat(_futex_waiter_forever, &s);
    MSLEEP(50);
    ATOMIC_SET(&s.word, 1);
    futex_wake(&s.word);
    thread_join(a);
    CuAssertTrue(tc, ATOMIC_GET(&s.calls) < 100);
}
#endif

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
    atomic_t cur;
    for (i = 0; i < 500; i++) {
        rwlock_distr_rdlock(&s->ctx);
        // 进入临界区:统计并发读者
        atomic_t r = ATOMIC_ADD(&s->reader_count, 1) + 1;
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
    // 先收拾再断言：断言失败走 longjmp 会跳过释放
    int violation = (int)ATOMIC_GET(&s.violation);
    atomic_t max_readers = ATOMIC_GET(&s.max_readers);
    rwlock_distr_free(&s.ctx);
    // 读写互斥必须严格,违反计数为 0
    CuAssertIntEquals(tc, 0, violation);
    // 应观察到并发 reader > 1
    CuAssertTrue(tc, max_readers > 1);
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

/* =======================================================================
 * waiter —— 消费线程的睡眠握手 + 睡前空转估计
 * ======================================================================= */

// 单线程走一遍状态：prepare 亮出睡眠态、cancel 清掉；mark_waking 只认当前态，抢到后变 WAKING、再抢必败；
// 信号时刻只留首个，不带 timer 的叫醒不记；prepare 会作废此前记下的信号时刻，没人叫醒的 commit 真的睡到超时、返回 0；
// 按地址等待下 prepare 之后来的叫醒不能让 commit 睡下去
static void test_waiter_state(CuTest *tc) {
    timer_ctx tm;
    waiter_ctx w;
    uint64_t first, t0;
    timer_init(&tm);
    waiter_init(&w);
    CuAssertIntEquals(tc, WAITER_AWAKE, waiter_state_get(&w));
    waiter_prepare(&w, WAITER_LINGER);
    CuAssertIntEquals(tc, WAITER_LINGER, waiter_state_get(&w));
    CuAssertIntEquals(tc, WAITER_LINGER, waiter_state_hint(&w));
    CuAssertTrue(tc, !waiter_mark_waking(&w, WAITER_SLEEP));
    CuAssertTrue(tc, waiter_mark_waking(&w, WAITER_LINGER));
    CuAssertIntEquals(tc, WAITER_WAKING, waiter_state_get(&w));
    CuAssertTrue(tc, !waiter_mark_waking(&w, WAITER_LINGER));
    waiter_cancel(&w);
    CuAssertIntEquals(tc, WAITER_AWAKE, waiter_state_get(&w));
    waiter_wake(&w, NULL);// 此刻没人在等
    CuAssertTrue(tc, 0 == w.sig_at);
    waiter_wake(&w, &tm);
    first = w.sig_at;
    MSLEEP(1);
    waiter_wake(&w, &tm);
    waiter_wake(&w, NULL);
    CuAssertTrue(tc, 0 != first && first == w.sig_at);
    waiter_wake_held(&w);
    waiter_trywake(&w);
    // 单次 commit 可能虚假唤醒提前回来，重试到累计睡够 10ms；commit 根本不睡的话 10 次转眼就完，照样判得出
    t0 = nowms();
    uint64_t sig = 0;
    for (int32_t i = 0; i < 10 && nowms() - t0 < 10; i++) {
        waiter_prepare(&w, WAITER_SLEEP);
        sig |= waiter_commit(&w, 20);
    }
    CuAssertTrue(tc, 0 == sig);
    CuAssertTrue(tc, nowms() - t0 >= 10);
    CuAssertIntEquals(tc, WAITER_AWAKE, waiter_state_get(&w));
#if WITH_FUTEX
    // mutex + cond 实现下同一线程这么叫会撞上自己持着的锁，只测按地址等待
    waiter_prepare(&w, WAITER_SLEEP);
    waiter_wake(&w, &tm);
    t0 = nowms();
    CuAssertTrue(tc, 0 != waiter_commit(&w, 2000) && nowms() - t0 < 1000);
#endif
    waiter_free(&w);
}
typedef struct waiter_pp_arg {
    waiter_ctx w[2];
    atomic_t turn;// 轮到谁：0 主线程，1 子线程
    atomic_t slow;// 等了 ≥400ms 才轮到的次数，即丢了唤醒、靠超时才醒
    int32_t rounds;
}waiter_pp_arg;
// 等到轮到 me：先亮出睡眠态再复查，没轮到才睡(限时 500ms，醒来再查)
static void _waiter_pp_wait(waiter_pp_arg *pp, int32_t me) {
    uint64_t t0 = nowms();
    while (me != (int32_t)ATOMIC_GET(&pp->turn)) {
        waiter_prepare(&pp->w[me], WAITER_SLEEP);
        if (me == (int32_t)ATOMIC_GET(&pp->turn)) {
            waiter_cancel(&pp->w[me]);
            break;
        }
        (void)waiter_commit(&pp->w[me], 500);
    }
    if (nowms() - t0 >= 400) {
        ATOMIC_ADD(&pp->slow, 1);
    }
}
// 交棒给 to：先改轮次再看对方睡没睡，与 _waiter_pp_wait 的"先亮态再复查"配对。
// 两个方向各用一种叫法(mutex + cond 实现下即解锁后 signal 与持锁 signal，都要走到)
static void _waiter_pp_pass(waiter_pp_arg *pp, int32_t to) {
    ATOMIC_SET_SEQCST(&pp->turn, to);
    if (WAITER_AWAKE != waiter_state_get(&pp->w[to])) {
        if (1 == to) {
            waiter_wake(&pp->w[to], NULL);
        } else {
            waiter_wake_held(&pp->w[to]);
        }
    }
}
static void _waiter_pp_peer(void *arg) {
    waiter_pp_arg *pp = (waiter_pp_arg *)arg;
    for (int32_t i = 0; i < pp->rounds; i++) {
        _waiter_pp_wait(pp, 1);
        _waiter_pp_pass(pp, 0);
    }
}
// 隔着 waiter 乒乓 2000 轮：每次都是"刚复查完准备睡"与"刚交棒准备叫"的赛跑，丢一次唤醒就要靠 500ms 超时才醒
static void test_waiter_pingpong(CuTest *tc) {
    waiter_pp_arg pp;
    pthread_t th;
    waiter_init(&pp.w[0]);
    waiter_init(&pp.w[1]);
    ATOMIC_SET(&pp.turn, 0);
    ATOMIC_SET(&pp.slow, 0);
    pp.rounds = 2000;
    th = thread_creat(_waiter_pp_peer, &pp);
    for (int32_t i = 0; i < pp.rounds; i++) {
        _waiter_pp_pass(&pp, 1);
        _waiter_pp_wait(&pp, 0);
    }
    thread_join(th);
    waiter_free(&pp.w[0]);
    waiter_free(&pp.w[1]);
    CuAssertIntEquals(tc, 0, (int32_t)ATOMIC_GET(&pp.slow));
}
typedef struct waiter_sig_arg {
    waiter_ctx *w;
    timer_ctx *timer;
    atomic_t started;// 叫醒线程已起来，马上去叫
    atomic_t done;// 主线程已收到信号时刻
}waiter_sig_arg;
// 一起来就叫(mutex + cond 实现下撞在对方持着的锁上)，之后每 5ms 再叫一次直到对方收到：对方虚假唤醒后重新 prepare 会作废已记下的时刻
static void _waiter_sig_waker(void *arg) {
    waiter_sig_arg *a = (waiter_sig_arg *)arg;
    ATOMIC_SET(&a->started, 1);
    while (0 == ATOMIC_GET(&a->done)) {
        waiter_wake(a->w, a->timer);
        MSLEEP(5);
    }
}
// 叫醒方记下的信号时刻由 commit 带回，不晚于醒来。mutex + cond 实现要在锁内取：这边 prepare 后持着锁再起叫醒线程，
// 它一叫就撞在锁上，持锁 30ms 后记下 t0 才进入等待，带回的不能早于 t0(锁外取会早 30ms)；按地址等待没有锁，t0 在起线程前取
static void test_waiter_sig_at(CuTest *tc) {
    timer_ctx tm;
    waiter_ctx w;
    waiter_sig_arg a;
    pthread_t th;
    uint64_t sig = 0, t0, t1;
    int32_t n = 0;
    timer_init(&tm);
    waiter_init(&w);
    a.w = &w;
    a.timer = &tm;
    ATOMIC_SET(&a.started, 0);
    ATOMIC_SET(&a.done, 0);
    waiter_prepare(&w, WAITER_SLEEP);
#if WITH_FUTEX
    t0 = timer_cur(&tm);
#endif
    th = thread_creat(_waiter_sig_waker, &a);
#if !WITH_FUTEX
    while (0 == ATOMIC_GET(&a.started)) {
        THREAD_YIELD();
    }
    MSLEEP(30);
    t0 = timer_cur(&tm);
#endif
    for (;;) {
        sig = waiter_commit(&w, 2000);
        if (0 != sig || ++n >= 10) {
            break;
        }
        waiter_prepare(&w, WAITER_SLEEP);// 虚假唤醒：再睡一次
    }
    t1 = timer_cur(&tm);
    ATOMIC_SET(&a.done, 1);
    thread_join(th);
    waiter_free(&w);
    CuAssertTrue(tc, sig >= t0 && sig <= t1);
}
typedef struct waiter_mp_arg {
    waiter_ctx w;
    atomic_t pending;// 已投未取的件数；对它做一次足序加就算入队
    int32_t per;// 每个生产者投的件数
}waiter_mp_arg;
// 生产者：先入队再读睡眠态，读到 SLEEP 才抢 WAKING、抢到的才叫(同 worker 投递)；每 8 件歇 1ms，让消费方有机会睡着
static void _waiter_mp_producer(void *arg) {
    waiter_mp_arg *a = (waiter_mp_arg *)arg;
    for (int32_t i = 0; i < a->per; i++) {
        ATOMIC_ADD(&a->pending, 1);
        if (WAITER_SLEEP == waiter_state_get(&a->w)
            && waiter_mark_waking(&a->w, WAITER_SLEEP)) {
            waiter_wake(&a->w, NULL);
        }
        if (0 == (i & 7)) {
            MSLEEP(1);
        }
    }
}
// 4 个生产者同时投给一个消费方，只让抢到 WAKING 的那个叫：丢一次唤醒就要靠 500ms 超时才醒
static void test_waiter_multi_producer(CuTest *tc) {
    waiter_mp_arg a;
    pthread_t ths[4];
    int32_t i, total, got = 0, slow = 0;
    atomic_t n;
    uint64_t t0;
    waiter_init(&a.w);
    ATOMIC_SET(&a.pending, 0);
    a.per = 200;
    total = 4 * a.per;
    for (i = 0; i < 4; i++) {
        ths[i] = thread_creat(_waiter_mp_producer, &a);
    }
    while (got < total) {
        n = ATOMIC_SET(&a.pending, 0);// 一次换走全部，拿到的是换之前的件数
        if (0 != n) {
            got += (int32_t)n;
            continue;
        }
        waiter_prepare(&a.w, WAITER_SLEEP);
        if (0 != ATOMIC_GET_SEQCST(&a.pending)) {
            waiter_cancel(&a.w);
            continue;
        }
        t0 = nowms();
        (void)waiter_commit(&a.w, 500);
        if (nowms() - t0 >= 400) {
            slow++;
        }
    }
    for (i = 0; i < 4; i++) {
        thread_join(ths[i]);
    }
    waiter_free(&a.w);
    CuAssertIntEquals(tc, total, got);
    CuAssertIntEquals(tc, 0, slow);
}
// 20ms 后开始叫，每 5ms 一次，直到主线程回来：只叫一次的话可能赶在对方进内核等待之前，落空。
// 假叫醒：按地址等待不改 epoch 直接叫，mutex + cond 不置叫醒标志直接 signal
static void _waiter_until_waker(void *arg) {
    waiter_until_arg *a = (waiter_until_arg *)arg;
    MSLEEP(20);
    while (0 == ATOMIC_GET(&a->done)) {
        switch (a->mode) {
        case UNTIL_WAKE:
            waiter_wake(a->w, NULL);
            break;
        case UNTIL_WAKE_HELD:
            waiter_wake_held(a->w);
            break;
        case UNTIL_TRYWAKE:
            waiter_trywake(a->w);
            break;
        default:
#if WITH_FUTEX
            futex_wake(&a->w->epoch);
#else
            mutex_lock(&a->w->mutex);
            cond_signal(&a->w->cond);
            mutex_unlock(&a->w->mutex);
#endif
            break;
        }
        MSLEEP(5);
    }
}
// 起叫醒线程按 a->mode 叫，主线程 commit_until 到 dl；回来后让叫醒线程收手并 join，带回返回值与醒来时刻
static int32_t _waiter_until_run(waiter_until_arg *a, timer_ctx *tm, uint64_t dl, uint64_t *at) {
    pthread_t th;
    int32_t rtn;
    ATOMIC_SET(&a->done, 0);
    waiter_prepare(a->w, WAITER_SLEEP);
    th = thread_creat(_waiter_until_waker, a);
    rtn = waiter_commit_until(a->w, tm, dl);
    *at = timer_cur_ms(tm);
    ATOMIC_SET(&a->done, 1);
    thread_join(th);
    return rtn;
}
// waiter_commit_until：剩余毫秒的夹紧；截止已过不睡、报到点；没人叫睡到截止、报到点；已被叫醒又已到点算叫醒；
// 另一线程用三种叫法叫醒都早于截止返回、报叫醒；虚假唤醒被吸收，照样睡到截止
static void test_waiter_commit_until(CuTest *tc) {
    timer_ctx tm;
    waiter_ctx w;
    waiter_until_arg a;
    uint64_t dl, spur_dl, spur_at;
    uint64_t dls[3], at[3];
    int32_t rtn[3], spur_rtn, i;
    timer_init(&tm);
    waiter_init(&w);
    a.w = &w;
    // 剩余毫秒夹在 [1, UINT32_MAX - 1]：0 是一直等，Windows 上 UINT32_MAX 也是一直等
    CuAssertTrue(tc, 1 == _waiter_left_ms(101, 100));
    CuAssertTrue(tc, UINT32_MAX - 1 == _waiter_left_ms((uint64_t)UINT32_MAX - 1, 0));
    CuAssertTrue(tc, UINT32_MAX - 1 == _waiter_left_ms((uint64_t)UINT32_MAX, 0));
    CuAssertTrue(tc, UINT32_MAX - 1 == _waiter_left_ms(UINT64_MAX, 0));
    waiter_prepare(&w, WAITER_SLEEP);
    CuAssertIntEquals(tc, 1, waiter_commit_until(&w, &tm, timer_cur_ms(&tm)));
    CuAssertIntEquals(tc, WAITER_AWAKE, waiter_state_get(&w));
    waiter_prepare(&w, WAITER_SLEEP);
    dl = timer_cur_ms(&tm) + 20;
    CuAssertIntEquals(tc, 1, waiter_commit_until(&w, &tm, dl));
    CuAssertTrue(tc, timer_cur_ms(&tm) >= dl);
    // 已被叫醒又已到点算叫醒。mutex + cond 下同一线程调 waiter_wake 会撞上 prepare 拿着的锁，直接置标志
    waiter_prepare(&w, WAITER_SLEEP);
#if WITH_FUTEX
    waiter_wake(&w, NULL);
#else
    w.woken = 1;
#endif
    CuAssertIntEquals(tc, ERR_OK, waiter_commit_until(&w, &tm, timer_cur_ms(&tm)));
    // 跨线程的用例先跑完、join 之后再断言，断言失败不会留下没收的线程
    for (i = 0; i < 3; i++) {
        a.mode = UNTIL_WAKE + i;
        dls[i] = timer_cur_ms(&tm) + 2000;
        rtn[i] = _waiter_until_run(&a, &tm, dls[i], &at[i]);
    }
    a.mode = UNTIL_SPURIOUS;
    spur_dl = timer_cur_ms(&tm) + 200;
    spur_rtn = _waiter_until_run(&a, &tm, spur_dl, &spur_at);
    waiter_free(&w);
    for (i = 0; i < 3; i++) {
        CuAssertIntEquals(tc, ERR_OK, rtn[i]);
        CuAssertTrue(tc, at[i] + 1000 < dls[i]);
    }
    CuAssertIntEquals(tc, 1, spur_rtn);
    CuAssertTrue(tc, spur_at >= spur_dl);
}
// spinner_spin 的两个判活回调：一个只在 spinning 标志已置 1 时报有活，一个永远没活
static int32_t _spinner_ready_flag(void *arg) {
    return 1 == (int32_t)ATOMIC_GET((atomic_t *)arg);
}
static int32_t _spinner_ready_never(void *arg) {
    (void)arg;
    return 0;
}
// 空转估计：预算、间隔与唤醒代价的平滑和夹紧；空转本身；以及空闲开始 / 睡醒 / 拿到活三处的状态流转，
// 含醒来活被偷走时作废上一觉的信号时刻
static void test_spinner(CuTest *tc) {
    timer_ctx tm;
    spinner_ctx sp;
    atomic_t spinning;
    uint64_t since, t0;
    timer_init(&tm);
    spinner_init(&sp, &tm, 1, 50);
    CuAssertTrue(tc, 0 == sp.gap_ns && 1000 == sp.wake_ns && 0 == sp.idle_since);
    // 预算：间隔不超过唤醒代价时转 2 倍间隔，夹在 [1µs, 50µs]；间隔更长只转下限
    sp.gap_ns = 10000;
    sp.wake_ns = 20000;
    CuAssertTrue(tc, 20000 == _spinner_budget(&sp));
    sp.gap_ns = 40000;
    sp.wake_ns = 45000;
    CuAssertTrue(tc, 50000 == _spinner_budget(&sp));
    sp.gap_ns = 300;
    sp.wake_ns = 1000;
    CuAssertTrue(tc, 1000 == _spinner_budget(&sp));
    sp.gap_ns = 30000;
    sp.wake_ns = 20000;
    CuAssertTrue(tc, 1000 == _spinner_budget(&sp));
    // 间隔估计每次挪 1/4，单个样本先截到 2 倍上限
    sp.gap_ns = 0;
    CuAssertTrue(tc, 4000 == _spinner_gap(&sp, 16000));
    CuAssertTrue(tc, 25000 == _spinner_gap(&sp, 10000000));
    // 唤醒代价每次挪 1/8，结果夹在上下限内
    sp.wake_ns = 8000;
    CuAssertTrue(tc, 9000 == _spinner_wake(&sp, 16000));
    sp.wake_ns = 1000;
    CuAssertTrue(tc, 1000 == _spinner_wake(&sp, 0));
    sp.wake_ns = 50000;
    CuAssertTrue(tc, 50000 == _spinner_wake(&sp, 10000000));
    // 空转：一问就有活立即返回 1(回调里看得到 spinning 已置 1)，返回前复位；
    // 一直没活就转满截止时刻(此刻是下限 1µs)返回 0。两次都算同一段空闲，起点不变
    spinner_init(&sp, &tm, 1, 50);
    ATOMIC_SET(&spinning, 0);
    CuAssertIntEquals(tc, 1, spinner_spin(&sp, &spinning, _spinner_ready_flag, &spinning));
    CuAssertIntEquals(tc, 0, (int32_t)ATOMIC_GET(&spinning));
    CuAssertTrue(tc, 0 != sp.idle_since && 0 == sp.sig_at);
    since = sp.idle_since;
    t0 = timer_cur(&tm);
    CuAssertIntEquals(tc, 0, spinner_spin(&sp, NULL, _spinner_ready_never, NULL));
    CuAssertTrue(tc, timer_cur(&tm) - t0 >= 1000);
    CuAssertTrue(tc, since == sp.idle_since);
    // 睡醒带回的信号时刻作为活的到来时刻，同时记下唤醒代价最小值；拿到活后退出空闲
    spinner_wake(&sp, sp.idle_since);
    CuAssertTrue(tc, sp.sig_at == sp.idle_since && 0 != sp.wake_min_ns);
    spinner_busy(&sp);
    CuAssertTrue(tc, 0 == sp.idle_since && 0 == sp.gap_ns);
    // 不在空闲时 busy 什么都不做
    sp.gap_ns = 777;
    spinner_busy(&sp);
    CuAssertTrue(tc, 777 == sp.gap_ns);
    // 醒来那份活被别人拿走、又空转一轮：上一觉的信号时刻作废，拿到活时按现在算间隔
    sp.gap_ns = 0;
    CuAssertIntEquals(tc, 0, spinner_spin(&sp, NULL, _spinner_ready_never, NULL));
    spinner_wake(&sp, sp.idle_since);
    CuAssertIntEquals(tc, 0, spinner_spin(&sp, NULL, _spinner_ready_never, NULL));
    CuAssertTrue(tc, 0 == sp.sig_at);
    spinner_busy(&sp);
    CuAssertTrue(tc, sp.gap_ns > 0);
}
// 唤醒极贵时的试转与切档：门控看唤醒代价最小值、中途关上档位不动；按估计不转时数满间隔才试转，
// 转中至少一半切进转档，转档里一直转空就退回；试转落空间隔翻倍、封顶；转档里 spinner_spin 按上限转满、
// 转中转空都记账，按估计转时不记账
static void test_spinner_hot(CuTest *tc) {
    timer_ctx tm;
    spinner_ctx sp;
    atomic_t spinning;
    uint64_t t0;
    int32_t i;
    timer_init(&tm);
    spinner_init(&sp, &tm, 1, 50);
    CuAssertTrue(tc, 0 == sp.wake_min_ns && SPINNER_EST == sp.mode && SPINNER_TRIAL_MIN == sp.trial_wait);
    // 最小值：没样本时直接取，更小直接取，更大每次只挪 1/64，样本先截到 2 倍上限
    CuAssertTrue(tc, 30000 == _spinner_wake_min(&sp, 30000));
    sp.wake_min_ns = 30000;
    CuAssertTrue(tc, 10000 == _spinner_wake_min(&sp, 10000));
    CuAssertTrue(tc, 30000 + (100000 - 30000) / 64 == _spinner_wake_min(&sp, 10000000));
    // 时钟回退出的负样本当作最小
    CuAssertTrue(tc, 1 == _spinner_wake_min(&sp, (uint64_t)(int64_t)-5000));
    // 门控没开(唤醒便宜)：不数间隔、不试转
    sp.wake_min_ns = SPINNER_WAKE_DEAR - 1;
    sp.trial_wait = 1;
    CuAssertTrue(tc, !_spinner_hot(&sp, sp.lo_ns) && 1 == sp.trial_wait);
    // 门控开但估计说转(预算超过下限)：照估计，不数间隔
    sp.wake_min_ns = SPINNER_WAKE_DEAR;
    CuAssertTrue(tc, !_spinner_hot(&sp, sp.lo_ns + 1) && 1 == sp.trial_wait);
    // 数满间隔开始试转；试转中每次都按上限转，转中恰好一半就切进转档，试转间隔复位成最小
    sp.trial_every = 4 * SPINNER_TRIAL_MIN;
    CuAssertTrue(tc, _spinner_hot(&sp, sp.lo_ns) && SPINNER_TRIAL == sp.mode && SPINNER_TRIAL_LEN == sp.trial_left);
    _spinner_settle(&sp, 1);
    for (i = 1; i < SPINNER_TRIAL_LEN; i++) {
        CuAssertTrue(tc, _spinner_hot(&sp, sp.lo_ns));
        _spinner_settle(&sp, i < SPINNER_TRIAL_LEN / 2);
    }
    CuAssertTrue(tc, SPINNER_HOT == sp.mode && 256 == sp.hit_ratio && SPINNER_TRIAL_MIN == sp.trial_every);
    // 转档里 spinner_spin 按上限转满，转空一次转中率降 1/8
    t0 = timer_cur(&tm);
    CuAssertIntEquals(tc, 0, spinner_spin(&sp, NULL, _spinner_ready_never, NULL));
    CuAssertTrue(tc, timer_cur(&tm) - t0 >= (uint64_t)sp.hi_ns);
    CuAssertIntEquals(tc, 224, sp.hit_ratio);
    // 转中一次转中率升 (256 - 224) / 8
    ATOMIC_SET(&spinning, 0);
    CuAssertIntEquals(tc, 1, spinner_spin(&sp, &spinning, _spinner_ready_flag, &spinning));
    CuAssertIntEquals(tc, 228, sp.hit_ratio);
    // 一直转空：转中率掉到 1/4 以下退回，下次试转在试转间隔之后
    for (i = 0; i < 64 && SPINNER_HOT == sp.mode; i++) {
        _spinner_settle(&sp, 0);
    }
    CuAssertTrue(tc, SPINNER_EST == sp.mode && SPINNER_TRIAL_MIN == sp.trial_wait);
    // 试转中门控关上：不按上限转、档位不动；重开后接着这次试转
    sp.trial_wait = 1;
    CuAssertTrue(tc, _spinner_hot(&sp, sp.lo_ns) && SPINNER_TRIAL == sp.mode);
    sp.wake_min_ns = SPINNER_WAKE_DEAR - 1;
    CuAssertTrue(tc, !_spinner_hot(&sp, sp.lo_ns) && SPINNER_TRIAL == sp.mode);
    sp.wake_min_ns = SPINNER_WAKE_DEAR;
    CuAssertTrue(tc, _spinner_hot(&sp, sp.lo_ns) && SPINNER_TRIAL == sp.mode && SPINNER_TRIAL_LEN == sp.trial_left);
    // 这次试转落空(差一次到一半)：间隔翻倍
    for (i = 0; i < SPINNER_TRIAL_LEN; i++) {
        _spinner_settle(&sp, i < SPINNER_TRIAL_LEN / 2 - 1);
    }
    CuAssertTrue(tc, SPINNER_EST == sp.mode && 2 * SPINNER_TRIAL_MIN == sp.trial_every && sp.trial_every == sp.trial_wait);
    // 间隔到上限就不再翻
    sp.trial_every = SPINNER_TRIAL_MAX;
    sp.trial_wait = 1;
    CuAssertTrue(tc, _spinner_hot(&sp, sp.lo_ns));
    for (i = 0; i < SPINNER_TRIAL_LEN; i++) {
        _spinner_settle(&sp, 0);
    }
    CuAssertTrue(tc, SPINNER_TRIAL_MAX == sp.trial_every && SPINNER_TRIAL_MAX == sp.trial_wait);
    // 门控开但估计说转：spinner_spin 照估计转，不数间隔也不记账
    sp.trial_wait = 5;
    sp.gap_ns = 10000;
    sp.wake_ns = 20000;
    CuAssertIntEquals(tc, 0, spinner_spin(&sp, NULL, _spinner_ready_never, NULL));
    CuAssertTrue(tc, SPINNER_EST == sp.mode && 5 == sp.trial_wait);
}

/* ======================================================================= */

void test_thread(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_mutex);
    SUITE_ADD_TEST(suite, test_spinlock);
    SUITE_ADD_TEST(suite, test_rwlock);
    SUITE_ADD_TEST(suite, test_trylock_busy);
    SUITE_ADD_TEST(suite, test_cond);
#if WITH_FUTEX
    SUITE_ADD_TEST(suite, test_futex);
#endif
    SUITE_ADD_TEST(suite, test_waiter_state);
    SUITE_ADD_TEST(suite, test_waiter_pingpong);
    SUITE_ADD_TEST(suite, test_waiter_sig_at);
    SUITE_ADD_TEST(suite, test_waiter_multi_producer);
    SUITE_ADD_TEST(suite, test_waiter_commit_until);
    SUITE_ADD_TEST(suite, test_spinner);
    SUITE_ADD_TEST(suite, test_spinner_hot);
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
