#include "test_base.h"
#include "lib.h"

#define MEMCNT_THREADS 80 // mem_stat 分条计数用例的线程数
#define MEMCNT_EACH    500 // 每线程的 malloc/free 轮次

/* -----------------------------------------------------------------------
 * 内存宏：MALLOC / CALLOC / REALLOC / FREE
 * ----------------------------------------------------------------------- */
static void test_memory(CuTest *tc) {
    /* MALLOC 分配，FREE 释放 */
    int *pi;
    MALLOC(pi, sizeof(int));
    CuAssertPtrNotNull(tc, pi);
    *pi = 42;
    CuAssertIntEquals(tc, 42, *pi);
    FREE(pi);

    /* CALLOC 分配并清零 */
    int *buf;
    CALLOC(buf, 8, sizeof(int));
    CuAssertPtrNotNull(tc, buf);
    for (int i = 0; i < 8; i++) {
        CuAssertIntEquals(tc, 0, buf[i]);
    }

    /* REALLOC 扩容，原数据保留 */
    int *nbuf;
    REALLOC(nbuf, buf, 16 * sizeof(int));
    CuAssertPtrNotNull(tc, nbuf);
    FREE(nbuf);
}

/* -----------------------------------------------------------------------
 * 32 位原子操作：SET / ADD / CAS / GET
 * ----------------------------------------------------------------------- */
static void test_atomic32(CuTest *tc) {
    atomic_t v = 0;

    /* ATOMIC_SET 写入并读回 */
    ATOMIC_SET(&v, 10);
    CuAssertTrue(tc, 10 == ATOMIC_GET(&v));

    /* ATOMIC_ADD 返回旧值，v 变为 11 */
    atomic_t old = ATOMIC_ADD(&v, 1);
    CuAssertTrue(tc, 10 == old);
    CuAssertTrue(tc, 11 == ATOMIC_GET(&v));

    /* ATOMIC_CAS 成功：旧值匹配时更新 */
    CuAssertTrue(tc, ATOMIC_CAS(&v, 11, 20));
    CuAssertTrue(tc, 20 == ATOMIC_GET(&v));

    /* ATOMIC_CAS 失败：旧值不匹配时保持不变 */
    CuAssertTrue(tc, !ATOMIC_CAS(&v, 0, 99));
    CuAssertTrue(tc, 20 == ATOMIC_GET(&v));

    /* 减法：ADD 负数 */
    ATOMIC_ADD(&v, -5);
    CuAssertTrue(tc, 15 == ATOMIC_GET(&v));
}

/* -----------------------------------------------------------------------
 * 64 位原子操作：SET / ADD / CAS / GET
 * ----------------------------------------------------------------------- */
static void test_atomic64(CuTest *tc) {
    atomic64_t v = 0;

    ATOMIC64_SET(&v, 1000000000LL);
    CuAssertTrue(tc, 1000000000LL == ATOMIC64_GET(&v));

    atomic64_t old = ATOMIC64_ADD(&v, 1);
    CuAssertTrue(tc, 1000000000LL == old);
    CuAssertTrue(tc, 1000000001LL == ATOMIC64_GET(&v));

    CuAssertTrue(tc, ATOMIC64_CAS(&v, 1000000001LL, 2000000000LL));
    CuAssertTrue(tc, 2000000000LL == ATOMIC64_GET(&v));

    CuAssertTrue(tc, !ATOMIC64_CAS(&v, 0, 99));
    CuAssertTrue(tc, 2000000000LL == ATOMIC64_GET(&v));
}

// SET_PTR 的宏体解引用必须括起来。改前是 (*ptr)，传表达式时 * 先于 + 结合：
// SET_PTR(base + 1, v) 展开成 (*base + 1) = v，赋值目标整个错位（多数情况直接编译失败）。
// 本用例传的就是表达式实参，改前编译不过，故它同时是编译期与运行期回归
static void test_set_ptr_expr_arg(CuTest *tc) {
    int32_t buf[3] = { 0, 0, 0 };
    int32_t *base = buf;

    SET_PTR(base + 1, 42);
    CuAssertIntEquals(tc, 0, buf[0]);
    CuAssertIntEquals(tc, 42, buf[1]);
    CuAssertIntEquals(tc, 0, buf[2]);

    // 三元表达式实参同理
    SET_PTR(1 ? base + 2 : NULL, 7);
    CuAssertIntEquals(tc, 7, buf[2]);

    // NULL 守卫仍生效
    int32_t *nil = NULL;
    SET_PTR(nil, 99);
    CuAssertTrue(tc, NULL == nil);
}

/* -----------------------------------------------------------------------
 * mem_stat 的分条计数：80 个线程并发 malloc/free 一个计数都不许丢。落独占格还是
 * 落共享的兜底格取决于此前已用掉多少槽位，两条路径都不丢，断言对两者都成立。
 * 断言用 >= 而不是 ==：本进程还有别的线程(日志线程等)也在分配，增量只会偏大；
 * 而要防的回归恰好是"少算"——槽位共享时用裸自增就会丢
 * MEMORY_CHECK 关掉时 mem_stat 恒写 0，增量必为 0，断言必挂，所以整块随开关编译
 * ----------------------------------------------------------------------- */
#if MEMORY_CHECK
static void _memcnt_worker(void *arg) {
    (void)arg;
    void *p;
    int32_t i;
    for (i = 0; i < MEMCNT_EACH; i++) {
        MALLOC(p, 32);
        FREE(p);
    }
}
static void test_mem_stat_striped(CuTest *tc) {
    uint64_t a0, f0, a1, f1;
    pthread_t th[MEMCNT_THREADS];
    int32_t i;
    mem_stat(&a0, &f0);
    for (i = 0; i < MEMCNT_THREADS; i++) {
        th[i] = thread_creat(_memcnt_worker, NULL);
    }
    for (i = 0; i < MEMCNT_THREADS; i++) {
        thread_join(th[i]);
    }
    mem_stat(&a1, &f1);
    const uint64_t want = (uint64_t)MEMCNT_THREADS * MEMCNT_EACH;
    CuAssertTrue(tc, a1 - a0 >= want);
    CuAssertTrue(tc, f1 - f0 >= want);
}
#endif//MEMORY_CHECK

// 这个用例会把 memory.c 的计数槽位一次性用光(只增不回收), 之后本进程新建的线程
// 全落到共享的兜底格。集成阶段那十几条 loader 线程才是分条计数最该压的场景,
// 所以它单开一个入口, 由 main 排在集成阶段之后跑
void test_base_slots(CuSuite *suite) {
#if MEMORY_CHECK
    SUITE_ADD_TEST(suite, test_mem_stat_striped);
#else
    (void)suite;
#endif
}
void test_base(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_memory);
    SUITE_ADD_TEST(suite, test_atomic32);
    SUITE_ADD_TEST(suite, test_atomic64);
    SUITE_ADD_TEST(suite, test_set_ptr_expr_arg);
}
