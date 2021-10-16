#include "netaddr.h"

SREY_NS_BEGIN

cnetaddr::cnetaddr() : m_type(IPV4)
{
    _clear();
}
cnetaddr::cnetaddr(const bool &ipv6)
{
    m_type = (ipv6 ? IPV6 : IPV4);
    _clear();
}
bool cnetaddr::setaddr(const char *phost, const uint16_t &usport)
{
    struct addrinfo staddr = { 0 };
    struct addrinfo *paddr = NULL;
    _clear();

    if (checkipv4(phost))
    {
        m_type = IPV4;
        staddr.ai_flags = AI_PASSIVE;
        staddr.ai_family = AF_INET;
    }
    else
    {
        m_type = IPV6;
        staddr.ai_flags = AI_PASSIVE;
        staddr.ai_family = AF_INET6;
    }

    int32_t irtn = getaddrinfo(phost, NULL, &staddr, &paddr);
    if (ERR_OK != irtn)
    {
        PRINTF("getaddrinfo(%s, NULL,...) failed. %s", phost, gai_strerror(irtn));
        if (NULL != paddr)
        {
            freeaddrinfo(paddr);
        }
        return false;
    }

    if (AF_INET == paddr->ai_family)
    {
        memcpy(&m_ipv4, paddr->ai_addr, paddr->ai_addrlen);
        m_ipv4.sin_port = htons(usport);
    }
    else
    {
        memcpy(&m_ipv6, paddr->ai_addr, paddr->ai_addrlen);
        m_ipv6.sin6_port = htons(usport);
    }
    freeaddrinfo(paddr);

    return true;
}

bool cnetaddr::setaddr(const struct sockaddr *paddr)
{
    if (NULL == paddr)
    {
        return false;
    }
    _clear();

    if (AF_INET == paddr->sa_family)
    {
        m_type = IPV4;
        memcpy(&m_ipv4, paddr, sizeof(m_ipv4));
    }
    else
    {
        m_type = IPV6;
        memcpy(&m_ipv6, paddr, sizeof(m_ipv6));
    }

    return true;
}
bool cnetaddr::setreaddr(const SOCKET &fd)
{
    if (INVALID_SOCK == fd)
    {
        return false;
    }

    sockaddr staddr;
    socklen_t isocklens = (socklen_t)sizeof(staddr);
    ZERO(&staddr, isocklens);
    _clear();

    if (ERR_OK != getpeername(fd, &staddr, &isocklens))
    {
        PRINTF("getpeername(%d, ...) failed. %s", (int32_t)fd, ERRORSTR(ERRNO));
        return false;
    }

    return setaddr(&staddr);
}
bool cnetaddr::setloaddr(const SOCKET &fd)
{
    if (INVALID_SOCK == fd)
    {
        return false;
    }

    sockaddr staddr;
    socklen_t isocklens = (socklen_t)sizeof(staddr);
    ZERO(&staddr, isocklens);
    _clear();

    if (ERR_OK != getsockname(fd, &staddr, &isocklens))
    {
        PRINTF("getsockname(%d, ...) failed %s", (int32_t)fd, ERRORSTR(ERRNO));
        return false;
    }

    return setaddr(&staddr);
}
sockaddr *cnetaddr::getaddr()
{
    if (IPV4 == m_type)
    {
        return (sockaddr*)&m_ipv4;
    }
    else
    {
        return (sockaddr*)&m_ipv6;
    }
}

socklen_t cnetaddr::getsize()
{
    if (IPV4 == m_type)
    {
        return (socklen_t)sizeof(m_ipv4);
    }
    else
    {
        return (socklen_t)sizeof(m_ipv6);
    }
}
std::string cnetaddr::getip() 
{
    int32_t irtn;
    char acip[128] = { 0 };
    int32_t ilens = (int32_t)sizeof(acip);

    if (IPV4 == m_type)
    {
        irtn = getnameinfo((const sockaddr*)&m_ipv4, (socklen_t)sizeof(m_ipv4), acip, ilens, NULL, 0, NI_NUMERICHOST);
    }
    else
    {
        irtn = getnameinfo((const sockaddr*)&m_ipv6, (socklen_t)sizeof(m_ipv6), acip, ilens, NULL, 0, NI_NUMERICHOST);
    }
    if (ERR_OK != irtn)
    {
        PRINTF("getnameinfo failed. return code %d, message %s", irtn, gai_strerror(irtn));
        return std::string("");
    }

    return std::string(acip);
}
uint16_t cnetaddr::getport()
{
    if (IPV4 == m_type)
    {
        return ntohs(m_ipv4.sin_port);
    }
    else
    {
        return ntohs(m_ipv6.sin6_port);
    }
}
void cnetaddr::_clear()
{
    ZERO(&m_ipv4, sizeof(m_ipv4));
    ZERO(&m_ipv6, sizeof(m_ipv6));
    m_ipv4.sin_family = AF_INET;
    m_ipv6.sin6_family = AF_INET6;
}
bool cnetaddr::checkipv4(const char *phost)
{
    for (int32_t i = INIT_NUMBER; i < strlen(phost); i++)
    {
        if (phost[i] == '.')
        {
            return true;
        }
        if (phost[i] == ':')
        {
            return false;
        }
        if (phost[i] < '0' || phost[i] > '9')
        {
            return false;
        }
    }

    return false;
}

SREY_NS_END
