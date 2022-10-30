#include "netaddr.h"

static inline int32_t _checkipv4(const char *phost)
{
    for (int32_t i = 0; i < (int32_t)strlen(phost); i++)
    {
        if (phost[i] == '.')
        {
            return ERR_OK;
        }
        if (phost[i] == ':')
        {
            return ERR_FAILED;
        }
        if (phost[i] < '0' || phost[i] > '9')
        {
            return ERR_FAILED;
        }
    }

    return ERR_FAILED;
}
void netaddr_empty_addr(union netaddr_ctx *pctx, const int32_t ifamily)
{
    ZERO(pctx, sizeof(union netaddr_ctx));
    pctx->addr.sa_family = ifamily;
}
int32_t netaddr_sethost(union netaddr_ctx *pctx, const char *phost, const uint16_t usport)
{
    ZERO(pctx, sizeof(union netaddr_ctx));
    if (ERR_OK == _checkipv4(phost))
    {
        pctx->addr.sa_family = AF_INET;
        int32_t irtn = inet_pton(AF_INET, phost, &pctx->ipv4.sin_addr.s_addr);
        if (irtn < ERR_OK)
        {
            return irtn;
        }
        pctx->ipv4.sin_port = htons(usport);
    }
    else
    {
        pctx->addr.sa_family = AF_INET6;
        int32_t irtn = inet_pton(AF_INET6, phost, &pctx->ipv6.sin6_addr.s6_addr);
        if (irtn < ERR_OK)
        {
            return irtn;
        }
        pctx->ipv6.sin6_port = htons(usport);
    }

    return ERR_OK;
}
int32_t netaddr_remoteaddr(union netaddr_ctx *pctx, SOCKET fd, const int32_t ifamily)
{
    if (INVALID_SOCK == fd)
    {
        return ERR_FAILED;
    }

    ZERO(pctx, sizeof(union netaddr_ctx));
    pctx->addr.sa_family = ifamily;
    if (AF_INET == ifamily)
    {
        struct sockaddr_in addr = { 0 };
        socklen_t iaddrlens = (socklen_t)sizeof(struct sockaddr_in);
        if (ERR_OK != getpeername(fd, (struct sockaddr *)&addr, &iaddrlens))
        {
            return ERRNO;
        }
        memcpy(&pctx->ipv4, &addr, sizeof(pctx->ipv4));
    }
    else
    {
        struct sockaddr_in6 addr = { 0 };
        socklen_t iaddrlens = (socklen_t)sizeof(struct sockaddr_in6);
        if (ERR_OK != getpeername(fd, (struct sockaddr *)&addr, &iaddrlens))
        {
            return ERRNO;
        }
        memcpy(&pctx->ipv6, &addr, sizeof(pctx->ipv6));
    }
    
    return ERR_OK;
}
int32_t netaddr_localaddr(union netaddr_ctx *pctx, SOCKET fd, const int32_t ifamily)
{
    if (INVALID_SOCK == fd)
    {
        return ERR_FAILED;
    }

    ZERO(pctx, sizeof(union netaddr_ctx));
    pctx->addr.sa_family = ifamily;
    if (AF_INET == ifamily)
    {
        struct sockaddr_in addr = { 0 };
        socklen_t iaddrlens = (socklen_t)sizeof(struct sockaddr_in);
        if (ERR_OK != getsockname(fd, (struct sockaddr *)&addr, &iaddrlens))
        {
            return ERRNO;
        }
        memcpy(&pctx->ipv4, &addr, sizeof(pctx->ipv4));
    }
    else
    {
        struct sockaddr_in6 addr = { 0 };
        socklen_t iaddrlens = (socklen_t)sizeof(struct sockaddr_in6);
        if (ERR_OK != getsockname(fd, (struct sockaddr *)&addr, &iaddrlens))
        {
            return ERRNO;
        }
        memcpy(&pctx->ipv6, &addr, sizeof(pctx->ipv6));
    }

    return ERR_OK;
}
struct sockaddr *netaddr_addr(union netaddr_ctx *pctx)
{
    return &pctx->addr;
}
socklen_t netaddr_size(union netaddr_ctx *pctx)
{
    return AF_INET == pctx->addr.sa_family ? (socklen_t)sizeof(pctx->ipv4) : (socklen_t)sizeof(pctx->ipv6);
}
int32_t netaddr_ip(union netaddr_ctx *pctx, char acip[IP_LENS])
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
}
uint16_t netaddr_port(union netaddr_ctx *pctx)
{
    return AF_INET == pctx->addr.sa_family ? ntohs(pctx->ipv4.sin_port) : ntohs(pctx->ipv6.sin6_port);
}
int32_t netaddr_family(union netaddr_ctx *pctx)
{
    return pctx->addr.sa_family;
}
