#ifndef NETADDR_H_
#define NETADDR_H_

#include "base/macro.h"

typedef union netaddr_ctx {
    struct sockaddr addr;
    struct sockaddr_in ipv4;
    struct sockaddr_in6 ipv6;
}netaddr_ctx;

int32_t is_ipv4(const char *ip);
int32_t is_ipv6(const char *ip);
int32_t is_ipaddr(const char* ip);
void netaddr_empty(netaddr_ctx *ctx);
int32_t netaddr_set(netaddr_ctx *ctx, const char *ip, const uint16_t port);
int32_t netaddr_remote(netaddr_ctx *ctx, SOCKET fd);
int32_t netaddr_local(netaddr_ctx *ctx, SOCKET fd);
struct sockaddr *netaddr_addr(netaddr_ctx *ctx);
socklen_t netaddr_size(netaddr_ctx *ctx);
int32_t netaddr_ip(netaddr_ctx *ctx, char ip[IP_LENS]);
uint16_t netaddr_port(netaddr_ctx *ctx);
int32_t netaddr_family(netaddr_ctx *ctx);

#endif//NETADDR_H_
