#include "test_base.h"
#include "lib.h"

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
    ATOMIC_ADD(&v, (atomic_t)-5);
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

void test_base(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_memory);
    SUITE_ADD_TEST(suite, test_atomic32);
    SUITE_ADD_TEST(suite, test_atomic64);
}
