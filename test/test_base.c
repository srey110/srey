#include "test_base.h"
#include "lib.h"

void test_memory(CuTest* tc) {
    int *pTest;
    MALLOC(pTest, sizeof(int));
    FREE(pTest);

    char *pBuf;
    CALLOC(pBuf, (size_t)5, sizeof(int));

    char *pNew;
    REALLOC(pNew, pBuf, (size_t)40);    
    FREE(pNew);
}
void test_atomic(CuTest* tc) {
    atomic_t i32 = 0;
    ATOMIC_SET(&i32, 1);
    CuAssertTrue(tc, 1 == i32);
    atomic_t rtn = ATOMIC_ADD(&i32, 1);
    CuAssertTrue(tc, 1 == rtn);
    CuAssertTrue(tc, 2 == i32);
    CuAssertTrue(tc, ATOMIC_CAS(&i32, 2, 3));
    CuAssertTrue(tc, 3 == i32);
    CuAssertTrue(tc, 3 == ATOMIC_GET(&i32));

    atomic64_t i64 = 0;
    ATOMIC64_SET(&i64, 1);
    CuAssertTrue(tc, 1 == i64);
    atomic64_t rtn64 = ATOMIC64_ADD(&i64, 1);
    CuAssertTrue(tc, 1 == rtn64);
    CuAssertTrue(tc, 2 == i64);
    CuAssertTrue(tc, ATOMIC64_CAS(&i64, 2, 3));
    CuAssertTrue(tc, 3 == i64);
    CuAssertTrue(tc, 3 == ATOMIC64_GET(&i64));
}
void test_base(CuSuite* suite) {
    SUITE_ADD_TEST(suite, test_memory);
    SUITE_ADD_TEST(suite, test_atomic);
}
