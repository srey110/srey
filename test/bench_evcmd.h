#ifndef BENCH_EVCMD_H_
#define BENCH_EVCMD_H_

/// <summary>
/// event 命令通道两种实现的性能对比(仅非 Windows / pipe 平台):
/// A) pipe 直接发数据:每条 cmd 整体 write 进 pipe,消费者 read 批量取(srey 现状);
/// B) pipe 触发 + queue+spinlock:数据进 queue,pipe 仅 write 1 字节信号(合并),消费者读信号后从 queue 取。
/// 多生产者 + 单消费者(event 线程)。结果经 LOG_INFO 输出。Windows 下为空实现。
/// </summary>
void bench_evcmd(void);

#endif//BENCH_EVCMD_H_
