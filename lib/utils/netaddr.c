#include "utils/netaddr.h"

int32_t is_ipv4(const char *ip) {
    struct in_addr addr;
    return inet_pton(AF_INET, ip, &addr) == 1 ? ERR_OK : ERR_FAILED;
}
int32_t is_ipv6(const char *ip) {
    struct in6_addr addr;
    return inet_pton(AF_INET6, ip, &addr) == 1 ? ERR_OK : ERR_FAILED;
}
int32_t is_ipaddr(const char* ip) {
    return (ERR_OK == is_ipv4(ip) || ERR_OK == is_ipv6(ip)) ? ERR_OK : ERR_FAILED;
}
int32_t is_loopback(const char *ip) {
    struct in_addr v4;
    if (1 == inet_pton(AF_INET, ip, &v4)) {
        return (127 == (ntohl(v4.s_addr) >> 24)) ? ERR_OK : ERR_FAILED;
    }
    struct in6_addr v6;
    if (1 != inet_pton(AF_INET6, ip, &v6)) {
        return ERR_FAILED;
    }
    // 逐字节比而不用 IN6_IS_ADDR_LOOPBACK：那个宏各平台头文件里的形态不一致
    static const uint8_t loop6[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
    if (0 == memcmp(v6.s6_addr, loop6, sizeof(loop6))) {
        return ERR_OK;
    }
    // ::ffff:127.x.x.x：双栈监听时回环也可能写成 v4-mapped
    static const uint8_t v4mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
    if (0 == memcmp(v6.s6_addr, v4mapped, sizeof(v4mapped))
        && 127 == v6.s6_addr[12]) {
        return ERR_OK;
    }
    return ERR_FAILED;
}
void netaddr_empty(netaddr_ctx *ctx) {
    ZERO(ctx, sizeof(netaddr_ctx));
}
int32_t netaddr_set(netaddr_ctx *ctx, const char *ip, const uint16_t port) {
    ZERO(ctx, sizeof(netaddr_ctx));
    if (1 == inet_pton(AF_INET, ip, &ctx->ipv4.sin_addr.s_addr)) {
        ctx->addr.sa_family = AF_INET;
        ctx->ipv4.sin_port = htons(port);
    } else if (1 == inet_pton(AF_INET6, ip, &ctx->ipv6.sin6_addr.s6_addr)) {
        ctx->addr.sa_family = AF_INET6;
        ctx->ipv6.sin6_port = htons(port);
    } else {
        return ERR_FAILED;
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
int32_t netaddr_compare(netaddr_ctx *a, netaddr_ctx *b) {
    if (a->addr.sa_family != b->addr.sa_family) {
        return ERR_FAILED;
    }
    if (AF_INET == a->addr.sa_family) {
        if (a->ipv4.sin_port != b->ipv4.sin_port
            || 0 != memcmp(&a->ipv4.sin_addr, &b->ipv4.sin_addr, sizeof(a->ipv4.sin_addr))) {
            return ERR_FAILED;
        }
    } else {
        if (a->ipv6.sin6_port != b->ipv6.sin6_port
            || 0 != memcmp(&a->ipv6.sin6_addr, &b->ipv6.sin6_addr, sizeof(a->ipv6.sin6_addr))) {
            return ERR_FAILED;
        }
    }
    return ERR_OK;
}
