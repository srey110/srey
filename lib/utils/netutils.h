#ifndef NETUTILS_H_
#define NETUTILS_H_

#include "base/macro.h"

void sock_init(void);
void sock_clean(void);
//获取socket可读长度
int32_t sock_nread(SOCKET fd);
//错误状态
int32_t sock_error(SOCKET fd);
int32_t sock_checkconn(SOCKET fd);
//获取SO_TYPE(SOCK_STREAM  SOCK_DGRAM)
int32_t sock_type(SOCKET fd);
//获取sin_family(AF_INET AF_INET6)
int32_t sock_family(SOCKET fd);
//设置TCP_NODELAY
int32_t sock_nodelay(SOCKET fd);
//非阻塞
int32_t sock_nonblock(SOCKET fd);
//地址重用 SO_REUSEADDR
int32_t sock_reuseaddr(SOCKET fd);
//端口重用
int32_t sock_reuseport(SOCKET fd);
//SO_KEEPALIVE
int32_t sock_keepalive(SOCKET fd, const int32_t delay, const int32_t intvl);
//设置SO_LINGER 避免TIME_WAIT状态
int32_t sock_linger(SOCKET fd);
//一组相互链接的socket
int32_t sock_pair(SOCKET sock[2]);

#endif
