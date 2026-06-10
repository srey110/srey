#ifndef TASK_CORO_EXTRA_H_
#define TASK_CORO_EXTRA_H_

#include "lib.h"

// 协程 API 边界/失败路径补充覆盖：
//   coro_sleep 1500ms（穿越 tv1 → tv2 时间轮 cascade）
//   coro_connect 拒绝（127.0.0.1:1 不可达端口）
//   coro_send 在已 ev_close 的 fd 上调用（peer-disconnect 路径）
//   dns_lookup 解析 example.com（DNS 协程路径）
//   coro_request 未知 rtype 触发请求超时
// 全部 case 通过后将 *ok 置 1；任何步骤失败立即 LOG_ERROR 并返回（不置位）。
// httpport 用于 send-after-close；rpcname 用于 coro_request 目标（任意已注册的 task）。
void task_coro_extra_start(loader_ctx *loader, const char *name, uint16_t httpport, const char *rpcname, int32_t *ok);

#endif//TASK_CORO_EXTRA_H_
