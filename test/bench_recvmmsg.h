#ifndef BENCH_RECVMMSG_H_
#define BENCH_RECVMMSG_H_

// 定某个平台该不该开 UDP 批量收(uev.h 的 UDP_RECV_BATCH)：同一台机器上逐个收(recvmsg)与批量收(recvmmsg)
// 交错跑一问一答与 32/256 包突发，比每包墙钟、服务端线程 CPU、收包调用数，最后给出建议，详见 bench_recvmmsg.c 文件头。
// 结果经 LOG_INFO 输出；没有 recvmmsg 的平台(Windows、macOS 等)只打一行说明
void bench_recvmmsg(void);

#endif//BENCH_RECVMMSG_H_
