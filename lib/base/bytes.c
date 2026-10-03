#include "base/bytes.h"
#include "base/macro_util.h"
#include "base/memory.h"

#define _FMT_STACK_SIZE 512 // _format_va 先在栈上试格式化的缓冲字节数，放不下才上堆
// 生成 _ascii_lower 表用:逐项算出一个字节的 ASCII 小写
#define _LOWER1(c)  (unsigned char)(((c) >= 'A' && (c) <= 'Z') ? (c) + ('a' - 'A') : (c))
#define _LOWER4(n)  _LOWER1(n), _LOWER1((n) + 1), _LOWER1((n) + 2), _LOWER1((n) + 3)
#define _LOWER16(n) _LOWER4(n), _LOWER4((n) + 4), _LOWER4((n) + 8), _LOWER4((n) + 12)
// ASCII 小写映射：只折 'A'..'Z'，其余字节原样。本文件的大小写不敏感查找/比较共用，不随 locale 变
static const unsigned char _ascii_lower[256] = {
    _LOWER16(0), _LOWER16(16), _LOWER16(32), _LOWER16(48),
    _LOWER16(64), _LOWER16(80), _LOWER16(96), _LOWER16(112),
    _LOWER16(128), _LOWER16(144), _LOWER16(160), _LOWER16(176),
    _LOWER16(192), _LOWER16(208), _LOWER16(224), _LOWER16(240)
};
// tchar 集合见 RFC 7230 §3.2.6：ALPHA / DIGIT / "!#$%&'*+-.^_`|~" 为 1，其余一概为 0。
// 按 16 列排，行首注释是高 4 位
static const uint8_t TCHAR_TBL[256] = {
    /* 0x0 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    /* 0x1 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    /* 0x2 */ 0,1,0,1,1,1,1,1,0,0,1,1,0,1,1,0,
    /* 0x3 */ 1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,
    /* 0x4 */ 0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    /* 0x5 */ 1,1,1,1,1,1,1,1,1,1,1,0,0,0,1,1,
    /* 0x6 */ 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    /* 0x7 */ 1,1,1,1,1,1,1,1,1,1,1,0,1,0,1,0,
    /* 0x8 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    /* 0x9 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    /* 0xA */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    /* 0xB */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    /* 0xC */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    /* 0xD */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    /* 0xE */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    /* 0xF */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

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
size_t _memcspn(const void *p, size_t lens, const char *what, size_t wlens) {
    const unsigned char *start = (const unsigned char *)p;
    const unsigned char *s = start;
    const unsigned char *end = start + lens;
    uint8_t map[32] = { 0 };
    size_t k;
    // 第 1 步：建一张 256 位(32 字节)的表，每个字节值占一位：c 在 what 里就把第 c 位置 1，即 map[c >> 3] 的第 c & 7 位。
    // 例：':' 是 0x3a(58)，落在 map[7] 的第 2 位。what 里有重复字符就重复置位，不影响结果；'\0' 也照样占第 0 位
    for (k = 0; k < wlens; k++) {
        map[(unsigned char)what[k] >> 3] |= (uint8_t)(1u << ((unsigned char)what[k] & 7));
    }
    // 第 2 步：逐字节查表，每个字节只看表里对应的那一位，与 what 有多长无关。第一个查到 1 的就是答案；
    // 一个都没有时 s 走到 end，返回的正好是 lens
    for (; s < end; s++) {
        if (0 != (map[*s >> 3] & (1u << (*s & 7)))) {
            break;
        }
    }
    return (size_t)(s - start);
}
#if defined(SIMD_SSE2) || defined(SIMD_NEON)
// 8 个字节里哪些落在 what 里：命中字节的最高位置 1。只有最低那个 1 一定准(减法借位只会往更高的字节误报)，
// 调用方只取第一个命中，够用。只在小端平台用(向量门控已排除大端)，第 0 个字节在最低位
static inline uint64_t _memcspn_swar(uint64_t x, const char *what, size_t wlens) {
    const uint64_t ones = 0x0101010101010101ull;
    uint64_t y, m = 0;
    size_t k;
    for (k = 0; k < wlens; k++) {
        // 第 1 步：what[k] 乘 0x0101..01 得到 8 个字节全是它的整数，与 x 异或：和 what[k] 相同的字节变成 0，其余不为 0。
        // 例：x 的 8 个字节为 "ab\rcdefg"、what[k] 为 '\r'(0x0d)，异或后第 2 个字节为 0
        y = x ^ (ones * (unsigned char)what[k]);
        // 第 2 步：找 y 里为 0 的字节。每个字节减 1(y - 0x0101..01)，原来是 0 的借位成 0xFF、最高位变 1；再与 ~y 按位与，
        // 去掉本来最高位就是 1 的字节(0x80~0xFF 减 1 后最高位仍是 1)。各个字符的结果按位或合在一起
        m |= (y - ones) & ~y;
    }
    // 第 3 步：每个字节只留最高位(0x8080..80)。借位会让紧挨在命中上面的字节误报，但只往高处传，最低那个 1 一定是真命中
    return m & (ones << 7);
}
// 按长度分三档，每档首尾各读一次(可重叠、不越界)，比完不逐字节循环
size_t _memcspn_short(const void *p, size_t lens, const char *what, size_t wlens) {
    const unsigned char *s = (const unsigned char *)p;
    uint64_t x0, x1, mk;
    uint32_t h0, h1;
    size_t k;
    if (lens >= 8) {
        // 第 1 档(8~15 字节)：前 8 与后 8 字节各读成一个 64 位整数(memcpy 读不要求对齐，编译成一条读指令)，两块可能重叠。
        // 先比前一块，命中就是最低那个 1 在第几个字节(ctz / 8)；没命中再比后一块，它的第 j 个字节是下标 lens-8+j。
        // 例：lens 为 12，前块是下标 0~7、后块是下标 4~11；前块没命中说明 0~7 都不是，后块的最低命中必落在 8~11
        memcpy(&x0, s, 8);
        memcpy(&x1, s + lens - 8, 8);
        mk = _memcspn_swar(x0, what, wlens);
        if (0 != mk) {
            return ctz64(mk) >> 3;
        }
        mk = _memcspn_swar(x1, what, wlens);
        return 0 != mk ? lens - 8 + (ctz64(mk) >> 3) : lens;
    }
    if (lens >= 4) {
        // 第 2 档(4~7 字节)：前 4 与后 4 字节各读一次，拼成一个 64 位整数一次比完：低 4 个字节是下标 0~3，高 4 个是下标 lens-4 起，
        // 所以第 k 个字节(k >= 4)对应下标 lens-4+(k-4)，即 lens-8+k。低半没命中就不会借位进高半，高半的最低命中同样准
        memcpy(&h0, s, 4);
        memcpy(&h1, s + lens - 4, 4);
        mk = _memcspn_swar((uint64_t)h0 | ((uint64_t)h1 << 32), what, wlens);
        if (0 == mk) {
            return lens;
        }
        k = ctz64(mk) >> 3;
        return k < 4 ? k : lens - 8 + k;
    }
    if (0 == lens) {
        return 0;
    }
    // 第 3 档(1~3 字节)：取下标 0、lens/2、lens-1 三个字节拼进低 3 个字节，三者合起来正好覆盖全部下标且按下标递增
    // (lens 为 1 时三个都是下标 0，为 2 时是 0、1、1)。高 5 个字节补的是 0，会撞上 what 里的 '\0'，所以只留低 3 个字节的结果
    x0 = (uint64_t)s[0] | ((uint64_t)s[lens >> 1] << 8) | ((uint64_t)s[lens - 1] << 16);
    mk = _memcspn_swar(x0, what, wlens) & 0x808080ull;
    if (0 == mk) {
        return lens;
    }
    k = ctz64(mk) >> 3;
    return 0 == k ? 0 : (1 == k ? lens >> 1 : lens - 1);// 第 0、1、2 个字节依次对应下标 0、lens/2、lens-1
}
#endif
void *_memstr(const void *ptr, size_t plens, const void *what, size_t wlen) {
    const char *w = (const char *)what;
    const char *cur = (const char *)ptr;
    const char *last;
    if (0 == wlen
        || wlen > plens) {
        return NULL;
    }
    last = cur + (plens - wlen);
    while (cur <= last) {
        cur = (const char *)memchr(cur, w[0], (size_t)(last - cur) + 1);
        if (NULL == cur) {
            return NULL;
        }
        // 先比末字节挡掉大多数候选；wlen 不超过 2 时首末两字节已经比完
        if (w[wlen - 1] == cur[wlen - 1]
            && (wlen <= 2 || 0 == memcmp(cur + 1, w + 1, wlen - 2))) {
            return (void *)cur;
        }
        cur++;
    }
    return NULL;
}
void *_memistr(const void *ptr, size_t plens, const void *what, size_t wlen) {
    const char *w = (const char *)what;
    const char *cur = (const char *)ptr;
    const char *last;
    if (0 == wlen
        || wlen > plens) {
        return NULL;
    }
    last = cur + (plens - wlen);
    while (cur <= last) {
        cur = (const char *)memichr(cur, w[0], (size_t)(last - cur) + 1);
        if (NULL == cur) {
            return NULL;
        }
        // 同 _memstr：先比(折叠后的)末字节挡掉大多数候选，wlen 不超过 2 时首末两字节已经比完
        if (_ascii_lower[(unsigned char)w[wlen - 1]] == _ascii_lower[(unsigned char)cur[wlen - 1]]
            && (wlen <= 2 || 0 == memcasecmp(cur + 1, w + 1, wlen - 2))) {
            return (void *)cur;
        }
        cur++;
    }
    return NULL;
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
int32_t is_token(const char *data, size_t lens) {
    unsigned char c;
    size_t i;
    if (0 == lens
        || NULL == data) {
        return 0;
    }
    for (i = 0; i < lens; i++) {
        c = (unsigned char)data[i];
        if (!TCHAR_TBL[c]) {
            return 0;
        }
    }
    return 1;
}
size_t token_span(const char *data, size_t lens) {
    size_t i;
    for (i = 0; i < lens; i++) {
        if (!TCHAR_TBL[(unsigned char)data[i]]) {
            break;
        }
    }
    return i;
}
char *_format_va(const char *fmt, va_list args) {
    /* 先用栈缓冲尝试格式化（绝大多数场景够用），成功则直接复制返回，避免堆分配；
     * 仅当字符串超过栈缓冲大小时，才按实际长度堆分配并重试。 */
    char stk[_FMT_STACK_SIZE];
    va_list args2;
    va_copy(args2, args);
    int32_t rtn = vsnprintf(stk, _FMT_STACK_SIZE, fmt, args);
    if (rtn < 0) {
        va_end(args2);
        return NULL;
    }
    if (rtn < _FMT_STACK_SIZE) {
        va_end(args2);
        return dup_zero(stk, (size_t)rtn);
    }
    /* 栈缓冲不足，按实际长度堆分配后重试 */
    size_t size = (size_t)rtn + 1;
    char *pbuff;
    MALLOC(pbuff, size);
    rtn = vsnprintf(pbuff, size, fmt, args2);
    va_end(args2);
    if (rtn < 0) {
        FREE(pbuff);
        return NULL;
    }
    return pbuff;
}
char *format_va(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char *buf = _format_va(fmt, args);
    va_end(args);
    return buf;
}
