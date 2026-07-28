#ifndef TEST_RAND_H_
#define TEST_RAND_H_

#include "lib.h"

// 测试专用确定性 PRNG(xorshift64)。不复用 utils.h 的 randrange：那个是线程局部状态、
// 由 threadid 与时间戳自动播种，失败无法照日志复现；这里种子由调用方显式给定。
// 状态显式随参数传递，各测试文件各持一份，互不干扰
typedef struct test_rng {
    uint64_t s;
} test_rng;
// 播种；seed 为 0 时取 1，xorshift 全零状态会自锁
static inline void test_rng_init(test_rng *r, uint64_t seed) {
    r->s = (0 == seed) ? 1 : seed;
}
static inline uint64_t test_rng_next(test_rng *r) {
    r->s ^= r->s << 13;
    r->s ^= r->s >> 7;
    r->s ^= r->s << 17;
    return r->s;
}
// Fisher-Yates 洗牌，序列完全由 test_rng_init 的种子决定
static inline void test_shuffle(test_rng *r, int32_t *arr, int32_t n) {
    int32_t i, j, tmp;
    for (i = 0; i < n - 1; i++) {
        j = i + (int32_t)(test_rng_next(r) % (uint64_t)(n - i));
        tmp = arr[j];
        arr[j] = arr[i];
        arr[i] = tmp;
    }
}

#endif//TEST_RAND_H_
