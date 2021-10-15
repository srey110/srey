#ifndef NETUTILS_H_
#define NETUTILS_H_

#include "macro.h"

SREY_NS_BEGIN

/*
* \brief          获取socket可读长度
* \param fd       socket句柄
* \return         ERR_FAILED 失败
* \return         长度
*/
inline int32_t socknread(const SOCKET &fd)
{
#ifdef OS_WIN
    u_long ulread = INIT_NUMBER;
    if (ioctlsocket(fd, FIONREAD, &ulread) < ERR_OK)
    {
        PRINTF("ioctlsocket(%d, FIONREAD, nread) failed. %s", (int32_t)fd, ERRORSTR(ERRNO));
        return ERR_FAILED;
    }

    return (int32_t)ulread;
#else
    int32_t iread = INIT_NUMBER;
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
void socknodelay(const SOCKET &fd);
/*
* \brief          设置socket 非阻塞
* \param fd       SOCKET
*/
void socknbio(const SOCKET &fd);
/*
* \brief          设置地址重用
* \param fd       SOCKET
*/
void sockraddr(const SOCKET &fd);
/*
* \brief          是否支持端口重用
* \return         true支持
*/
bool checkrport();
/*
* \brief          设置端口重用
* \param fd       监听的SOCKET
*/
void sockrport(const SOCKET &lsfd);
/*
* \brief            设置socket SO_KEEPALIVE
* \param fd         SOCKET
* \param idelay     多长时间没有报文开始发送keepalive 秒   小于等于0不设置时间间隔，使用系统默认的
* \param iintvl     发送keepalive心跳时间间隔 秒
*/
void sockkpa(const SOCKET &fd, const int32_t &idelay, const int32_t &iintvl);
/*
* \brief          一组相互链接的socket
* \param sock     SOCKET
* \return         true 成功
*/
bool sockpair(SOCKET sock[2]);

SREY_NS_END

#endif
