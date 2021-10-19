#include "netaddr.h"

void _clear(struct netaddr_ctx *pctx)
{
    ZERO(&pctx->m_ipv4, sizeof(pctx->m_ipv4));
    ZERO(&pctx->m_ipv6, sizeof(pctx->m_ipv6));
    pctx->m_ipv4.sin_family = AF_INET;
    pctx->m_ipv6.sin6_family = AF_INET6;
}
int32_t _checkipv4(const char *phost)
{
    for (int32_t i = 0; i < strlen(phost); i++)
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
int32_t netaddr_sethost(struct netaddr_ctx *pctx, const char *phost, const uint16_t usport)
{
    struct addrinfo staddr = { 0 };
    struct addrinfo *paddr = NULL;
    _clear(pctx);

    if (ERR_OK == _checkipv4(phost))
    {
        pctx->m_type = IPV4;
        staddr.ai_flags = AI_PASSIVE;
        staddr.ai_family = AF_INET;
    }
    else
    {
        pctx->m_type = IPV6;
        staddr.ai_flags = AI_PASSIVE;
        staddr.ai_family = AF_INET6;
    }

    int32_t irtn = getaddrinfo(phost, NULL, &staddr, &paddr);
    if (ERR_OK != irtn)
    {
        if (NULL != paddr)
        {
            freeaddrinfo(paddr);
        }
        return irtn;
    }

    if (AF_INET == paddr->ai_family)
    {
        memcpy(&pctx->m_ipv4, paddr->ai_addr, paddr->ai_addrlen);
        pctx->m_ipv4.sin_port = htons(usport);
    }
    else
    {
        memcpy(&pctx->m_ipv6, paddr->ai_addr, paddr->ai_addrlen);
        pctx->m_ipv6.sin6_port = htons(usport);
    }
    freeaddrinfo(paddr);

    return ERR_OK;
}
int32_t _setaddr(struct netaddr_ctx *pctx, const struct sockaddr *paddr)
{
    if (NULL == paddr)
    {
        return ERR_FAILED;
    }
    _clear(pctx);

    if (AF_INET == paddr->sa_family)
    {
        pctx->m_type = IPV4;
        memcpy(&pctx->m_ipv4, paddr, sizeof(pctx->m_ipv4));
    }
    else
    {
        pctx->m_type = IPV6;
        memcpy(&pctx->m_ipv6, paddr, sizeof(pctx->m_ipv6));
    }

    return ERR_OK;
}
int32_t netaddr_remoteaddr(struct netaddr_ctx *pctx, const SOCKET fd)
{
    if (INVALID_SOCK == fd)
    {
        return ERR_FAILED;
    }

    struct sockaddr staddr;
    socklen_t isocklens = (socklen_t)sizeof(struct sockaddr);
    ZERO(&staddr, isocklens);
    _clear(pctx);

    if (ERR_OK != getpeername(fd, &staddr, &isocklens))
    {
        return ERRNO;
    }

    return _setaddr(pctx, &staddr);
}
int32_t netaddr_localaddr(struct netaddr_ctx *pctx, const SOCKET fd)
{
    if (INVALID_SOCK == fd)
    {
        return ERR_FAILED;
    }

    struct sockaddr staddr;
    socklen_t isocklens = (socklen_t)sizeof(struct sockaddr);
    ZERO(&staddr, isocklens);
    _clear(pctx);

    if (ERR_OK != getsockname(fd, &staddr, &isocklens))
    {
        return ERRNO;
    }

    return _setaddr(pctx, &staddr);
}
struct sockaddr *netaddr_addr(struct netaddr_ctx *pctx)
{
    if (IPV4 == pctx->m_type)
    {
        return (struct sockaddr*)&pctx->m_ipv4;
    }
    else
    {
        return (struct sockaddr*)&pctx->m_ipv6;
    }
}
socklen_t netaddr_size(struct netaddr_ctx *pctx)
{
    if (IPV4 == pctx->m_type)
    {
        return (socklen_t)sizeof(pctx->m_ipv4);
    }
    else
    {
        return (socklen_t)sizeof(pctx->m_ipv6);
    }
}
int32_t netaddr_ip(struct netaddr_ctx *pctx, char acip[IP_LENS])
{
    int32_t irtn;
    int32_t ilens = (int32_t)sizeof(acip);
    ZERO(acip, ilens);

    if (IPV4 == pctx->m_type)
    {
        irtn = getnameinfo((struct sockaddr*)&pctx->m_ipv4, (socklen_t)sizeof(pctx->m_ipv4), acip, ilens, NULL, 0, NI_NUMERICHOST);
    }
    else
    {
        irtn = getnameinfo((struct sockaddr*)&pctx->m_ipv6, (socklen_t)sizeof(pctx->m_ipv6), acip, ilens, NULL, 0, NI_NUMERICHOST);
    }

    return irtn;
}
uint16_t netaddr_port(struct netaddr_ctx *pctx)
{
    if (IPV4 == pctx->m_type)
    {
        return ntohs(pctx->m_ipv4.sin_port);
    }
    else
    {
        return ntohs(pctx->m_ipv6.sin6_port);
    }
}
int32_t netaddr_isipv4(struct netaddr_ctx *pctx)
{
    return IPV4 == pctx->m_type ? ERR_OK : ERR_FAILED;
}
int32_t netaddr_addrfamily(struct netaddr_ctx *pctx)
{
    return IPV4 == pctx->m_type ? AF_INET : AF_INET6;
}
