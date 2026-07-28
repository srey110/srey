#include "bench_hashmap.h"
#include "test_rand.h"
#include "lib.h"
#include "containers/hashmap.h"

// 单轮操作数;上游 benchmarks() 默认 5000000,这里取一半量级以压缩单次跑批时长
#define BH_N 2000000

// 固定种子:各轮 shuffle 可复现,且不扰动全局 rand()
static test_rng _bh_rng;
// set 轮次内 hashmap 自身的分配次数,用于观察扩容开销
static uint64_t _bh_allocs;

static void *_bh_malloc(size_t size) {
    _bh_allocs++;
    return malloc(size);
}
static void *_bh_realloc(void *ptr, size_t size) {
    return realloc(ptr, size);
}
static void _bh_free(void *ptr) {
    free(ptr);
}
static uint64_t _bh_hash(const void *item, uint64_t seed0, uint64_t seed1) {
    return hashmap_xxhash3(item, sizeof(int32_t), seed0, seed1);
}
static int _bh_cmp(const void *a, const void *b, void *udata) {
    (void)udata;
    return *(const int32_t *)a - *(const int32_t *)b;
}
static void _bh_report(const char *tag, uint64_t ms) {
    double secs = (double)ms / 1000.0;
    LOG_INFO("[bench_hashmap] %-14s %d ops in %llums, %.0f ns/op, %.0f op/sec",
             tag, BH_N, (unsigned long long)ms,
             secs > 0.0 ? secs / (double)BH_N * 1e9 : 0.0,
             secs > 0.0 ? (double)BH_N / secs : 0.0);
}
// 跑一组 set/get/delete。cap=0 走默认初始容量(全程反复扩容),cap=BH_N 预留(无扩容)
static void _bh_round(const char *suffix, size_t cap, int32_t *vals) {
    struct hashmap *map;
    uint64_t t0;
    int32_t i;
    char name[32];

    _bh_allocs = 0;
    map = hashmap_new_with_allocator(_bh_malloc, _bh_realloc, _bh_free,
                                     sizeof(int32_t), cap, 1, 2,
                                     _bh_hash, _bh_cmp, NULL, NULL);
    if (NULL == map) {
        LOG_ERROR("[bench_hashmap] hashmap_new failed, cap %zu.", cap);
        return;
    }
    test_shuffle(&_bh_rng, vals, BH_N);
    t0 = nowms();
    for (i = 0; i < BH_N; i++) {
        (void)hashmap_set(map, &vals[i]);
    }
    SNPRINTF(name, sizeof(name), "set%s", suffix);
    _bh_report(name, nowms() - t0);
    LOG_INFO("[bench_hashmap] %-14s allocs=%llu", name, (unsigned long long)_bh_allocs);

    test_shuffle(&_bh_rng, vals, BH_N);
    t0 = nowms();
    for (i = 0; i < BH_N; i++) {
        (void)hashmap_get(map, &vals[i]);
    }
    SNPRINTF(name, sizeof(name), "get%s", suffix);
    _bh_report(name, nowms() - t0);

    test_shuffle(&_bh_rng, vals, BH_N);
    t0 = nowms();
    for (i = 0; i < BH_N; i++) {
        (void)hashmap_delete(map, &vals[i]);
    }
    SNPRINTF(name, sizeof(name), "delete%s", suffix);
    _bh_report(name, nowms() - t0);

    hashmap_free(map);
}
void bench_hashmap(void) {
    int32_t *vals;
    int32_t i;

    test_rng_init(&_bh_rng, 88172645463325252ULL);
    MALLOC(vals, sizeof(int32_t) * BH_N);
    for (i = 0; i < BH_N; i++) {
        vals[i] = i;
    }
    LOG_INFO("[bench_hashmap] === set/get/delete, N=%d, item=%zu ===", BH_N, sizeof(int32_t));
    _bh_round("", 0, vals);
    _bh_round(" (cap)", BH_N, vals);
    FREE(vals);
}
