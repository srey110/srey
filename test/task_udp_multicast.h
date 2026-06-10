#ifndef TASK_UDP_MULTICAST_H_
#define TASK_UDP_MULTICAST_H_

#include "lib.h"

// ev_udp_join / ev_udp_ttl / ev_udp_loop / ev_udp_leave 集成测试：
//   单 task 创建一个 UDP socket bind 端口 PORT,udp_join "239.99.99.99",
//   设 udp_ttl=1(默认值再设一次验证 API) + udp_loop=1(本机回环),
//   自己 ev_sendto 到多播组,_net_recvfrom 回调内验证收到内容并计数;
//   末尾调 ev_udp_leave 验证离开 API 不报错。ASan 跑无 leak 证明 udp_opt_arg 释放路径正确。
void task_udp_multicast_start(loader_ctx *loader, const char *name, uint16_t port, int32_t *ok);

#endif//TASK_UDP_MULTICAST_H_
