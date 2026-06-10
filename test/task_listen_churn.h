#ifndef TASK_LISTEN_CHURN_H_
#define TASK_LISTEN_CHURN_H_

#include "lib.h"

// Listener 动态生命周期回归测试：
//   循环 N 次 task_listen → coro_connect → ev_close → ev_unlisten 同一端口，
//   重点暴露 lib/event 在 accept 事件与 listener 注销并发时的引用计数/合并路径
//   （针对 CMD_LSN_UNREF + qtn 隔离队列延后释放路径）。
// 全部循环跑完后将 *ok 置 1；中间出错立即 LOG_ERROR 并返回。
void task_listen_churn_start(loader_ctx *loader, const char *name, uint16_t port, int32_t *ok);

#endif//TASK_LISTEN_CHURN_H_
