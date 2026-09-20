#ifndef NETUTILS_H_
#define NETUTILS_H_

#include "base/macro.h"

/// <summary>
/// 初始化 socket 环境（Windows 下调用 WSAStartup）
/// </summary>
void sock_init(void);
/// <summary>
/// 清理 socket 环境（Windows 下调用 WSACleanup）
/// </summary>
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
/// 设置地址重用。istcp 非 0（TCP 监听）时 Windows 走 SO_EXCLUSIVEADDRUSE：
/// 该平台的 SO_REUSEADDR 允许他进程抢占正在使用的 addr:port（本地端口劫持），监听须显式独占；
/// 代价是关闭后不能立即重绑，重启窗口内返回 WSAEADDRINUSE。
/// istcp 为 0（UDP）时各平台一律 SO_REUSEADDR：多播要求多 socket 绑定同一 addr:port
/// </summary>
/// <param name="fd">socket 句柄</param>
/// <param name="istcp">非 0 TCP 监听，0 UDP</param>
/// <returns>ERR_OK 成功</returns>
int32_t sock_reuseaddr(SOCKET fd, int32_t istcp);
/// <summary>
/// 设置端口重用。FreeBSD 优先用 SO_REUSEPORT_LB(它的 SO_REUSEPORT 不分发 TCP 连接),
/// 两者互斥:同一 addr:port 上一半 fd 用 _LB 另一半用 SO_REUSEPORT 会 EADDRINUSE
/// </summary>
/// <param name="fd">socket 句柄</param>
/// <returns>ERR_OK 成功</returns>
int32_t sock_reuseport(SOCKET fd);
/// <summary>
/// AF_INET6 socket 置为只收 IPv6(IPV6_V6ONLY=1)。**须在 bind 前调用**。
/// 不设就随平台默认走:Linux / macOS 收 v4-mapped 的 IPv4,Windows / FreeBSD 只收 IPv6,
/// 同为 Linux 还随 net.ipv6.bindv6only 翻转。要双栈就 "0.0.0.0" 与 "::" 各绑一次
/// </summary>
/// <param name="fd">socket 句柄</param>
/// <param name="family">socket 地址族,取自 netaddr_family</param>
/// <returns>ERR_OK 成功（非 AF_INET6 的 socket 不设置、直接返 ERR_OK）</returns>
int32_t sock_v6only(SOCKET fd, int32_t family);
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
/// 创建互联的 socket 对（环回 TCP，两端已带 CLOEXEC）
/// </summary>
/// <param name="sock">socket数组</param>
/// <param name="nonblock">非 0 则两端设为非阻塞，0 保持阻塞</param>
/// <returns>ERR_OK 成功</returns>
int32_t sock_pair(SOCKET sock[2], int32_t nonblock);
/// <summary>
/// 创建 socket：Unix 带 CLOEXEC，Windows 建 overlapped 且禁句柄继承，避免被子进程(fork+exec / CreateProcess)继承
/// </summary>
/// <param name="family">地址族</param>
/// <param name="type">socket 类型</param>
/// <param name="proto">协议，0 表示按 family/type 选默认</param>
/// <param name="nonblock">非 0 则一并设为非阻塞，设不上按创建失败处理</param>
/// <returns>socket 句柄，失败返回 INVALID_SOCK</returns>
SOCKET sock_create_cloexec(int32_t family, int32_t type, int32_t proto, int32_t nonblock);
/// <summary>
/// accept 并带 CLOEXEC，避免被 fork+exec 的子进程继承
/// </summary>
/// <param name="fd">监听 socket</param>
/// <param name="addr">对端地址，可为 NULL</param>
/// <param name="addrlen">地址长度，可为 NULL</param>
/// <param name="nonblock">非 0 则连同非阻塞一并设好，调用方无须再 sock_nonblock；设不上按 accept 失败处理</param>
/// <returns>新连接 socket，失败返回 INVALID_SOCK（此时错误码可分类，含非阻塞设置失败那种）</returns>
SOCKET sock_accept_cloexec(SOCKET fd, struct sockaddr *addr, socklen_t *addrlen, int32_t nonblock);

#endif
