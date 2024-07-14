#ifndef NETUTILS_H_
#define NETUTILS_H_

#include "base/macro.h"

void sock_init(void);
void sock_clean(void);
/// <summary>
/// 获取socket可读长度
/// </summary>
/// <param name="fd">socket 句柄</param>
/// <returns>字节数,ERR_FAILED 失败</returns>
int32_t sock_nread(SOCKET fd);
/// <summary>
/// 获取socket错误码
/// </summary>
/// <param name="fd">socket 句柄</param>
/// <returns>错误码,ERR_FAILED 失败</returns>
int32_t sock_error(SOCKET fd);
/// <summary>
/// 检查socket是否链接成功
/// </summary>
/// <param name="fd">socket 句柄</param>
/// <returns>ERR_OK 成功</returns>
int32_t sock_checkconn(SOCKET fd);
/// <summary>
/// 获取socket类型
/// </summary>
/// <param name="fd">socket 句柄</param>
/// <returns>SOCK_STREAM  SOCK_DGRAM, ERR_FAILED 失败</returns>
int32_t sock_type(SOCKET fd);
/// <summary>
/// 获取socket地址信息
/// </summary>
/// <param name="fd">socket 句柄</param>
/// <returns>AF_INET AF_INET6, ERR_FAILED 失败</returns>
int32_t sock_family(SOCKET fd);
/// <summary>
/// 设置立即发送
/// </summary>
/// <param name="fd">socket 句柄</param>
/// <returns>ERR_OK 成功</returns>
int32_t sock_nodelay(SOCKET fd);
/// <summary>
/// 设置非阻塞
/// </summary>
/// <param name="fd">socket 句柄</param>
/// <returns>ERR_OK 成功</returns>
int32_t sock_nonblock(SOCKET fd);
/// <summary>
/// 设置地址重用
/// </summary>
/// <param name="fd">socket 句柄</param>
/// <returns>ERR_OK 成功</returns>
int32_t sock_reuseaddr(SOCKET fd);
/// <summary>
/// 设置端口重用
/// </summary>
/// <param name="fd">socket 句柄</param>
/// <returns>ERR_OK 成功</returns>
int32_t sock_reuseport(SOCKET fd);
/// <summary>
/// 设置KEEPALIVE
/// </summary>
/// <param name="fd">socket 句柄</param>
/// <param name="delay">多久后发送keepalive 秒</param>
/// <param name="intvl">重试间隔 秒</param>
/// <returns>ERR_OK 成功</returns>
int32_t sock_keepalive(SOCKET fd, const int32_t delay, const int32_t intvl);
/// <summary>
/// 设置SO_LINGER 避免TIME_WAIT状态
/// </summary>
/// <param name="fd">socket 句柄</param>
/// <returns>ERR_OK 成功</returns>
int32_t sock_linger(SOCKET fd);
/// <summary>
/// socket对
/// </summary>
/// <param name="sock">socket数组</param>
/// <returns>ERR_OK 成功</returns>
int32_t sock_pair(SOCKET sock[2]);

#endif
