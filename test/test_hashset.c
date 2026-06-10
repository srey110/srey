#include "test_hashset.h"
#include "lib.h"

// 整数元素的 hash / compare
static uint64_t _int_hash(const void *item, uint64_t seed0, uint64_t seed1) {
    return hashmap_xxhash3(item, sizeof(int32_t), seed0, seed1);
}
static int _int_cmp(const void *a, const void *b, void *udata) {
    (void)udata;
    int32_t x = *(const int32_t *)a;
    int32_t y = *(const int32_t *)b;
    return x - y;
}

// add/contains/remove 基本 CRUD
static void test_hs_basic(CuTest *tc) {
    hashset *s = hashset_new(sizeof(int32_t), 0, _int_hash, _int_cmp, NULL, NULL);
    CuAssertPtrNotNull(tc, s);
    CuAssertTrue(tc, 0 == hashset_count(s));

    int32_t v = 42;
    CuAssertTrue(tc, 0 == hashset_contains(s, &v));
    CuAssertTrue(tc, 1 == hashset_add(s, &v));
    CuAssertTrue(tc, 1 == hashset_contains(s, &v));
    CuAssertTrue(tc, 1 == hashset_count(s));

    CuAssertPtrNotNull(tc, hashset_remove(s, &v));
    CuAssertTrue(tc, 0 == hashset_contains(s, &v));
    CuAssertTrue(tc, 0 == hashset_count(s));

    hashset_free(s);
}
// 重复 add 返 0,首次返 1
static void test_hs_dedup(CuTest *tc) {
    hashset *s = hashset_new(sizeof(int32_t), 0, _int_hash, _int_cmp, NULL, NULL);
    int32_t v = 7;
    CuAssertTrue(tc, 1 == hashset_add(s, &v));
    CuAssertTrue(tc, 0 == hashset_add(s, &v));
    CuAssertTrue(tc, 0 == hashset_add(s, &v));
    CuAssertTrue(tc, 1 == hashset_count(s));
    hashset_free(s);
}
// remove 不存在元素返 NULL
static void test_hs_remove_missing(CuTest *tc) {
    hashset *s = hashset_new(sizeof(int32_t), 0, _int_hash, _int_cmp, NULL, NULL);
    int32_t v = 99;
    CuAssertTrue(tc, NULL == hashset_remove(s, &v));
    int32_t w = 1;
    hashset_add(s, &w);
    CuAssertTrue(tc, NULL == hashset_remove(s, &v));    // 1 个元素,删除不存在
    CuAssertTrue(tc, 1 == hashset_count(s));
    hashset_free(s);
}
// clear 后 count=0,可再次 add
static void test_hs_clear(CuTest *tc) {
    hashset *s = hashset_new(sizeof(int32_t), 0, _int_hash, _int_cmp, NULL, NULL);
    int32_t v;
    for (v = 0; v < 100; v++) {
        hashset_add(s, &v);
    }
    CuAssertTrue(tc, 100 == hashset_count(s));

    hashset_clear(s, 0);
    CuAssertTrue(tc, 0 == hashset_count(s));
    v = 5;
    CuAssertTrue(tc, 0 == hashset_contains(s, &v));
    CuAssertTrue(tc, 1 == hashset_add(s, &v));
    CuAssertTrue(tc, 1 == hashset_count(s));

    // clear(update_cap=true)缩回初始容量
    hashset_clear(s, 1);
    CuAssertTrue(tc, 0 == hashset_count(s));

    hashset_free(s);
}
// scan 回调返 false 终止遍历
static int32_t _scan_count_then_stop(const void *item, void *udata) {
    (void)item;
    int32_t *cnt = (int32_t *)udata;
    (*cnt)++;
    return *cnt < 3 ? 1 : 0;   // 第 3 个时返 false 停止
}
static int32_t _scan_count_all(const void *item, void *udata) {
    (void)item;
    int32_t *cnt = (int32_t *)udata;
    (*cnt)++;
    return 1;
}
// scan 全量遍历 + 早停(回调返 0 终止)
static void test_hs_scan(CuTest *tc) {
    hashset *s = hashset_new(sizeof(int32_t), 0, _int_hash, _int_cmp, NULL, NULL);
    int32_t v;
    for (v = 1; v <= 10; v++) {
        hashset_add(s, &v);
    }
    // 全量 scan
    int32_t total = 0;
    hashset_scan(s, _scan_count_all, &total);
    CuAssertTrue(tc, 10 == total);

    // 早停 scan
    int32_t cnt = 0;
    hashset_scan(s, _scan_count_then_stop, &cnt);
    CuAssertTrue(tc, 3 == cnt);

    hashset_free(s);
}
// iter 完整遍历
static void test_hs_iter(CuTest *tc) {
    hashset *s = hashset_new(sizeof(int32_t), 0, _int_hash, _int_cmp, NULL, NULL);
    int32_t v;
    for (v = 100; v < 200; v++) {
        hashset_add(s, &v);
    }
    int32_t sum = 0;
    size_t i = 0;
    void *item;
    while (hashset_iter(s, &i, &item)) {
        sum += *(int32_t *)item;
    }
    int32_t expect = 0;
    for (v = 100; v < 200; v++) {
        expect += v;
    }
    CuAssertTrue(tc, expect == sum);
    hashset_free(s);
}
// elfree 回调被调正确次数:含指针字段的元素
typedef struct _bag {
    int32_t key;
    char *name;       // strdup,by elfree 释放
} _bag;
static int32_t g_bag_free_cnt;
static uint64_t _bag_hash(const void *item, uint64_t s0, uint64_t s1) {
    const _bag *b = (const _bag *)item;
    return hashmap_xxhash3(&b->key, sizeof(int32_t), s0, s1);
}
static int _bag_cmp(const void *a, const void *b, void *ud) {
    (void)ud;
    return ((const _bag *)a)->key - ((const _bag *)b)->key;
}
static void _bag_free(void *item) {
    _bag *b = (_bag *)item;
    FREE(b->name);
    g_bag_free_cnt++;
}
// elfree 回调:含指针字段元素,hashset_free 时按 count 次调用 _bag_free
static void test_hs_elfree(CuTest *tc) {
    g_bag_free_cnt = 0;
    hashset *s = hashset_new(sizeof(_bag), 0, _bag_hash, _bag_cmp, _bag_free, NULL);
    _bag b;
    int32_t i;
    for (i = 0; i < 10; i++) {
        b.key = i;
        size_t len = 16;
        MALLOC(b.name, len);
        snprintf(b.name, len, "name_%d", i);
        hashset_add(s, &b);
    }
    CuAssertTrue(tc, 10 == hashset_count(s));
    // free 应触发 10 次 _bag_free
    hashset_free(s);
    CuAssertTrue(tc, 10 == g_bag_free_cnt);
}
// 大规模 10k 元素 add/contains/remove(ASan 验证内存安全)
static void test_hs_stress(CuTest *tc) {
    hashset *s = hashset_new(sizeof(int32_t), 0, _int_hash, _int_cmp, NULL, NULL);
    int32_t i;
    for (i = 0; i < 10000; i++) {
        CuAssertTrue(tc, 1 == hashset_add(s, &i));
    }
    CuAssertTrue(tc, 10000 == hashset_count(s));
    // 全部存在
    for (i = 0; i < 10000; i++) {
        CuAssertTrue(tc, 1 == hashset_contains(s, &i));
    }
    // 全部移除
    for (i = 0; i < 10000; i++) {
        CuAssertPtrNotNull(tc, hashset_remove(s, &i));
    }
    CuAssertTrue(tc, 0 == hashset_count(s));
    hashset_free(s);
}
// 边界:NULL/0 参数返 NULL
static void test_hs_invalid(CuTest *tc) {
    CuAssertPtrEquals(tc, NULL, hashset_new(0, 0, _int_hash, _int_cmp, NULL, NULL));
    CuAssertPtrEquals(tc, NULL, hashset_new(sizeof(int32_t), 0, NULL, _int_cmp, NULL, NULL));
    CuAssertPtrEquals(tc, NULL, hashset_new(sizeof(int32_t), 0, _int_hash, NULL, NULL, NULL));
}
// 添加后 clear 不缩容 + 再次添加复用容量
static void test_hs_clear_no_shrink(CuTest *tc) {
    hashset *s = hashset_new(sizeof(int32_t), 64, _int_hash, _int_cmp, NULL, NULL);
    int32_t i;
    for (i = 0; i < 50; i++) {
        hashset_add(s, &i);
    }
    hashset_clear(s, 0);   // 不更新容量
    CuAssertTrue(tc, 0 == hashset_count(s));
    // 再次填满,验证可用
    for (i = 0; i < 50; i++) {
        CuAssertTrue(tc, 1 == hashset_add(s, &i));
    }
    CuAssertTrue(tc, 50 == hashset_count(s));
    hashset_free(s);
}

// 注册套件
void test_hashset(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_hs_basic);
    SUITE_ADD_TEST(suite, test_hs_dedup);
    SUITE_ADD_TEST(suite, test_hs_remove_missing);
    SUITE_ADD_TEST(suite, test_hs_clear);
    SUITE_ADD_TEST(suite, test_hs_scan);
    SUITE_ADD_TEST(suite, test_hs_iter);
    SUITE_ADD_TEST(suite, test_hs_elfree);
    SUITE_ADD_TEST(suite, test_hs_stress);
    SUITE_ADD_TEST(suite, test_hs_invalid);
    SUITE_ADD_TEST(suite, test_hs_clear_no_shrink);
}
