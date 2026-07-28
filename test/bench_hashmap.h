#ifndef BENCH_HASHMAP_H_
#define BENCH_HASHMAP_H_

/// <summary>
/// hashmap 的 set / get / delete 吞吐对比,分默认初始容量(全程扩容)与预留容量(无扩容)两组。
/// 移自 hashmap.c 原 #ifdef HASHMAP_TEST 块内的 benchmarks();正确性断言已移入 test_containers
/// 的 test_hashmap_upstream_* 用例,此处只计时不断言,避免污染测量。结果经 LOG_INFO 输出。
/// </summary>
void bench_hashmap(void);

#endif//BENCH_HASHMAP_H_
