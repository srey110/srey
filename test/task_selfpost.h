#ifndef TASK_SELFPOST_H_
#define TASK_SELFPOST_H_

#include "lib.h"

// task 自投递不死锁回归测试:
//   task 在自身 STARTUP 的 dispatch 内连续向自己 task_call, 条数远超 qumsg 容量。
//   此时 qumsg 的唯一消费者正是执行 _startup 的 worker(task->global=1 排他),
//   若入队在队列满时阻塞自旋, 该 worker 永久卡死且无人能排空 —— 硬死锁。
//   修复后 fsqu_push 满时降级到无界溢出层, 全部消息应被收齐。
// 平台覆盖差异(勿把本用例的绿灯当成降级路径已验证):
//   FSQU_MPQ=1(Linux/Windows): 真正检测降级路径, 但回归表现为挂死整个测试进程而非断言失败
//     —— 且 ./bin/test 本就阻塞等 SIGINT, 挂死与正常等待在输出上无法区分。
//   FSQU_MPQ=0(macOS/BSD): fsqu_push 本就是无界 queue+spin, 不可能阻塞, 本用例恒真。
//   容器级"超容量 push 必返回 + 跨界 FIFO"由 test_containers.c 的 test_fsqu_overflow_fifo
//   平台无关覆盖, 且它跑在 CuTest 套件内, 挂死时能从上一条用例输出定位。
// 全部消息收齐后将 *ok 置 1。
void task_selfpost_start(loader_ctx *loader, const char *name, int32_t *ok);

#endif//TASK_SELFPOST_H_
