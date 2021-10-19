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
        PRINTF("ioctlsocket(%d, FIONREAD, nread) failed. %s", (int32_t)fd, ERRORSTR(ERRNO));
        return ERR_FAILED;
    }

    return (int32_t)ulread;
#else
    int32_t iread = 0;
    if (ioctl(fd, FIONREAD, &iread) < ERR_OK)
    {
        PRINTF("ioctl(%d, FIONREAD, nread) failed. %s", fd, ERRORSTR(ERRNO));
        return ERR_FAILED;
    }

    return iread;
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
* \brief          一组相互链接的socket
* \param sock     SOCKET
* \return         ERR_OK 成功
*/
int32_t sockpair(SOCKET sock[2]);

#endif
