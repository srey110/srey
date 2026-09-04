#ifndef TASK_UDP_SERVER_H_
#define TASK_UDP_SERVER_H_

#include "lib.h"

// 启动 UDP 回显服务端任务，收到数据报后原样发回给发送方
// 用于测试客户端侧 coro_sendto 的收发正确性。
// 末参是监听的协议类型（回显要的是透传，传 PACK_NONE），不是同批其他 task_*_start 的打印开关
void task_udp_server_start(loader_ctx *loader, const char *name, uint16_t port, pack_type pktype);

#endif//TASK_UDP_SERVER_H_
