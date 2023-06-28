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
void netaddr_empty_addr(netaddr_ctx *ctx, const int32_t family) {
    ZERO(ctx, sizeof(netaddr_ctx));
    ctx->addr.sa_family = family;
}
int32_t netaddr_sethost(netaddr_ctx *ctx, const char *ip, const uint16_t port) {
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
int32_t netaddr_remoteaddr(netaddr_ctx *ctx, SOCKET fd, const int32_t family) {
    ZERO(ctx, sizeof(netaddr_ctx));
    ctx->addr.sa_family = family;
    if (AF_INET == family) {
        struct sockaddr_in addr = { 0 };
        socklen_t addrlen = (socklen_t)sizeof(struct sockaddr_in);
        if (ERR_OK != getpeername(fd, (struct sockaddr *)&addr, &addrlen)) {
            return ERR_FAILED;
        }
        memcpy(&ctx->ipv4, &addr, sizeof(ctx->ipv4));
    } else {
        struct sockaddr_in6 addr = { 0 };
        socklen_t addrlen = (socklen_t)sizeof(struct sockaddr_in6);
        if (ERR_OK != getpeername(fd, (struct sockaddr *)&addr, &addrlen)) {
            return ERR_FAILED;
        }
        memcpy(&ctx->ipv6, &addr, sizeof(ctx->ipv6));
    }
    return ERR_OK;
}
int32_t netaddr_localaddr(netaddr_ctx *ctx, SOCKET fd, const int32_t family) {
    ZERO(ctx, sizeof(netaddr_ctx));
    ctx->addr.sa_family = family;
    if (AF_INET == family) {
        struct sockaddr_in addr = { 0 };
        socklen_t addrlen = (socklen_t)sizeof(struct sockaddr_in);
        if (ERR_OK != getsockname(fd, (struct sockaddr *)&addr, &addrlen)) {
            return ERR_FAILED;
        }
        memcpy(&ctx->ipv4, &addr, sizeof(ctx->ipv4));
    } else {
        struct sockaddr_in6 addr = { 0 };
        socklen_t addrlen = (socklen_t)sizeof(struct sockaddr_in6);
        if (ERR_OK != getsockname(fd, (struct sockaddr *)&addr, &addrlen)) {
            return ERR_FAILED;
        }
        memcpy(&ctx->ipv6, &addr, sizeof(ctx->ipv6));
    }
    return ERR_OK;
}
struct sockaddr *netaddr_addr(netaddr_ctx *ctx) {
    return &ctx->addr;
}
socklen_t netaddr_size(netaddr_ctx *ctx) {
    return AF_INET == ctx->addr.sa_family ? (int32_t)sizeof(ctx->ipv4) : (int32_t)sizeof(ctx->ipv6);
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
