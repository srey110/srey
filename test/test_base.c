#include "test_base.h"
#include "lib.h"

static void test_memory(CuTest* tc) {
    int *pTest;
    MALLOC(pTest, sizeof(int));
    FREE(pTest);

    char *pBuf;
    CALLOC(pBuf, (size_t)5, sizeof(int));

    char *pNew;
    REALLOC(pNew, pBuf, (size_t)40);    
    FREE(pNew);
}
static void test_atomic(CuTest* tc) {
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
static void test_utils(CuTest* tc) {
    int16_t sval = -6534;
    char buf[8];
    memcpy(buf, &sval, sizeof(sval));
    sval = (int16_t)unpack_integer(buf, 2, 1, 1);
    CuAssertTrue(tc, -6534 == sval);
    sval = htons(sval);
    memcpy(buf, &sval, sizeof(sval));
    sval = (int16_t)unpack_integer(buf, 2, 0, 1);
    CuAssertTrue(tc, -6534 == sval);

    sval = 6534;
    memcpy(buf, &sval, sizeof(sval));
    sval = (uint16_t)unpack_integer(buf, 2, 1, 1);
    CuAssertTrue(tc, 6534 == sval);
    sval = htons(sval);
    memcpy(buf, &sval, sizeof(sval));
    sval = (int16_t)unpack_integer(buf, 2, 0, 0);
    CuAssertTrue(tc, 6534 == sval);

    sval = -6534;
    pack_integer(buf, sval, 2, 1);
    sval = (uint16_t)unpack_integer(buf, 2, 1, 1);
    CuAssertTrue(tc, -6534 == sval);
    sval = htons(sval);
    pack_integer(buf, sval, 2, 1);
    sval = (uint16_t)unpack_integer(buf, 2, 0, 1);
    CuAssertTrue(tc, -6534 == sval);

    sval = 6534;
    pack_integer(buf, sval, 2, 1);
    sval = (uint16_t)unpack_integer(buf, 2, 1, 0);
    CuAssertTrue(tc, 6534 == sval);
    sval = htons(sval);
    pack_integer(buf, sval, 2, 1);
    sval = (uint16_t)unpack_integer(buf, 2, 0, 0);
    CuAssertTrue(tc, 6534 == sval);

    float f = -123456.789f;
    memcpy(buf, &f, sizeof(f));
    f = unpack_float(buf, 1);
    CuAssertTrue(tc, f - (-123456.789) <= 0.000001);

    f = -123456.789f;
    pack_float(buf, f, 1);
    f = unpack_float(buf, 1);
    CuAssertTrue(tc, f - (-123456.789) <= 0.000001);
    
    double d = -123456.789;
    memcpy(buf, &d, sizeof(d));
    d = unpack_double(buf, 1);
    CuAssertTrue(tc, d - (-123456.789) <= 0.000001);

    d = -123456.789;
    pack_double(buf, d, 1);
    d = unpack_double(buf, 1);
    CuAssertTrue(tc, d - (-123456.789) <= 0.000001);

    uint64_t i64 = (uint64_t)UINT_MAX + 10;
    i64 = ntohll(i64);
    CuAssertTrue(tc, 648518346358128640 == i64);
    i64 = htonll(i64);
    CuAssertTrue(tc, (uint64_t)UINT_MAX + 10 == i64);
}
static void test_binary(CuTest* tc) {
    binary_ctx bwrite;
    binary_init(&bwrite, NULL, 0, 3);
    int8_t i8 = -124;
    binary_set_int8(&bwrite, i8);
    uint8_t ui8 = 124;
    binary_set_uint8(&bwrite, ui8);
    int16_t i16 = (int16_t)-65530;
    binary_set_integer(&bwrite, i16, sizeof(i16), 1);
    uint16_t ui16 = 65530;
    binary_set_uinteger(&bwrite, ui16, sizeof(ui16), 1);
    int32_t i32 = -123456;
    binary_set_integer(&bwrite, i32, 3, 1);
    uint32_t ui32 = 123456;
    binary_set_integer(&bwrite, ui32, 4, 1);
    int64_t i64 = -123456789;
    binary_set_integer(&bwrite, i64, 4, 1);
    uint64_t ui64 = 12345678900;
    binary_set_integer(&bwrite, ui64, sizeof(ui64), 0);
    binary_set_skip(&bwrite, 4);
    float f = -12345.6789f;
    binary_set_float(&bwrite, f, 1);
    double d = 12345.6789;
    binary_set_double(&bwrite, d, 1);
    binary_set_fill(&bwrite, 0, 4);
    const char *str = "this is test";
    binary_set_string(&bwrite, str, strlen(str) + 1);
    const char *binary = "this is test";
    binary_set_string(&bwrite, str, strlen(binary));

    binary_ctx bread;
    binary_init(&bread, bwrite.data, bwrite.offset, 2);
    CuAssertTrue(tc, i8 == binary_get_int8(&bread));
    CuAssertTrue(tc, ui8 == binary_get_uint8(&bread));
    CuAssertTrue(tc, i16 == (int16_t)binary_get_integer(&bread, sizeof(i16), 1));
    CuAssertTrue(tc, ui16 == (uint16_t)binary_get_uinteger(&bread, sizeof(ui16), 1));
    CuAssertTrue(tc, i32 == (int32_t)binary_get_integer(&bread, 3, 1));
    CuAssertTrue(tc, ui32 == (uint32_t)binary_get_uinteger(&bread, 4, 1));
    CuAssertTrue(tc, i64 == binary_get_integer(&bread, 4, 1));
    CuAssertTrue(tc, ui64 == binary_get_uinteger(&bread, sizeof(ui64), 0));
    binary_get_skip(&bread, 4);
    float f2 = binary_get_float(&bread, 1);
    CuAssertTrue(tc, f2 - f <= 0.0000001);
    double d2 = binary_get_double(&bread, 1);
    CuAssertTrue(tc, d2 - d <= 0.0000001);
    binary_get_skip(&bread, 4);
    const char *str2 = binary_get_string(&bread, 0);
    CuAssertTrue(tc, 0 == strcmp(str2, str));
    const char *binary2 = binary_get_string(&bread, strlen(binary));
    CuAssertTrue(tc, 0 == memcmp(binary2, binary, strlen(binary)));
    CuAssertTrue(tc, bread.offset == bread.size);
    FREE(bread.data);
}
void test_base(CuSuite* suite) {
    SUITE_ADD_TEST(suite, test_memory);
    SUITE_ADD_TEST(suite, test_atomic);
    SUITE_ADD_TEST(suite, test_utils);
    SUITE_ADD_TEST(suite, test_binary);
}
