#ifndef BENCH_MPQ_H_
#define BENCH_MPQ_H_

// FSQU_FAST_MODEL 三个后端(queue+spin / mpq / bbq)直接用 fsqu 本体实例化的入队出队对比,
// 分纯入队、MPSC 逐条出队、MPSC 批量出队三个维度,容量取产线几档,详见 bench_mpq.c 文件头。结果经 LOG_INFO 输出。
void bench_mpq(void);

#endif//BENCH_MPQ_H_
