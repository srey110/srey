#ifndef TASK_FORK_H_
#define TASK_FORK_H_

#include "lib.h"

// coro_fork / coro_fork_wait 单元测试：
//   1) 单个 fork：fire-and-forget，验证 worker 被调用 + arg 透传
//   2) 多个 fork 顺序执行：fork 3 个 worker，验证都执行完
//   3) fork 内 yield（coro_sleep）：验证 fork 协程能正常 yield/resume
//   4) 嵌套 fork：fork 出来的协程内再 fork，验证内层 worker 被调
//   5) scatter-gather：coro_fork_wait 3 个 worker 并发 coro_sleep，验证总耗时 ≈ max
//   6) fork_wait 0 任务：立即返回 ERR_OK，不挂起
//   7) fork_wait 内多次 yield：每个 worker 多次 yield，验证 barrier 计数正确
// 全部 case 通过后将 *ok 置 1。
void task_fork_start(loader_ctx *loader, const char *name, int32_t *ok);

#endif//TASK_FORK_H_
