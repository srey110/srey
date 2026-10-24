#include "utils/netaddr.h"

// 0~255 的十进制串，每项 4 字节，不足的补 '\0'；netaddr_ip4_str 整项拷、按位数前进
static const char _netaddr_dec[256][4] = {
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14", "15",
    "16", "17", "18", "19", "20", "21", "22", "23", "24", "25", "26", "27", "28", "29", "30", "31",
    "32", "33", "34", "35", "36", "37", "38", "39", "40", "41", "42", "43", "44", "45", "46", "47",
    "48", "49", "50", "51", "52", "53", "54", "55", "56", "57", "58", "59", "60", "61", "62", "63",
    "64", "65", "66", "67", "68", "69", "70", "71", "72", "73", "74", "75", "76", "77", "78", "79",
    "80", "81", "82", "83", "84", "85", "86", "87", "88", "89", "90", "91", "92", "93", "94", "95",
    "96", "97", "98", "99", "100", "101", "102", "103", "104", "105", "106", "107", "108", "109", "110", "111",
    "112", "113", "114", "115", "116", "117", "118", "119", "120", "121", "122", "123", "124", "125", "126", "127",
    "128", "129", "130", "131", "132", "133", "134", "135", "136", "137", "138", "139", "140", "141", "142", "143",
    "144", "145", "146", "147", "148", "149", "150", "151", "152", "153", "154", "155", "156", "157", "158", "159",
    "160", "161", "162", "163", "164", "165", "166", "167", "168", "169", "170", "171", "172", "173", "174", "175",
    "176", "177", "178", "179", "180", "181", "182", "183", "184", "185", "186", "187", "188", "189", "190", "191",
    "192", "193", "194", "195", "196", "197", "198", "199", "200", "201", "202", "203", "204", "205", "206", "207",
    "208", "209", "210", "211", "212", "213", "214", "215", "216", "217", "218", "219", "220", "221", "222", "223",
    "224", "225", "226", "227", "228", "229", "230", "231", "232", "233", "234", "235", "236", "237", "238", "239",
    "240", "241", "242", "243", "244", "245", "246", "247", "248", "249", "250", "251", "252", "253", "254", "255"
};

int32_t is_ipv4(const char *ip) {
    struct in_addr addr;
    return inet_pton(AF_INET, ip, &addr) == 1 ? ERR_OK : ERR_FAILED;
}
// 带 "%网卡号" 的 IPv6(链路本地常见)：inet_pton 只在部分平台认这个后缀，一律先剥掉再判，
// 让 is_ipv6 / is_ipaddr 两个平台一致。网卡号只当不透明后缀，不校验
int32_t is_ipv6(const char *ip) {
    char buf[IP_LENS];
    if (NULL == ip || '\0' == *ip) {
        return ERR_FAILED;
    }
    const char *scope = strchr(ip, '%');
    if (NULL != scope) {
        size_t lens = (size_t)(scope - ip);
        if (lens >= sizeof(buf)) {
            return ERR_FAILED;
        }
        memcpy(buf, ip, lens);
        buf[lens] = '\0';
        ip = buf;
    }
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
// 严格点分十进制(恰 4 段、每段 1~3 位且不带前导零、不超过 255、以 '\0' 结尾)才在这里解，其余一律
// 交给 inet_pton。接受的串是各家 inet_pton 都接受的子集，结果与它逐字节相同；前导零各家说法不一，不碰。
// 先解到局部再整体拷：解到一半失败时不能在 sin_addr 上留字节，那 4 字节在 IPv6 里是 sin6_flowinfo
static int32_t _netaddr_pton4(const char *s, uint8_t out[4]) {
    uint8_t b[4];
    uint32_t part, n;
    for (int32_t i = 0; i < 4; i++) {
        if ((uint8_t)(s[0] - '0') > 9) {
            return ERR_FAILED;
        }
        part = (uint32_t)(s[0] - '0');
        n = 1;
        while ((uint8_t)(s[n] - '0') <= 9) {
            if (0 == part
                || n >= 3) {
                return ERR_FAILED;
            }
            part = part * 10 + (uint32_t)(s[n] - '0');
            n++;
        }
        if (part > 255
            || s[n] != (3 == i ? '\0' : '.')) {
            return ERR_FAILED;
        }
        b[i] = (uint8_t)part;
        s += n + 1;
    }
    memcpy(out, b, sizeof(b));
    return ERR_OK;
}
// "IPv6%网卡"：后缀是数字就直接当网卡号，否则按网卡名查(Windows 只认数字)，写进 scope_id。
// NOINLINE：带栈上数组，内联进 netaddr_set 会让每次调用都过一次栈保护检查
NOINLINE static int32_t _netaddr_pton6_scope(netaddr_ctx *ctx, const char *ip, const char *pct) {
    if ('\0' == pct[1]) {
        return ERR_FAILED;
    }
    char addr[IP_LENS];
    size_t n = (size_t)(pct - ip);
    if (n >= sizeof(addr)) {
        return ERR_FAILED;
    }
    memcpy(addr, ip, n);
    addr[n] = '\0';
    struct in6_addr a6;
    if (1 != inet_pton(AF_INET6, addr, &a6)) {
        return ERR_FAILED;
    }
    const char *scope = pct + 1;
    uint64_t id;
    if (ERR_OK != strtou64(scope, strlen(scope), UINT32_MAX, &id)) {
#if defined(OS_WIN)
        return ERR_FAILED;
#else
        id = if_nametoindex(scope);
        if (0 == id) {
            return ERR_FAILED;
        }
#endif
    }
    ctx->ipv6.sin6_addr = a6;
    ctx->ipv6.sin6_scope_id = (uint32_t)id;
    return ERR_OK;
}
int32_t netaddr_set(netaddr_ctx *ctx, const char *ip, const uint16_t port) {
    ZERO(ctx, sizeof(netaddr_ctx));
    // 带 % 的不能交给 inet_pton：macOS 照收并丢掉后缀，网卡名还会被塞进地址字节
    const char *pct;
    if (ERR_OK == _netaddr_pton4(ip, (uint8_t *)&ctx->ipv4.sin_addr.s_addr)
        || 1 == inet_pton(AF_INET, ip, &ctx->ipv4.sin_addr.s_addr)) {
        ctx->addr.sa_family = AF_INET;
        ctx->ipv4.sin_port = htons(port);
    } else if (NULL == (pct = strchr(ip, '%'))
               ? 1 == inet_pton(AF_INET6, ip, &ctx->ipv6.sin6_addr.s6_addr)
               : ERR_OK == _netaddr_pton6_scope(ctx, ip, pct)) {
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
socklen_t netaddr_size(const netaddr_ctx *ctx) {
    return AF_INET == ctx->addr.sa_family ? (socklen_t)sizeof(ctx->ipv4) : (socklen_t)sizeof(ctx->ipv6);
}
void netaddr_ip4_str(const uint8_t *b, char ip[IP_LENS]) {
    char *p = ip;
    for (int32_t i = 0; i < 4; i++) {
        memcpy(p, _netaddr_dec[b[i]], sizeof(_netaddr_dec[0]));
        p += 1 + (b[i] >= 10) + (b[i] >= 100);
        *p++ = '.';
    }
    p[-1] = '\0';
}
int32_t netaddr_ip(const netaddr_ctx *ctx, char ip[IP_LENS]) {
    if (AF_INET == ctx->addr.sa_family) {
        netaddr_ip4_str((const uint8_t *)&ctx->ipv4.sin_addr, ip);
        return ERR_OK;
    }
    if (NULL == inet_ntop(AF_INET6, &ctx->ipv6.sin6_addr, ip, IP_LENS)) {
        ip[0] = '\0';
        return ERR_FAILED;
    }
    // 补 "%网卡号"：地址最长 45 字节，再加 '%' 与 10 位数仍在 IP_LENS 内
    if (0 != ctx->ipv6.sin6_scope_id) {
        char *p = ip + strlen(ip);
        *p = '%';
        u64tostr(p + 1, ctx->ipv6.sin6_scope_id, 10);
    }
    return ERR_OK;
}
uint16_t netaddr_port(const netaddr_ctx *ctx) {
    return AF_INET == ctx->addr.sa_family ? ntohs(ctx->ipv4.sin_port) : ntohs(ctx->ipv6.sin6_port);
}
int32_t netaddr_family(const netaddr_ctx *ctx) {
    return ctx->addr.sa_family;
}
int32_t netaddr_compare(const netaddr_ctx *a, const netaddr_ctx *b) {
    if (a->addr.sa_family != b->addr.sa_family) {
        return ERR_FAILED;
    }
    if (AF_INET == a->addr.sa_family) {
        if (a->ipv4.sin_port != b->ipv4.sin_port
            || 0 != memcmp(&a->ipv4.sin_addr, &b->ipv4.sin_addr, sizeof(a->ipv4.sin_addr))) {
            return ERR_FAILED;
        }
    } else {
        // scope_id 区分链路本地地址在哪块网卡上。只在两边都给了时才比：不带 "%网卡" 的地址 scope_id 为 0，
        // 拿它跟内核收包带回的网卡号比就永远对不上
        if (a->ipv6.sin6_port != b->ipv6.sin6_port
            || (0 != a->ipv6.sin6_scope_id
                && 0 != b->ipv6.sin6_scope_id
                && a->ipv6.sin6_scope_id != b->ipv6.sin6_scope_id)
            || 0 != memcmp(&a->ipv6.sin6_addr, &b->ipv6.sin6_addr, sizeof(a->ipv6.sin6_addr))) {
            return ERR_FAILED;
        }
    }
    return ERR_OK;
}
