#ifndef TASK_LISTEN_UNLISTEN_RACE_H_
#define TASK_LISTEN_UNLISTEN_RACE_H_

#include "lib.h"

// SO_REUSEPORT + 多 watcher 下 ev_unlisten 与 in-flight accept 的并发压力测试。
// 每轮 task_listen → coro_fork N 个并发 client connect → 短 sleep → ev_unlisten → 等 cleanup。
// 30 轮全部跑完 *ok=1；ASan 不报 UAF / heap-buffer-overflow 即视为通过。
void task_listen_unlisten_race_start(loader_ctx *loader, const char *name, uint16_t port, int32_t *ok);

#endif//TASK_LISTEN_UNLISTEN_RACE_H_
