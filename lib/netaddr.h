#ifndef NETADDR_H_
#define NETADDR_H_

#include "macro.h"
union netaddr_ctx
{
    struct sockaddr addr;
    struct sockaddr_in ipv4;
    struct sockaddr_in6 ipv6;
};
static inline void netaddr_empty_addr(union netaddr_ctx *pctx, const int32_t ifamily)
{
    ZERO(&pctx->ipv6, sizeof(pctx->ipv6));
    pctx->addr.sa_family = ifamily;
};
/*
* \brief          设置地址
* \param phost    ip
* \param usport   port
* \return         ERR_OK 成功 gai_strerror获取错误信息
*/
int32_t netaddr_sethost(union netaddr_ctx *pctx, const char *phost, const uint16_t usport);
/*
* \brief          设置地址
* \param phost    ip
* \param usport   port
* \return         ERR_OK 成功
*/
void netaddr_setaddr(union netaddr_ctx *pctx, const struct sockaddr *paddr);
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
static inline struct sockaddr *netaddr_addr(union netaddr_ctx *pctx)
{
    return &pctx->addr;
};
/*
* \brief          地址长度
* \return         地址长度
*/
static inline socklen_t netaddr_size(union netaddr_ctx *pctx)
{
    return AF_INET == pctx->addr.sa_family ? (socklen_t)sizeof(pctx->ipv4) : (socklen_t)sizeof(pctx->ipv6);
};
/*
* \brief          获取IP
* \param acip     ip
* \return         ERR_OK 成功
*/
static inline int32_t netaddr_ip(union netaddr_ctx *pctx, char acip[IP_LENS])
{
    ZERO(acip, IP_LENS);
    if (AF_INET == pctx->addr.sa_family)
    {
        if (NULL == inet_ntop(AF_INET, &pctx->ipv4.sin_addr, acip, IP_LENS))
        {
            PRINTF("inet_ntop failed, %s", ERRORSTR(ERRNO));
            return ERR_FAILED;
        }
    }
    else
    {
        if (NULL == inet_ntop(AF_INET6, &pctx->ipv6.sin6_addr, acip, IP_LENS))
        {
            PRINTF("inet_ntop failed, %s", ERRORSTR(ERRNO));
            return ERR_FAILED;
        }
    }

    return ERR_OK;
};
/*
* \brief          获取端口
* \return         端口
*/
static inline uint16_t netaddr_port(union netaddr_ctx *pctx)
{
    return AF_INET == pctx->addr.sa_family ? ntohs(pctx->ipv4.sin_port) : ntohs(pctx->ipv6.sin6_port);
};
/*
* \return         AF_INET or AF_INET6;
*/
static inline int32_t netaddr_family(union netaddr_ctx *pctx)
{
    return pctx->addr.sa_family;
};

#endif//NETADDR_H_
