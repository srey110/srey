#ifndef TASK_UDP_SERVER_H_
#define TASK_UDP_SERVER_H_

#include "lib.h"

// 启动 UDP 回显服务端任务，收到数据报后原样发回给发送方
// 用于测试客户端侧 coro_sendto 的收发正确性
void task_udp_server_start(loader_ctx *loader, const char *name, uint16_t port, int32_t pt);

#endif//TASK_UDP_SERVER_H_
