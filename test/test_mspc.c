#include "test_mspc.h"
#include "lib.h"
#include "containers/mspc.h"
#include "thread/thread.h"

/* -----------------------------------------------------------------------
 * 单线程基础测试
 * ----------------------------------------------------------------------- */
static void test_mspc_basic(CuTest *tc) {
    mspc_ctx q;
    mspc_init(&q, 0);   /* 0 → 内部使用默认容量 1024 */

    CuAssertTrue(tc, 1024 == q.capacity);
    CuAssertTrue(tc, 0 == mspc_size(&q));
    CuAssertTrue(tc, NULL == mspc_pop(&q));     /* 空队列出队 → NULL */

    /* 入队 10 个元素（用整数值转换为指针，避免额外堆分配）*/
    for (uintptr_t i = 1; i <= 10; i++) {
        CuAssertTrue(tc, ERR_OK == mspc_push(&q, (void *)i));
    }
    CuAssertTrue(tc, 10 == mspc_size(&q));

    /* FIFO 验证：出队顺序与入队顺序一致 */
    for (uintptr_t i = 1; i <= 10; i++) {
        CuAssertTrue(tc, (void *)i == mspc_pop(&q));
    }
    CuAssertTrue(tc, 0 == mspc_size(&q));
    CuAssertTrue(tc, NULL == mspc_pop(&q));

    mspc_free(&q);
}

/* -----------------------------------------------------------------------
 * 边界测试：容量满、容量向上取幂、重复使用
 * ----------------------------------------------------------------------- */
static void test_mspc_boundary(CuTest *tc) {
    mspc_ctx q;

    /* 容量 4，填满后再 push 应返回 ERR_FAILED */
    mspc_init(&q, 4);
    CuAssertTrue(tc, 4 == q.capacity);
    for (uintptr_t i = 1; i <= 4; i++) {
        CuAssertTrue(tc, ERR_OK == mspc_push(&q, (void *)i));
    }
    CuAssertTrue(tc, ERR_FAILED == mspc_push(&q, (void *)5));  /* 满 */

    /* 消费 2 个后应可再入队 2 个 */
    CuAssertTrue(tc, (void *)1 == mspc_pop(&q));
    CuAssertTrue(tc, (void *)2 == mspc_pop(&q));
    CuAssertTrue(tc, ERR_OK == mspc_push(&q, (void *)5));
    CuAssertTrue(tc, ERR_OK == mspc_push(&q, (void *)6));
    CuAssertTrue(tc, ERR_FAILED == mspc_push(&q, (void *)7));

    /* 验证剩余出队顺序 */
    CuAssertTrue(tc, (void *)3 == mspc_pop(&q));
    CuAssertTrue(tc, (void *)4 == mspc_pop(&q));
    CuAssertTrue(tc, (void *)5 == mspc_pop(&q));
    CuAssertTrue(tc, (void *)6 == mspc_pop(&q));
    CuAssertTrue(tc, NULL == mspc_pop(&q));
    mspc_free(&q);

    /* 非 2 的幂容量应自动向上对齐：5 → 8 */
    mspc_init(&q, 5);
    CuAssertTrue(tc, 8 == q.capacity);
    for (uintptr_t i = 1; i <= 8; i++) {
        CuAssertTrue(tc, ERR_OK == mspc_push(&q, (void *)i));
    }
    CuAssertTrue(tc, ERR_FAILED == mspc_push(&q, (void *)9));
    mspc_free(&q);
}

/* -----------------------------------------------------------------------
 * 并发测试：多生产者 × 多消费者
 * ----------------------------------------------------------------------- */
#define _PROD_CNT    4
#define _CONS_CNT    4
#define _ITEMS_EACH  50000   /* 每个生产者产出的元素数 */
#define _TOTAL       (_PROD_CNT * _ITEMS_EACH)

/* 生产者线程推送连续整数：producer i 推送 [i*N+1 .. (i+1)*N] */
typedef struct {
    mspc_ctx *q;
    int       id;
} _mspc_targ;

static void _producer(void *arg) {
    _mspc_targ *a = (_mspc_targ *)arg;
    uintptr_t start = (uintptr_t)a->id * _ITEMS_EACH + 1;
    uintptr_t end   = start + _ITEMS_EACH;
    for (uintptr_t v = start; v < end; v++) {
        /* 队列满时自旋重试，直到推送成功 */
        while (ERR_OK != mspc_push(a->q, (void *)v)) {
            CPU_PAUSE();
        }
    }
}

/* g_consumed：已消费元素计数；g_sum：消费元素值之和（用于完整性校验）*/
static atomic_t   g_consumed;
static atomic64_t g_sum;

static void _consumer(void *arg) {
    mspc_ctx *q = (mspc_ctx *)arg;
    for (;;) {
        void *p = mspc_pop(q);
        if (NULL != p) {
            /* ATOMIC_ADD 返回旧值，旧值+1 == _TOTAL 表示恰好消费完最后一个 */
            uint32_t prev = ATOMIC_ADD(&g_consumed, 1);
            ATOMIC64_ADD(&g_sum, (uintptr_t)p);
            if (prev + 1 >= (uint32_t)_TOTAL) {
                break;
            }
        } else {
            /* 队列暂空：若已全部消费完则退出，否则自旋等待 */
            if (ATOMIC_GET(&g_consumed) >= (uint32_t)_TOTAL) {
                break;
            }
            CPU_PAUSE();
        }
    }
}

static void test_mspc_concurrent(CuTest *tc) {
    mspc_ctx q;
    mspc_init(&q, 1024);

    g_consumed = 0;
    g_sum      = 0;

    /* 期望总和：1 + 2 + ... + _TOTAL = _TOTAL * (_TOTAL + 1) / 2 */
    int64_t expected_sum = (int64_t)_TOTAL * (_TOTAL + 1) / 2;

    pthread_t  producers[_PROD_CNT];
    pthread_t  consumers[_CONS_CNT];
    _mspc_targ pargs[_PROD_CNT];

    /* 先启动消费者，再启动生产者，避免队列满后生产者长时间自旋 */
    for (int i = 0; i < _CONS_CNT; i++) {
        consumers[i] = thread_creat(_consumer, &q);
    }
    for (int i = 0; i < _PROD_CNT; i++) {
        pargs[i].q  = &q;
        pargs[i].id = i;
        producers[i] = thread_creat(_producer, &pargs[i]);
    }

    for (int i = 0; i < _PROD_CNT; i++) thread_join(producers[i]);
    for (int i = 0; i < _CONS_CNT; i++) thread_join(consumers[i]);

    /* 验证：元素总数与元素值之和均正确，说明无丢失、无重复 */
    CuAssertTrue(tc, (uint32_t)_TOTAL == ATOMIC_GET(&g_consumed));
    CuAssertTrue(tc, expected_sum == (int64_t)ATOMIC64_GET(&g_sum));

    mspc_free(&q);
}

void test_mspc(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_mspc_basic);
    SUITE_ADD_TEST(suite, test_mspc_boundary);
    SUITE_ADD_TEST(suite, test_mspc_concurrent);
}
