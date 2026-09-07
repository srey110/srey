#ifndef BENCH_MPQ_H_
#define BENCH_MPQ_H_

// mpq(无锁多生产者队列)与 普通 queue + spinlock 的多生产者并发 push 性能对比。
// 多生产者并发入队场景下,mpq 走无锁 CAS,queue 需 spinlock 串行化。结果经 LOG_INFO 输出。
void bench_mpq(void);

#endif//BENCH_MPQ_H_
