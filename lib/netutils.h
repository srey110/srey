#ifndef NETUTILS_H_
#define NETUTILS_H_

#include "macro.h"

/*
* \brief          获取socket可读长度
* \param fd       socket句柄
* \return         ERR_FAILED 失败
* \return         长度
*/
static inline int32_t socknread(SOCKET fd)
{
#if defined(OS_WIN)
    u_long ulread = 0;
    if (ioctlsocket(fd, FIONREAD, &ulread) < ERR_OK)
    {
        return ERR_FAILED;
    }

    return (int32_t)ulread;
#else
    int32_t iread = 0;
    if (ioctl(fd, FIONREAD, &iread) < ERR_OK)
    {
        return ERR_FAILED;
    }

    return iread;
#endif
};
/*
* \brief          获取sa_family
* \param fd       SOCKET
* \return         ERR_FAILED 失败
* \return         SOCK_STREAM  SOCK_DGRAM
*/
static inline int32_t socktype(SOCKET fd)
{
    int32_t itype = 0;
    int32_t ilen = (int32_t)sizeof(itype);
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, (char *)&itype, (socklen_t*)&ilen) < ERR_OK)
    {
        PRINTF("getsockopt(%d, ...) failed. %s", (int32_t)fd, ERRORSTR(ERRNO));
        return ERR_FAILED;
    }

    return itype;
};
/*
* \brief          获取sin_family
* \param fd       SOCKET
* \return         ERR_FAILED 失败
*/
static inline int32_t sockaddrfamily(SOCKET fd)
{
#if defined(OS_WIN)
    WSAPROTOCOL_INFO info;
    int32_t ilens = (int32_t)sizeof(info);
    if (getsockopt(fd, SOL_SOCKET, SO_PROTOCOL_INFO, (char *)&info, &ilens) < ERR_OK)
    {
        PRINTF("getsockopt(%d, SOL_SOCKET, SO_PROTOCOL_INFO, ...) failed. %s", (int32_t)fd, ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    return info.iAddressFamily;
#else
#ifdef SO_DOMAIN
    int32_t ifamily = 0;
    int32_t ilens = (int32_t)sizeof(ifamily);
    if (getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &ifamily, (socklen_t*)&ilens) < 0)
    {
        PRINTF("getsockopt(%d, SOL_SOCKET, SO_DOMAIN, ...) failed. %s", (int32_t)fd, ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    return ifamily;
#endif
    return AF_INET;
#endif
};
/*
* \brief          设置socket TCP_NODELAY
* \param fd       SOCKET
*/
void socknodelay(SOCKET fd);
/*
* \brief          设置socket 非阻塞
* \param fd       SOCKET
*/
void socknbio(SOCKET fd);
/*
* \brief          设置地址重用
* \param fd       SOCKET
*/
void sockraddr(SOCKET fd);
/*
* \brief          是否支持端口重用
* \return         ERR_OK支持
*/
int32_t checkrport();
/*
* \brief          设置端口重用
* \param fd       监听的SOCKET
*/
void sockrport(SOCKET lsfd);
/*
* \brief            设置socket SO_KEEPALIVE
* \param fd         SOCKET
* \param idelay     多长时间没有报文开始发送keepalive 秒   小于等于0不设置时间间隔，使用系统默认的
* \param iintvl     发送keepalive心跳时间间隔 秒
*/
void sockkpa(SOCKET fd, const int32_t idelay, const int32_t iintvl);
/*
* \brief          设置SO_LINGER
* \param fd       SOCKET
*/
void closereset(SOCKET fd);
/*
* \brief          一组相互链接的socket
* \param sock     SOCKET
* \return         ERR_OK 成功
*/
int32_t sockpair(SOCKET sock[2]);

#endif
