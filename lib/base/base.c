#include "base/base.h"
#include "base/macro.h"

#define _MC ((1 << CHAR_BIT) - 1) //字节掩码（0xff），用于逐字节提取整数

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
    char *buf = (char *)ptr;
    val = tolower((unsigned char)val);
    while (maxlen--) {
        if (tolower((unsigned char)*buf) == val) {
            return (void *)buf;
        }
        buf++;
    }
    return NULL;
}
#ifndef OS_WIN
int32_t _memicmp(const void *ptr1, const void *ptr2, size_t lens) {
    size_t i = 0;
    char *buf1 = (char *)ptr1;
    char *buf2 = (char *)ptr2;
    while (i < lens
           && tolower((unsigned char)*buf1) == tolower((unsigned char)*buf2)) {
        buf1++;
        buf2++;
        i++;
    }
    if (i == lens) {
        return 0;
    } else {
        if (tolower((unsigned char)*buf1) > tolower((unsigned char)*buf2)) {
            return 1;
        } else {
            return -1;
        }
    }
}
#endif//OS_WIN
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
    char *pos;
    char *wt = (char *)what;
    char *cur = (char *)ptr;
    do {
        pos = chr(cur, wt[0], plens - (size_t)(cur - (char*)ptr));
        if (NULL == pos
            || plens - (size_t)(pos - (char*)ptr) < wlen) {
            return NULL;
        }
        if (0 == cmp(pos, what, wlen)) {
            return (void *)pos;
        }
        cur = pos + 1;
    } while (plens - (size_t)(cur - (char*)ptr) >= wlen);
    return NULL;
}
int32_t ct_memcmp(const void *a, const void *b, size_t len) {
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    /* volatile 防止编译器将循环优化为提前退出，确保始终遍历全部字节。*/
    volatile unsigned char diff = 0;
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
        // 先判后乘, 免得溢出之后再回头查; 顺带把 max 上界一并管了, 不必事后再比。
        // d > max 要单独挡: max 小于当前位(如 max=4 撞上 '9')时 max - d 会回绕成巨值,
        // 判定恒不成立, 超界值就被放过去了
        if (d > max
            || v > (max - d) / 10) {
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
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return ERR_FAILED;
}
// 按指定字节序将 src 的 size 字节复制到 dest，自动处理大小端转换
static inline void _copy_with_endian(char *dest, const char *src, size_t size, int32_t islittle) {
    ASSERTAB(size > 0, "pack_float/double size must be positive.");
    if (islittle == IS_LITTLE) {
        memcpy(dest, src, size);
    } else {
        dest += size - 1;
        while (0 != size--) {
            *(dest--) = *(src++);
        }
    }
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
    _copy_with_endian(buf, (const char *)&val, sizeof(val), islittle);
}
float unpack_float(const char *buf, int32_t islittle) {
    float rtn;
    _copy_with_endian((char *)&rtn, buf, sizeof(rtn), islittle);
    return rtn;
}
void pack_double(char *buf, double val, int32_t islittle) {
    _copy_with_endian(buf, (const char *)&val, sizeof(val), islittle);
}
double unpack_double(const char *buf, int32_t islittle) {
    double rtn;
    _copy_with_endian((char *)&rtn, buf, sizeof(rtn), islittle);
    return rtn;
}
#if !defined(OS_WIN) && !defined(OS_DARWIN) && !defined(OS_AIX)
uint64_t ntohll(uint64_t val) {
    if (!IS_LITTLE) {
        return val;
    }
    uint64_t rtn;
    pack_integer((char *)&rtn, val, (int32_t)sizeof(uint64_t), 0);
    return rtn;
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
