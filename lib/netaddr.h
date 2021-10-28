#ifndef NETADDR_H_
#define NETADDR_H_

#include "macro.h"

#define  IPV4 0
#define  IPV6 1
struct netaddr_ctx
{
    int32_t type;
    struct sockaddr_in	ipv4;
    struct sockaddr_in6 ipv6;
}netaddr_ctx;
/*
* \brief          设置地址
* \param phost    ip
* \param usport   port
* \return         ERR_OK 成功 gai_strerror获取错误信息
*/
int32_t netaddr_sethost(struct netaddr_ctx *pctx, const char *phost, const uint16_t usport);
/*
* \brief          设置地址
* \param phost    ip
* \param usport   port
* \return         ERR_OK 成功
*/
int32_t netaddr_setaddr(struct netaddr_ctx *pctx, const struct sockaddr *paddr);
/*
* \brief          获取远端地址信息
* \param fd       SOCKET
* \return         ERR_OK 成功
*/
int32_t netaddr_remoteaddr(struct netaddr_ctx *pctx, SOCKET fd);
/*
* \brief          获取本地地址信息
* \param fd       SOCKET
* \return         ERR_OK 成功
*/
int32_t netaddr_localaddr(struct netaddr_ctx *pctx, SOCKET fd);
/*
* \brief          返回地址
* \return         sockaddr *
*/
static inline struct sockaddr *netaddr_addr(struct netaddr_ctx *pctx)
{
    if (IPV4 == pctx->type)
    {
        return (struct sockaddr*)&pctx->ipv4;
    }
    else
    {
        return (struct sockaddr*)&pctx->ipv6;
    }
};
/*
* \brief          地址长度
* \return         地址长度
*/
static inline socklen_t netaddr_size(struct netaddr_ctx *pctx)
{
    if (IPV4 == pctx->type)
    {
        return (socklen_t)sizeof(pctx->ipv4);
    }
    else
    {
        return (socklen_t)sizeof(pctx->ipv6);
    }
};
/*
* \brief          获取IP
* \param acip     ip
* \return         ERR_OK 成功 gai_strerror获取错误信息
*/
static inline int32_t netaddr_ip(struct netaddr_ctx *pctx, char acip[IP_LENS])
{
    int32_t irtn;
    int32_t ilens = (int32_t)sizeof(acip);
    ZERO(acip, ilens);

    if (IPV4 == pctx->type)
    {
        irtn = getnameinfo((struct sockaddr*)&pctx->ipv4, (socklen_t)sizeof(pctx->ipv4),
            acip, ilens, NULL, 0, NI_NUMERICHOST);
    }
    else
    {
        irtn = getnameinfo((struct sockaddr*)&pctx->ipv6, (socklen_t)sizeof(pctx->ipv6),
            acip, ilens, NULL, 0, NI_NUMERICHOST);
    }

    return irtn;
};
/*
* \brief          获取端口
* \return         端口
*/
static inline uint16_t netaddr_port(struct netaddr_ctx *pctx)
{
    if (IPV4 == pctx->type)
    {
        return ntohs(pctx->ipv4.sin_port);
    }
    else
    {
        return ntohs(pctx->ipv6.sin6_port);
    }
};
/*
* \brief          是否为ipv4
* \return         ERR_OK 是
*/
static inline int32_t netaddr_isipv4(struct netaddr_ctx *pctx)
{
    return IPV4 == pctx->type ? ERR_OK : ERR_FAILED;
};
/*
* \return         AF_INET or AF_INET6;
*/
static inline int32_t netaddr_addrfamily(struct netaddr_ctx *pctx)
{
    return IPV4 == pctx->type ? AF_INET : AF_INET6;
};

#endif//NETADDR_H_
