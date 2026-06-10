#ifndef TASK_MULTICAST_H_
#define TASK_MULTICAST_H_

#include "lib.h"

// ev_send_multi 多播 API 集成测试：
//   1) task 自起 N=5 个 client 协程 coro_connect 到本 task listen 的端口
//   2) accept 端 _net_accept 把 server-side fd/skid 累积到数组,集齐 N 个后
//      调 ev_send_multi 一次性广播 "BROADCAST_HELLO" 给所有 server-side fd
//   3) 每个 client-side fd 收到 broadcast 在 _net_recv 内 received_count++
//   4) 验证 received_count == N (全部 client 收到广播 + 共享 pack ref 归 0 自动释放)
// ASan 下不报 leak 即证明引用计数路径正确。
void task_multicast_start(loader_ctx *loader, const char *name, uint16_t port, int32_t *ok);

#endif//TASK_MULTICAST_H_
