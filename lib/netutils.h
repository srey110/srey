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
int32_t socknread(const SOCKET &fd);
/*
* \brief          socket讀取數據
* \param fd       socket句柄
* \return         ERR_FAILED 失败，需要關閉socket
* \return         长度
*/
//int32_t sockrecv(const SOCKET &fd, class cchainbuffer *pbuf);
/*
* \brief          创建一监听socket
* \param ip       ip
* \param port     port
* \param backlog  等待连接队列的最大长度 -1 使用128
* \return         INVALID_SOCK 失败
*/
SOCKET socklsn(const char *ip, const uint16_t &port, const int32_t &backlog);
/*
* \brief          创建一socket链接
* \param ip       ip
* \param port     port
* \return         INVALID_SOCK 失败
*/
SOCKET sockcnt(const char *ip, const uint16_t &port);
/*
* \brief          设置socket参数 TCP_NODELAY  SO_KEEPALIVE 非阻塞
* \param fd       SOCKET
*/
void sockopts(SOCKET &fd);
/*
* \brief          一组相互链接的socket
* \param sock     SOCKET
* \return         true 成功
*/
bool sockpair(SOCKET sock[2]);

SREY_NS_END

#endif
