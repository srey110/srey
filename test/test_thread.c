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
        ATOMIC_ADD(&s->readers, (atomic_t)-1);
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

/* ======================================================================= */

void test_thread(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_mutex);
    SUITE_ADD_TEST(suite, test_spinlock);
    SUITE_ADD_TEST(suite, test_rwlock);
    SUITE_ADD_TEST(suite, test_cond);
    SUITE_ADD_TEST(suite, test_thread_basic);
}
