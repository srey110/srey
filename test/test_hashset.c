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
    CuAssertPtrEquals(tc, NULL, (void *)hashset_add(s, &v));
    // add 返 NULL 有"新增"与"OOM"两义,靠 oom 区分:成功新增时必须是 0
    CuAssertIntEquals(tc, 0, hashset_oom(s));
    CuAssertTrue(tc, 1 == hashset_contains(s, &v));
    CuAssertTrue(tc, 1 == hashset_count(s));

    CuAssertPtrNotNull(tc, hashset_remove(s, &v));
    CuAssertTrue(tc, 0 == hashset_contains(s, &v));
    CuAssertTrue(tc, 0 == hashset_count(s));

    hashset_free(s);
}
// 重复 add 覆写已存在元素,返回旧值指针;首次返 NULL
static void test_hs_replace(CuTest *tc) {
    hashset *s = hashset_new(sizeof(int32_t), 0, _int_hash, _int_cmp, NULL, NULL);
    int32_t v = 7;
    CuAssertPtrEquals(tc, NULL, (void *)hashset_add(s, &v));
    CuAssertPtrNotNull(tc, hashset_add(s, &v));
    CuAssertPtrNotNull(tc, hashset_add(s, &v));
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
    CuAssertTrue(tc, NULL == hashset_remove(s, &v));// 1 个元素,删除不存在
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
    CuAssertPtrEquals(tc, NULL, (void *)hashset_add(s, &v));
    CuAssertTrue(tc, 1 == hashset_count(s));

    // clear(update_cap=非 0)保留当前桶数、不分配
    hashset_clear(s, 1);
    CuAssertTrue(tc, 0 == hashset_count(s));

    hashset_free(s);
}
// scan 回调返 false 终止遍历
static int32_t _scan_count_then_stop(const void *item, void *udata) {
    (void)item;
    int32_t *cnt = (int32_t *)udata;
    (*cnt)++;
    return *cnt < 3 ? 1 : 0;// 第 3 个时返 false 停止
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
    // 全量 scan:走完返 1
    int32_t total = 0;
    CuAssertIntEquals(tc, 1, hashset_scan(s, _scan_count_all, &total));
    CuAssertTrue(tc, 10 == total);

    // 早停 scan:被 iter 提前终止返 0
    int32_t cnt = 0;
    CuAssertIntEquals(tc, 0, hashset_scan(s, _scan_count_then_stop, &cnt));
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
    const size_t len = 16;
    for (i = 0; i < 10; i++) {
        b.key = i;
        MALLOC(b.name, len);
        snprintf(b.name, len, "name_%d", i);
        hashset_add(s, &b);
    }
    CuAssertTrue(tc, 10 == hashset_count(s));
    // free 应触发 10 次 _bag_free
    hashset_free(s);
    CuAssertTrue(tc, 10 == g_bag_free_cnt);
}
// hashset_clear 是除 hashset_free 外唯一会自动调 elfree 的路径:两处 clear 用例都建在
// elfree=NULL 的 int 集合上,把 hashmap_clear 里的 free_elements 删掉一样全绿
static void test_hs_clear_elfree(CuTest *tc) {
    g_bag_free_cnt = 0;
    hashset *s = hashset_new(sizeof(_bag), 0, _bag_hash, _bag_cmp, _bag_free, NULL);
    _bag b;
    int32_t i;
    size_t len = 16;
    for (i = 0; i < 10; i++) {
        b.key = i;
        MALLOC(b.name, len);
        snprintf(b.name, len, "name_%d", i);
        hashset_add(s, &b);
    }
    hashset_clear(s, 0);
    CuAssertTrue(tc, 0 == hashset_count(s));
    CuAssertIntEquals(tc, 10, g_bag_free_cnt);
    // clear 之后容器仍可用,再塞一轮由 free 收尾
    for (i = 0; i < 3; i++) {
        b.key = 100 + i;
        MALLOC(b.name, len);
        snprintf(b.name, len, "again_%d", i);
        hashset_add(s, &b);
    }
    hashset_free(s);
    CuAssertIntEquals(tc, 13, g_bag_free_cnt);
}
// hashset_free(NULL) 的 NULL 安全承诺:漏判会段错误终止整个 ./bin/test
static void test_hs_free_null(CuTest *tc) {
    uint64_t a0, f0, a1, f1;
    mem_stat(&a0, &f0);
    hashset_free(NULL);
    mem_stat(&a1, &f1);
    // 既不能崩，也不能凭空调一次 free
    CuAssertTrue(tc, a0 == a1 && f0 == f1);
}
// add 覆写已存在 key 时返回旧值指针,旧值不会被自动 elfree,需调用方按需处理其内部分配
static void test_hs_replace_elfree(CuTest *tc) {
    g_bag_free_cnt = 0;
    hashset *s = hashset_new(sizeof(_bag), 0, _bag_hash, _bag_cmp, _bag_free, NULL);
    _bag first;
    first.key = 1;
    MALLOC(first.name, 16);
    snprintf(first.name, 16, "first");
    CuAssertPtrEquals(tc, NULL, (void *)hashset_add(s, &first));

    _bag second;
    second.key = 1;
    MALLOC(second.name, 16);
    snprintf(second.name, 16, "second");
    _bag *old = (_bag *)hashset_add(s, &second);
    CuAssertPtrNotNull(tc, old);
    CuAssertTrue(tc, 0 == strcmp("first", old->name));
    FREE(old->name);

    CuAssertTrue(tc, 1 == hashset_count(s));
    hashset_free(s);
    CuAssertTrue(tc, 1 == g_bag_free_cnt);
}
// remove 返回被删元素副本,同样不自动 elfree(hashset_remove 直通 hashmap_delete):
// 元素内部的 strdup / MALLOC 字段全靠调用方拿返回值自己释放
static void test_hs_remove_elfree(CuTest *tc) {
    g_bag_free_cnt = 0;
    hashset *s = hashset_new(sizeof(_bag), 0, _bag_hash, _bag_cmp, _bag_free, NULL);
    _bag b;
    b.key = 3;
    MALLOC(b.name, 16);
    snprintf(b.name, 16, "gone");
    CuAssertPtrEquals(tc, NULL, (void *)hashset_add(s, &b));

    _bag key;
    key.key = 3;
    key.name = NULL;// 只按 key 定位,name 不参与 hash/compare
    _bag *removed = (_bag *)hashset_remove(s, &key);
    CuAssertPtrNotNull(tc, removed);
    CuAssertTrue(tc, 3 == removed->key);
    CuAssertTrue(tc, 0 == strcmp("gone", removed->name));
    CuAssertTrue(tc, 0 == hashset_count(s));
    // 契约要害:remove 一次都不许调 elfree,否则调用方紧接着的手动释放就是 double free
    CuAssertIntEquals(tc, 0, g_bag_free_cnt);

    // 改由调用方释放:计数恰好 +1
    _bag_free(removed);
    CuAssertIntEquals(tc, 1, g_bag_free_cnt);

    // 表已空,free 不再触发 elfree
    hashset_free(s);
    CuAssertIntEquals(tc, 1, g_bag_free_cnt);
}
// 大规模 10k 元素 add/contains/remove(ASan 验证内存安全)
static void test_hs_stress(CuTest *tc) {
    hashset *s = hashset_new(sizeof(int32_t), 0, _int_hash, _int_cmp, NULL, NULL);
    int32_t i;
    for (i = 0; i < 10000; i++) {
        CuAssertPtrEquals(tc, NULL, (void *)hashset_add(s, &i));
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
// clear 后再次填满:验证清空后容器仍可用。容量语义本身不在此断言,见 test_hashmap_clear_alloc
static void test_hs_clear_refill(CuTest *tc) {
    hashset *s = hashset_new(sizeof(int32_t), 64, _int_hash, _int_cmp, NULL, NULL);
    int32_t i;
    for (i = 0; i < 50; i++) {
        hashset_add(s, &i);
    }
    hashset_clear(s, 0);// 重新分配桶数组缩回建表 cap
    CuAssertTrue(tc, 0 == hashset_count(s));
    // 再次填满,验证可用
    for (i = 0; i < 50; i++) {
        CuAssertPtrEquals(tc, NULL, (void *)hashset_add(s, &i));
    }
    CuAssertTrue(tc, 50 == hashset_count(s));
    hashset_free(s);
}

// 注册套件
void test_hashset(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_hs_basic);
    SUITE_ADD_TEST(suite, test_hs_replace);
    SUITE_ADD_TEST(suite, test_hs_remove_missing);
    SUITE_ADD_TEST(suite, test_hs_clear);
    SUITE_ADD_TEST(suite, test_hs_scan);
    SUITE_ADD_TEST(suite, test_hs_iter);
    SUITE_ADD_TEST(suite, test_hs_elfree);
    SUITE_ADD_TEST(suite, test_hs_replace_elfree);
    SUITE_ADD_TEST(suite, test_hs_remove_elfree);
    SUITE_ADD_TEST(suite, test_hs_stress);
    SUITE_ADD_TEST(suite, test_hs_invalid);
    SUITE_ADD_TEST(suite, test_hs_clear_refill);
    SUITE_ADD_TEST(suite, test_hs_clear_elfree);
    SUITE_ADD_TEST(suite, test_hs_free_null);
}
