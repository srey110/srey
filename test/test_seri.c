#include "test_seri.h"
#include "lib.h"

// nil / true / false 三元基础往返
static void test_seri_basic_nil_bool(CuTest *tc) {
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    seri_append_nil(&bw);
    seri_append_bool(&bw, 1);
    seri_append_bool(&bw, 0);

    seri_iter iter;
    seri_item item;
    seri_iter_init(&iter, bw.data, bw.offset);
    CuAssertIntEquals(tc, 1, seri_iter_next(&iter, &item));
    CuAssertIntEquals(tc, SERI_ITEM_NIL, item.type);
    CuAssertIntEquals(tc, 1, seri_iter_next(&iter, &item));
    CuAssertIntEquals(tc, SERI_ITEM_BOOL, item.type);
    CuAssertIntEquals(tc, 1, item.v.b);
    CuAssertIntEquals(tc, 1, seri_iter_next(&iter, &item));
    CuAssertIntEquals(tc, SERI_ITEM_BOOL, item.type);
    CuAssertIntEquals(tc, 0, item.v.b);
    CuAssertIntEquals(tc, 0, seri_iter_next(&iter, &item));

    binary_free(&bw);
}
// 整数各档边界：0 / byte / word / dword(u32) / dword(neg i32) / qword
static void test_seri_int_buckets(CuTest *tc) {
    int64_t vals[] = {
        0,
        1, 0xFF,              // BYTE 边界
        0x100, 0xFFFF,        // WORD 边界
        0x10000, 0xFFFFFFFF,  // DWORD(u32) 边界
        -1, INT32_MIN,        // DWORD(i32) 负数
        ((int64_t)INT32_MAX) + 1,            // QWORD 正越界
        INT64_MIN, INT64_MAX  // QWORD 端点
    };
    size_t n = sizeof(vals) / sizeof(vals[0]);
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    size_t i;
    for (i = 0; i < n; i++) {
        seri_append_int(&bw, vals[i]);
    }

    seri_iter iter;
    seri_item item;
    seri_iter_init(&iter, bw.data, bw.offset);
    for (i = 0; i < n; i++) {
        CuAssertIntEquals(tc, 1, seri_iter_next(&iter, &item));
        CuAssertIntEquals(tc, SERI_ITEM_INT, item.type);
        CuAssertTrue(tc, vals[i] == item.v.i);
    }
    CuAssertIntEquals(tc, 0, seri_iter_next(&iter, &item));

    binary_free(&bw);
}
// 实数精度往返
static void test_seri_real(CuTest *tc) {
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    seri_append_real(&bw, 3.14159265358979);
    seri_append_real(&bw, -1.0e-300);
    seri_append_real(&bw, 0.0);

    seri_iter iter;
    seri_item item;
    seri_iter_init(&iter, bw.data, bw.offset);
    seri_iter_next(&iter, &item);
    CuAssertIntEquals(tc, SERI_ITEM_REAL, item.type);
    CuAssertDblEquals(tc, 3.14159265358979, item.v.r, 1e-12);
    seri_iter_next(&iter, &item);
    CuAssertDblEquals(tc, -1.0e-300, item.v.r, 1e-310);
    seri_iter_next(&iter, &item);
    CuAssertDblEquals(tc, 0.0, item.v.r, 0.0);

    binary_free(&bw);
}
// 字符串短/长边界：长度 0 / 1 / 31(短) / 32(长 u16) 临界，含二进制 NUL
static void test_seri_string(CuTest *tc) {
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    seri_append_string(&bw, "", 0);
    seri_append_string(&bw, "a", 1);
    char s31[31];
    memset(s31, 'x', 31);
    seri_append_string(&bw, s31, 31);
    char s32[32];
    memset(s32, 'y', 32);
    seri_append_string(&bw, s32, 32);
    // 二进制安全：含 NUL
    char bin[5] = {'\0', 'A', '\0', 'B', '\0'};
    seri_append_string(&bw, bin, 5);

    seri_iter iter;
    seri_item item;
    seri_iter_init(&iter, bw.data, bw.offset);
    seri_iter_next(&iter, &item);
    CuAssertIntEquals(tc, SERI_ITEM_STRING, item.type);
    CuAssertIntEquals(tc, 0, (int32_t)item.v.s.len);
    seri_iter_next(&iter, &item);
    CuAssertIntEquals(tc, 1, (int32_t)item.v.s.len);
    CuAssertIntEquals(tc, 'a', item.v.s.p[0]);
    seri_iter_next(&iter, &item);
    CuAssertIntEquals(tc, 31, (int32_t)item.v.s.len);
    CuAssertTrue(tc, 0 == memcmp(item.v.s.p, s31, 31));
    seri_iter_next(&iter, &item);
    CuAssertIntEquals(tc, 32, (int32_t)item.v.s.len);
    CuAssertTrue(tc, 0 == memcmp(item.v.s.p, s32, 32));
    seri_iter_next(&iter, &item);
    CuAssertIntEquals(tc, 5, (int32_t)item.v.s.len);
    CuAssertTrue(tc, 0 == memcmp(item.v.s.p, bin, 5));

    binary_free(&bw);
}
// userdata 指针往返
static void test_seri_userdata(CuTest *tc) {
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    int local_var = 0;
    void *p1 = &local_var;
    void *p2 = (void *)(uintptr_t)0xCAFEBABE12345678ULL;
    seri_append_userdata(&bw, p1);
    seri_append_userdata(&bw, p2);

    seri_iter iter;
    seri_item item;
    seri_iter_init(&iter, bw.data, bw.offset);
    seri_iter_next(&iter, &item);
    CuAssertIntEquals(tc, SERI_ITEM_USERDATA, item.type);
    CuAssertPtrEquals(tc, p1, item.v.ud);
    seri_iter_next(&iter, &item);
    CuAssertPtrEquals(tc, p2, item.v.ud);

    binary_free(&bw);
}
// 简单 array：数组段 [10, 20]，hash 段 {"k1"=true}
static void test_seri_array_simple(CuTest *tc) {
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    seri_append_array_start(&bw, 2);
    seri_append_int(&bw, 10);
    seri_append_int(&bw, 20);
    seri_append_string(&bw, "k1", 2);
    seri_append_bool(&bw, 1);
    seri_append_array_end(&bw);

    seri_iter iter;
    seri_item item;
    seri_iter_init(&iter, bw.data, bw.offset);
    seri_iter_next(&iter, &item);
    CuAssertIntEquals(tc, SERI_ITEM_ARRAY_BEGIN, item.type);
    CuAssertIntEquals(tc, 2, (int32_t)item.v.array_n);
    seri_iter_next(&iter, &item);
    CuAssertIntEquals(tc, SERI_ITEM_INT, item.type);
    CuAssertTrue(tc, 10 == item.v.i);
    seri_iter_next(&iter, &item);
    CuAssertTrue(tc, 20 == item.v.i);
    // hash 段
    seri_iter_next(&iter, &item);
    CuAssertIntEquals(tc, SERI_ITEM_STRING, item.type);
    CuAssertIntEquals(tc, 2, (int32_t)item.v.s.len);
    CuAssertTrue(tc, 0 == memcmp(item.v.s.p, "k1", 2));
    seri_iter_next(&iter, &item);
    CuAssertIntEquals(tc, SERI_ITEM_BOOL, item.type);
    CuAssertIntEquals(tc, 1, item.v.b);
    // hash 段结束（NIL 标记）
    seri_iter_next(&iter, &item);
    CuAssertIntEquals(tc, SERI_ITEM_NIL, item.type);
    CuAssertIntEquals(tc, 0, seri_iter_next(&iter, &item));

    binary_free(&bw);
}
// 长 array 转义：array_n >= 31 时 cookie=31 后跟 INT 真实长度
static void test_seri_array_long(CuTest *tc) {
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    uint32_t n = 100;
    seri_append_array_start(&bw, n);
    uint32_t i;
    for (i = 0; i < n; i++) {
        seri_append_int(&bw, (int64_t)i);
    }
    seri_append_array_end(&bw);

    seri_iter iter;
    seri_item item;
    seri_iter_init(&iter, bw.data, bw.offset);
    seri_iter_next(&iter, &item);
    CuAssertIntEquals(tc, SERI_ITEM_ARRAY_BEGIN, item.type);
    CuAssertIntEquals(tc, (int32_t)n, (int32_t)item.v.array_n);
    for (i = 0; i < n; i++) {
        seri_iter_next(&iter, &item);
        CuAssertTrue(tc, (int64_t)i == item.v.i);
    }
    seri_iter_next(&iter, &item);
    CuAssertIntEquals(tc, SERI_ITEM_NIL, item.type);

    binary_free(&bw);
}
// 嵌套 array：外层 [42, inner_array]，inner = {3.14, "hi"}
static void test_seri_array_nested(CuTest *tc) {
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    seri_append_array_start(&bw, 2);
    seri_append_int(&bw, 42);
    seri_append_array_start(&bw, 2);  // 嵌套
    seri_append_real(&bw, 3.14);
    seri_append_string(&bw, "hi", 2);
    seri_append_array_end(&bw);       // 内层 end
    seri_append_array_end(&bw);       // 外层 end

    seri_iter iter;
    seri_item item;
    seri_iter_init(&iter, bw.data, bw.offset);
    seri_iter_next(&iter, &item);
    CuAssertIntEquals(tc, SERI_ITEM_ARRAY_BEGIN, item.type);
    CuAssertIntEquals(tc, 2, (int32_t)item.v.array_n);
    seri_iter_next(&iter, &item);
    CuAssertTrue(tc, 42 == item.v.i);
    seri_iter_next(&iter, &item);
    CuAssertIntEquals(tc, SERI_ITEM_ARRAY_BEGIN, item.type);
    CuAssertIntEquals(tc, 2, (int32_t)item.v.array_n);
    seri_iter_next(&iter, &item);
    CuAssertIntEquals(tc, SERI_ITEM_REAL, item.type);
    CuAssertDblEquals(tc, 3.14, item.v.r, 1e-9);
    seri_iter_next(&iter, &item);
    CuAssertIntEquals(tc, SERI_ITEM_STRING, item.type);
    CuAssertIntEquals(tc, 2, (int32_t)item.v.s.len);
    seri_iter_next(&iter, &item);
    CuAssertIntEquals(tc, SERI_ITEM_NIL, item.type);  // 内层 end
    seri_iter_next(&iter, &item);
    CuAssertIntEquals(tc, SERI_ITEM_NIL, item.type);  // 外层 end

    binary_free(&bw);
}
// 错误流：截断 buffer / 非法 tag，iter_next 返回 -1
static void test_seri_invalid_stream(CuTest *tc) {
    // 构造一个完整 INT(QWORD) 然后截掉一半
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    seri_append_int(&bw, 0x1234567890ABCDEFLL);

    seri_iter iter;
    seri_item item;
    seri_iter_init(&iter, bw.data, bw.offset - 3);  // 截掉末尾 3 字节
    CuAssertIntEquals(tc, -1, seri_iter_next(&iter, &item));
    binary_free(&bw);

    // 非法 tag：低 3 位 = 7（未定义类型）
    char bad = 0x07;
    seri_iter_init(&iter, &bad, 1);
    CuAssertIntEquals(tc, -1, seri_iter_next(&iter, &item));
}

void test_seri(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_seri_basic_nil_bool);
    SUITE_ADD_TEST(suite, test_seri_int_buckets);
    SUITE_ADD_TEST(suite, test_seri_real);
    SUITE_ADD_TEST(suite, test_seri_string);
    SUITE_ADD_TEST(suite, test_seri_userdata);
    SUITE_ADD_TEST(suite, test_seri_array_simple);
    SUITE_ADD_TEST(suite, test_seri_array_long);
    SUITE_ADD_TEST(suite, test_seri_array_nested);
    SUITE_ADD_TEST(suite, test_seri_invalid_stream);
}
