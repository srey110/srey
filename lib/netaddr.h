#ifndef NETADDR_H_
#define NETADDR_H_

#include "macro.h"

#define  IPV4 0
#define  IPV6 1
struct netaddr_ctx
{
    int32_t m_type;
    struct sockaddr_in	m_ipv4;
    struct sockaddr_in6 m_ipv6;
}netaddr_ctx;

/*
* \brief          设置地址
* \param phost    ip
* \param usport   port
* \param bipv6    是否IP V6
* \return         ERR_OK 成功
*/
int32_t netaddr_sethost(struct netaddr_ctx *pctx, const char *phost, const uint16_t usport);
/*
* \brief          获取远端地址信息
* \param fd       SOCKET
* \return         ERR_OK 成功
*/
int32_t netaddr_remoteaddr(struct netaddr_ctx *pctx, const SOCKET fd);
/*
* \brief          获取本地地址信息
* \param fd       SOCKET
* \return         ERR_OK 成功
*/
int32_t netaddr_localaddr(struct netaddr_ctx *pctx, const SOCKET fd);
/*
* \brief          返回地址
* \return         sockaddr *
*/
struct sockaddr *netaddr_addr(struct netaddr_ctx *pctx);
/*
* \brief          地址长度
* \return         地址长度
*/
socklen_t netaddr_size(struct netaddr_ctx *pctx);
/*
* \brief          获取IP
* \param acip     ip
* \return         ERR_OK 成功
*/
int32_t netaddr_ip(struct netaddr_ctx *pctx, char acip[IP_LENS]);
/*
* \brief          获取端口
* \return         端口
*/
uint16_t netaddr_port(struct netaddr_ctx *pctx);
/*
* \brief          是否为ipv4
* \return         ERR_OK 是
*/
int32_t netaddr_isipv4(struct netaddr_ctx *pctx);
/*
* \return         AF_INET or AF_INET6;
*/
int32_t netaddr_addrfamily(struct netaddr_ctx *pctx);

#endif//NETADDR_H_
