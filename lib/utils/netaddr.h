#ifndef NETADDR_H_
#define NETADDR_H_

#include "base/macro.h"

typedef union netaddr_ctx {
    struct sockaddr addr;
    struct sockaddr_in ipv4;
    struct sockaddr_in6 ipv6;
}netaddr_ctx;
/// <summary>
/// 是否为ipv4 地址
/// </summary>
/// <param name="ip">IP</param>
/// <returns>ERR_OK ipv4 </returns>
int32_t is_ipv4(const char *ip);
/// <summary>
/// 是否为ipv6 地址
/// </summary>
/// <param name="ip">IP</param>
/// <returns>ERR_OK ipv6 </returns>
int32_t is_ipv6(const char *ip);
/// <summary>
/// 是否为ip地址
/// </summary>
/// <param name="ip">IP</param>
/// <returns>ERR_OK ip地址 </returns>
int32_t is_ipaddr(const char* ip);
/// <summary>
/// 清空netaddr_ctx
/// </summary>
/// <param name="ctx">netaddr_ctx</param>
void netaddr_empty(netaddr_ctx *ctx);
/// <summary>
/// 设置sockaddr
/// </summary>
/// <param name="ctx">netaddr_ctx</param>
/// <param name="ip">IP</param>
/// <param name="port">端口</param>
/// <returns>ERR_OK 成功</returns>
int32_t netaddr_set(netaddr_ctx *ctx, const char *ip, const uint16_t port);
/// <summary>
/// 获取远端sockaddr信息
/// </summary>
/// <param name="ctx">netaddr_ctx</param>
/// <param name="fd">socket句柄</param>
/// <returns>ERR_OK 成功</returns>
int32_t netaddr_remote(netaddr_ctx *ctx, SOCKET fd);
/// <summary>
/// 获取本地sockaddr信息
/// </summary>
/// <param name="ctx">netaddr_ctx</param>
/// <param name="fd">socket句柄</param>
/// <returns>ERR_OK 成功</returns>
int32_t netaddr_local(netaddr_ctx *ctx, SOCKET fd);
/// <summary>
/// 获取struct sockaddr *
/// </summary>
/// <param name="ctx">netaddr_ctx</param>
/// <returns>struct sockaddr *</returns>
struct sockaddr *netaddr_addr(netaddr_ctx *ctx);
/// <summary>
/// 获取sockaddr长度
/// </summary>
/// <param name="ctx">netaddr_ctx</param>
/// <returns>长度</returns>
socklen_t netaddr_size(netaddr_ctx *ctx);
/// <summary>
/// 获取ip
/// </summary>
/// <param name="ctx">netaddr_ctx</param>
/// <param name="ip">IP</param>
/// <returns>ERR_OK 成功</returns>
int32_t netaddr_ip(netaddr_ctx *ctx, char ip[IP_LENS]);
/// <summary>
/// 获取端口
/// </summary>
/// <param name="ctx">netaddr_ctx</param>
/// <returns>端口</returns>
uint16_t netaddr_port(netaddr_ctx *ctx);
/// <summary>
/// 获取AF_INET 或 AF_INET6
/// </summary>
/// <param name="ctx">netaddr_ctx</param>
/// <returns>AF_INET 或 AF_INET6</returns>
int32_t netaddr_family(netaddr_ctx *ctx);

#endif//NETADDR_H_
