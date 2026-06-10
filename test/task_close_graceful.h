#ifndef TASK_CLOSE_GRACEFUL_H_
#define TASK_CLOSE_GRACEFUL_H_

#include "lib.h"

// ev_close(immed=0) 优雅关闭数据完整性回归测试。
// 每轮 client coro_connect → ev_send N 字节 → ev_close(immed=0),
// 同 task 内 server accept 端累计 _net_recv 字节数,验证 server 端收到完整数据。
// ROUNDS 轮 × BYTES_PER_ROUND 全部到齐且 server close_cb 触发 ROUNDS 次,*ok=1。
void task_close_graceful_start(loader_ctx *loader, const char *name, uint16_t port, int32_t *ok);

#endif//TASK_CLOSE_GRACEFUL_H_
