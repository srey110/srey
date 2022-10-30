#ifndef NETADDR_H_
#define NETADDR_H_

#include "macro.h"

union netaddr_ctx
{
    struct sockaddr addr;
    struct sockaddr_in ipv4;
    struct sockaddr_in6 ipv6;
};

void netaddr_empty_addr(union netaddr_ctx *pctx, const int32_t ifamily);
/*
* \brief          设置地址
* \param phost    ip
* \param usport   port
* \return         ERR_OK 成功 gai_strerror获取错误信息
*/
int32_t netaddr_sethost(union netaddr_ctx *pctx, const char *phost, const uint16_t usport);
/*
* \brief          获取远端地址信息
* \param fd       SOCKET
* \return         ERR_OK 成功
*/
int32_t netaddr_remoteaddr(union netaddr_ctx *pctx, SOCKET fd, const int32_t ifamily);
/*
* \brief          获取本地地址信息
* \param fd       SOCKET
* \return         ERR_OK 成功
*/
int32_t netaddr_localaddr(union netaddr_ctx *pctx, SOCKET fd, const int32_t ifamily);
/*
* \brief          返回地址
* \return         sockaddr *
*/
struct sockaddr *netaddr_addr(union netaddr_ctx *pctx);
/*
* \brief          地址长度
* \return         地址长度
*/
socklen_t netaddr_size(union netaddr_ctx *pctx);
/*
* \brief          获取IP
* \param acip     ip
* \return         ERR_OK 成功
*/
int32_t netaddr_ip(union netaddr_ctx *pctx, char acip[IP_LENS]);
/*
* \brief          获取端口
* \return         端口
*/
uint16_t netaddr_port(union netaddr_ctx *pctx);
/*
* \return         AF_INET or AF_INET6;
*/
int32_t netaddr_family(union netaddr_ctx *pctx);

#endif//NETADDR_H_
