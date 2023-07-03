#include "netaddr.h"

int32_t is_ipv4(const char *ip) {
    struct sockaddr_in sin;
    return inet_pton(AF_INET, ip, &sin) == 1 ? ERR_OK : ERR_FAILED;
}
int32_t is_ipv6(const char *ip) {
    struct sockaddr_in6 sin6;
    return inet_pton(AF_INET6, ip, &sin6) == 1 ? ERR_OK : ERR_FAILED;
}
int32_t is_ipaddr(const char* ip) {
    return (ERR_OK == is_ipv4(ip) || ERR_OK == is_ipv6(ip)) ? ERR_OK : ERR_FAILED;
}
void netaddr_empty(netaddr_ctx *ctx) {
    ZERO(ctx, sizeof(netaddr_ctx));
}
int32_t netaddr_set(netaddr_ctx *ctx, const char *ip, const uint16_t port) {
    ZERO(ctx, sizeof(netaddr_ctx));
    if (ERR_OK == is_ipv4(ip)) {
        ctx->addr.sa_family = AF_INET;
        if (1 != inet_pton(AF_INET, ip, &ctx->ipv4.sin_addr.s_addr)) {
            return ERR_FAILED;
        }
        ctx->ipv4.sin_port = htons(port);
    } else {
        ctx->addr.sa_family = AF_INET6;
        if (1 != inet_pton(AF_INET6, ip, &ctx->ipv6.sin6_addr.s6_addr)) {
            return ERR_FAILED;
        }
        ctx->ipv6.sin6_port = htons(port);
    }
    return ERR_OK;
}
int32_t netaddr_remote(netaddr_ctx *ctx, SOCKET fd) {
    ZERO(ctx, sizeof(netaddr_ctx));
    socklen_t addrlen = (socklen_t)sizeof(netaddr_ctx);
    if (ERR_OK != getpeername(fd, &ctx->addr, &addrlen)) {
        return ERR_FAILED;
    }
    return ERR_OK;
}
int32_t netaddr_local(netaddr_ctx *ctx, SOCKET fd) {
    ZERO(ctx, sizeof(netaddr_ctx));
    socklen_t addrlen = (socklen_t)sizeof(netaddr_ctx);
    if (ERR_OK != getsockname(fd, &ctx->addr, &addrlen)) {
        return ERR_FAILED;
    }
    return ERR_OK;
}
struct sockaddr *netaddr_addr(netaddr_ctx *ctx) {
    return &ctx->addr;
}
socklen_t netaddr_size(netaddr_ctx *ctx) {
    return AF_INET == ctx->addr.sa_family ? (socklen_t)sizeof(ctx->ipv4) : (socklen_t)sizeof(ctx->ipv6);
}
int32_t netaddr_ip(netaddr_ctx *ctx, char ip[IP_LENS]) {
    ZERO(ip, IP_LENS);
    if (AF_INET == ctx->addr.sa_family) {
        if (NULL == inet_ntop(AF_INET, &ctx->ipv4.sin_addr, ip, IP_LENS)) {
            return ERR_FAILED;
        }
    } else {
        if (NULL == inet_ntop(AF_INET6, &ctx->ipv6.sin6_addr, ip, IP_LENS)) {
            return ERR_FAILED;
        }
    }
    return ERR_OK;
}
uint16_t netaddr_port(netaddr_ctx *ctx) {
    return AF_INET == ctx->addr.sa_family ? ntohs(ctx->ipv4.sin_port) : ntohs(ctx->ipv6.sin6_port);
}
int32_t netaddr_family(netaddr_ctx *ctx) {
    return ctx->addr.sa_family;
}
