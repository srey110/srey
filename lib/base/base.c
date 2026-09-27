#include "base/base.h"
#include "base/macro.h"

#define _MC ((1 << CHAR_BIT) - 1) //字节掩码（0xff），用于逐字节提取整数
// 生成 _ascii_lower 表用:逐项算出一个字节的 ASCII 小写
#define _LOWER1(c)  (unsigned char)(((c) >= 'A' && (c) <= 'Z') ? (c) + ('a' - 'A') : (c))
#define _LOWER4(n)  _LOWER1(n), _LOWER1((n) + 1), _LOWER1((n) + 2), _LOWER1((n) + 3)
#define _LOWER16(n) _LOWER4(n), _LOWER4((n) + 4), _LOWER4((n) + 8), _LOWER4((n) + 12)
// 生成 _hex_val 表用：逐项算出一个字节的十六进制值，非法为 -1
#define _HEX1(c)  (int8_t)(((c) >= '0' && (c) <= '9') ? (c) - '0' : ((c) >= 'a' && (c) <= 'f') ? (c) - 'a' + 10 \
                            : ((c) >= 'A' && (c) <= 'F') ? (c) - 'A' + 10 : -1)
#define _HEX4(n)  _HEX1(n), _HEX1((n) + 1), _HEX1((n) + 2), _HEX1((n) + 3)
#define _HEX16(n) _HEX4(n), _HEX4((n) + 4), _HEX4((n) + 8), _HEX4((n) + 12)

static const char hex_char_upper[16] = {
    '0', '1', '2', '3',
    '4', '5', '6', '7',
    '8', '9', 'A', 'B',
    'C', 'D', 'E', 'F'
};
static const char hex_char_lower[16] = {
    '0', '1', '2', '3',
    '4', '5', '6', '7',
    '8', '9', 'a', 'b',
    'c', 'd', 'e', 'f'
};
// ASCII 小写映射：只折 'A'..'Z'，其余字节原样。本文件的大小写不敏感查找/比较共用，不随 locale 变
static const unsigned char _ascii_lower[256] = {
    _LOWER16(0), _LOWER16(16), _LOWER16(32), _LOWER16(48),
    _LOWER16(64), _LOWER16(80), _LOWER16(96), _LOWER16(112),
    _LOWER16(128), _LOWER16(144), _LOWER16(160), _LOWER16(176),
    _LOWER16(192), _LOWER16(208), _LOWER16(224), _LOWER16(240)
};
// 十六进制字符的值，非法字符为 -1（= ERR_FAILED）
static const int8_t _hex_val[256] = {
    _HEX16(0), _HEX16(16), _HEX16(32), _HEX16(48),
    _HEX16(64), _HEX16(80), _HEX16(96), _HEX16(112),
    _HEX16(128), _HEX16(144), _HEX16(160), _HEX16(176),
    _HEX16(192), _HEX16(208), _HEX16(224), _HEX16(240)
};

// barrier 是承重的:去掉它下面那句 memset 会被 -O2 -flto 的 DSE 整段删除。
// MSVC 没有这道 barrier,那边靠 volatile 写:先逐字节写到 8 字节对齐,中间按 8 字节写,余下逐字节
void secure_zero(void *buf, size_t len) {
    if (EMPTYPTR(buf, len)) {
        return;
    }
#if defined(__GNUC__) || defined(__clang__)
    memset(buf, 0, len);
    __asm__ __volatile__("" : : "r"(buf) : "memory");
#else
    volatile unsigned char *p = (volatile unsigned char *)buf;
    volatile uint64_t *w;
    while (0 != ((uintptr_t)p & 7) && len > 0) {
        *p++ = 0;
        len--;
    }
    w = (volatile uint64_t *)p;
    for (; len >= 8; len -= 8) {
        *w++ = 0;
    }
    p = (volatile unsigned char *)w;
    while (len--) {
        *p++ = 0;
    }
#endif
}
void *memichr(const void *ptr, int32_t val, size_t maxlen) {
    const unsigned char *buf = (const unsigned char *)ptr;
    val = _ascii_lower[(unsigned char)val];
    if (val < 'a'
        || val > 'z') {
        return memchr(ptr, val, maxlen);
    }
    while (maxlen--) {
        if (_ascii_lower[*buf] == val) {
            return (void *)buf;
        }
        buf++;
    }
    return NULL;
}
int32_t memcasecmp(const void *ptr1, const void *ptr2, size_t lens) {
    const unsigned char *buf1 = (const unsigned char *)ptr1;
    const unsigned char *buf2 = (const unsigned char *)ptr2;
    int32_t c1, c2;
    size_t i;
    for (i = 0; i < lens; i++) {
        c1 = _ascii_lower[buf1[i]];
        c2 = _ascii_lower[buf2[i]];
        if (c1 != c2) {
            return c1 > c2 ? 1 : -1;
        }
    }
    return 0;
}
int32_t strcasecmp_s(const char *s1, const char *s2) {
    const unsigned char *p1 = (const unsigned char *)s1;
    const unsigned char *p2 = (const unsigned char *)s2;
    int32_t c1, c2;
    do {
        c1 = _ascii_lower[*p1++];
        c2 = _ascii_lower[*p2++];
    } while (c1 == c2 && 0 != c1);
    return c1 == c2 ? 0 : (c1 > c2 ? 1 : -1);
}
int32_t strncasecmp_s(const char *s1, const char *s2, size_t n) {
    const unsigned char *p1 = (const unsigned char *)s1;
    const unsigned char *p2 = (const unsigned char *)s2;
    int32_t c1, c2;
    size_t i;
    for (i = 0; i < n; i++) {
        c1 = _ascii_lower[p1[i]];
        c2 = _ascii_lower[p2[i]];
        if (c1 != c2) {
            return c1 > c2 ? 1 : -1;
        }
        if (0 == c1) {
            break;
        }
    }
    return 0;
}
void *memstr(int32_t ncs, const void *ptr, size_t plens, const void *what, size_t wlen) {
    if (NULL == ptr
        || NULL == what
        || 0 == plens
        || 0 == wlen
        || wlen > plens) {
        return NULL;
    }
    chr_func chr;
    cmp_func cmp;
    mem_funcs_pick(ncs, &chr, &cmp);
    const char *wt = (const char *)what;
    char *cur = (char *)ptr;
    char *last = cur + (plens - wlen);
    while (cur <= last) {
        cur = (char *)chr(cur, wt[0], (size_t)(last - cur) + 1);
        if (NULL == cur) {
            return NULL;
        }
        if (1 == wlen
            || 0 == cmp(cur + 1, wt + 1, wlen - 1)) {
            return (void *)cur;
        }
        cur++;
    }
    return NULL;
}
int32_t ct_memcmp(const void *a, const void *b, size_t len) {
    /* 输入按 volatile 读：每个字节都得真读一遍，编译器没法提前退出 */
    const volatile unsigned char *pa = (const volatile unsigned char *)a;
    const volatile unsigned char *pb = (const volatile unsigned char *)b;
    unsigned char diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= pa[i] ^ pb[i];
    }
    return (int32_t)diff;
}
char *trim_left(char *data, size_t dlens, size_t *lens) {
    size_t off = 0;
    while (off < dlens && is_ows(data[off])) {
        off++;
    }
    if (off == dlens) {
        SET_PTR(lens, 0);
        return NULL;
    }
    SET_PTR(lens, dlens - off);
    return data + off;
}
char *trim_right(char *data, size_t dlens, size_t *lens) {
    size_t n = dlens;
    while (n > 0 && is_ows(data[n - 1])) {
        n--;
    }
    if (0 == n) {
        SET_PTR(lens, 0);
        return NULL;
    }
    SET_PTR(lens, n);
    return data;
}
char *trim(char *data, size_t dlens, size_t *lens) {
    size_t n = 0;
    char *cur = trim_left(data, dlens, &n);
    if (NULL == cur) {
        SET_PTR(lens, 0);
        return NULL;
    }
    return trim_right(cur, n, lens);
}
char *dup_zero(const void *src, size_t lens) {
    char *dst;
    MALLOC(dst, lens + 1);
    if (0 != lens) {
        memcpy(dst, src, lens);
    }
    dst[lens] = '\0';
    return dst;
}
int32_t str2u64(const char *str, size_t lens, uint64_t max, uint64_t *out) {
    const uint64_t cut = max / 10;
    const uint64_t lim = max % 10;
    uint64_t v = 0;
    uint64_t d;
    size_t i;
    if (0 == lens
        || NULL == str) {
        return ERR_FAILED;
    }
    for (i = 0; i < lens; i++) {
        if (str[i] < '0'
            || str[i] > '9') {
            return ERR_FAILED;
        }
        d = (uint64_t)(str[i] - '0');
        // 先判后乘, 免得溢出之后再回头查; 顺带把 max 上界一并管了, 不必事后再比
        if (v > cut
            || (v == cut && d > lim)) {
            return ERR_FAILED;
        }
        v = v * 10 + d;
    }
    *out = v;
    return ERR_OK;
}
char *strupper(char *str) {
    if (NULL == str) {
        return NULL;
    }
    char* p = str;
    while (*p != '\0') {
        if (*p >= 'a'
            && *p <= 'z') {
            *p &= ~0x20;
        }
        ++p;
    }
    return str;
}
char *strlower(char *str) {
    if (NULL == str) {
        return NULL;
    }
    char *p = str;
    while (*p != '\0') {
        if (*p >= 'A' && *p <= 'Z') {
            BIT_SET(*p, 0x20);
        }
        ++p;
    }
    return str;
}
char* strreverse(char* str) {
    if (NULL == str) {
        return NULL;
    }
    char* b = str;
    char* e = str;
    while (*e) {
        ++e;
    }
    --e;
    char tmp;
    while (e > b) {
        tmp = *e;
        *e = *b;
        *b = tmp;
        --e;
        ++b;
    }
    return str;
}
const char *_filename(const char *file) {
    const char *sep = strrchr(file, PATH_SEPARATOR);
    return NULL != sep ? sep + 1 : file;
}
char *tohex(const void *buf, size_t len, char *out, int32_t lower) {
    const char *tbl = lower ? hex_char_lower : hex_char_upper;
    size_t j = 0;
    unsigned char *p = (unsigned char *)buf;
    for (size_t i = 0; i < len; ++i) {
        out[j] = tbl[(p[i] / 16)];
        ++j;
        out[j] = tbl[(p[i] % 16)];
        ++j;
    }
    out[j] = '\0';
    return out;
}
int32_t fromhex(char c) {
    return _hex_val[(unsigned char)c];
}
// 32 / 64 位整数换字节序
static inline uint32_t _swap32(uint32_t v) {
#if defined(OS_WIN)
    return _byteswap_ulong(v);
#else
    return ((v & 0xffu) << 24) | ((v & 0xff00u) << 8) | ((v >> 8) & 0xff00u) | (v >> 24);
#endif
}
static inline uint64_t _swap64(uint64_t v) {
#if defined(OS_WIN)
    return _byteswap_uint64(v);
#else
    return ((uint64_t)_swap32((uint32_t)v) << 32) | _swap32((uint32_t)(v >> 32));
#endif
}
void pack_integer(char *buf, uint64_t val, int32_t size, int32_t islittle) {
    ASSERTAB(size > 0, "pack_integer size must be positive.");
    buf[islittle ? 0 : size - 1] = (int8_t)(val & _MC);
    for (int32_t i = 1; i < size; i++) {
        val >>= CHAR_BIT;
        buf[islittle ? i : size - 1 - i] = (int8_t)(val & _MC);
    }
}
int64_t unpack_integer(const char *buf, int32_t size, int32_t islittle, int32_t issigned) {
    if (size <= 0) {
        return 0;
    }
    uint64_t rtn = 0;
    int32_t limit = (size <= (int32_t)sizeof(uint64_t)) ? size : (int32_t)sizeof(uint64_t);
    for (int32_t i = limit - 1; i >= 0; i--) {
        rtn <<= CHAR_BIT;
        rtn |= (uint64_t)(uint8_t)buf[islittle ? i : size - 1 - i];
    }
    if (size < (int32_t)sizeof(uint64_t)) {
        if (issigned) {
            uint64_t mask = 1llu << (size * CHAR_BIT - 1);
            rtn = ((rtn ^ mask) - mask);
        }
    }
    return (int64_t)rtn;
}
void pack_float(char *buf, float val, int32_t islittle) {
    uint32_t u;
    memcpy(&u, &val, sizeof(u));
    if (islittle != IS_LITTLE) {
        u = _swap32(u);
    }
    memcpy(buf, &u, sizeof(u));
}
float unpack_float(const char *buf, int32_t islittle) {
    uint32_t u;
    float rtn;
    memcpy(&u, buf, sizeof(u));
    if (islittle != IS_LITTLE) {
        u = _swap32(u);
    }
    memcpy(&rtn, &u, sizeof(rtn));
    return rtn;
}
void pack_double(char *buf, double val, int32_t islittle) {
    uint64_t u;
    memcpy(&u, &val, sizeof(u));
    if (islittle != IS_LITTLE) {
        u = _swap64(u);
    }
    memcpy(buf, &u, sizeof(u));
}
double unpack_double(const char *buf, int32_t islittle) {
    uint64_t u;
    double rtn;
    memcpy(&u, buf, sizeof(u));
    if (islittle != IS_LITTLE) {
        u = _swap64(u);
    }
    memcpy(&rtn, &u, sizeof(rtn));
    return rtn;
}
#if !defined(OS_WIN) && !defined(OS_DARWIN) && !defined(OS_AIX)
uint64_t ntohll(uint64_t val) {
    return IS_LITTLE ? _swap64(val) : val;
}
uint64_t htonll(uint64_t val) {
    return ntohll(val);
}
#endif//OS_WIN OS_DARWIN OS_AIX
uint32_t pow2_ceil(uint32_t n) {
    if (0 == n || 0 == (n & (n - 1))) {
        return n;
    }
    ASSERTAB(n <= 0x80000000u, "pow2_ceil overflow.");
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    return n + 1;
}
void fill_timespec(struct timespec *timeout, uint32_t ms) {
    timeout->tv_sec = ms / 1000;
    timeout->tv_nsec = (long)(ms % 1000) * (1000 * 1000);
}
#if defined(OS_WIN)
const char *_fmterror(DWORD error) {
    char *err = NULL;
    if (0 == FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                            NULL,
                            error,
                            MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
                            (LPTSTR)&err,
                            0,
                            NULL)) {
        return "FormatMessageA error.";
    }
    static THREAD_LOCAL char errstr[4096]; // 线程局部存储，避免多线程竞争
    size_t ilens = strlen(err);
    ilens = ilens >= sizeof(errstr) ? sizeof(errstr) - 1 : ilens;
    memcpy(errstr, err, ilens);
    // FormatMessageA 的文本自带结尾 CRLF，留着每条错误日志后面都多一个空行
    while (ilens > 0 && ('\r' == errstr[ilens - 1] || '\n' == errstr[ilens - 1])) {
        ilens--;
    }
    errstr[ilens] = '\0';
    LocalFree(err);
    return errstr;
}
#endif
