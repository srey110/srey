#ifndef CRYPT_PUB_H_
#define CRYPT_PUB_H_

#include "base/macro.h"

// SHA-2 连续 8 轮：工作变量 a~h 不搬动，每轮把名字轮换一位；RND(i,a,b,c,d,e,f,g,h) 为单轮宏。sha256 / sha512 共用
#define CRYPT_ROUNDS8(RND, i) do { \
    RND((i) + 0, a, b, c, d, e, f, g, h); \
    RND((i) + 1, h, a, b, c, d, e, f, g); \
    RND((i) + 2, g, h, a, b, c, d, e, f); \
    RND((i) + 3, f, g, h, a, b, c, d, e); \
    RND((i) + 4, e, f, g, h, a, b, c, d); \
    RND((i) + 5, d, e, f, g, h, a, b, c); \
    RND((i) + 6, c, d, e, f, g, h, a, b); \
    RND((i) + 7, b, c, d, e, f, g, h, a); \
} while (0)

#if defined(OS_WIN)
// 32 位换序；Windows 下用内建函数
static inline uint32_t _crypt_swap32(uint32_t v) {
    return _byteswap_ulong(v);
}
// 64 位换序；Windows 下用内建函数
static inline uint64_t _crypt_swap64(uint64_t v) {
    return _byteswap_uint64(v);
}
#else
// 32 位换序
static inline uint32_t _crypt_swap32(uint32_t v) {
    return (v << 24) | ((v << 8) & 0x00FF0000U) | ((v >> 8) & 0x0000FF00U) | (v >> 24);
}
// 64 位换序
static inline uint64_t _crypt_swap64(uint64_t v) {
    return ((uint64_t)_crypt_swap32((uint32_t)v) << 32) | _crypt_swap32((uint32_t)(v >> 32));
}
#endif
// 按小端读 4 字节
static inline uint32_t _crypt_read32le(const void *p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return IS_LITTLE ? v : _crypt_swap32(v);
}
// 按小端读 8 字节
static inline uint64_t _crypt_read64le(const void *p) {
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    return IS_LITTLE ? v : _crypt_swap64(v);
}
// 按大端读 4 字节
static inline uint32_t _crypt_read32be(const void *p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return IS_LITTLE ? _crypt_swap32(v) : v;
}
// 按大端读 8 字节
static inline uint64_t _crypt_read64be(const void *p) {
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    return IS_LITTLE ? _crypt_swap64(v) : v;
}
// 按小端写 4 字节
static inline void _crypt_write32le(void *p, uint32_t v) {
    if (!IS_LITTLE) {
        v = _crypt_swap32(v);
    }
    memcpy(p, &v, sizeof(v));
}
// 按小端写 8 字节
static inline void _crypt_write64le(void *p, uint64_t v) {
    if (!IS_LITTLE) {
        v = _crypt_swap64(v);
    }
    memcpy(p, &v, sizeof(v));
}
// 按大端写 4 字节
static inline void _crypt_write32be(void *p, uint32_t v) {
    if (IS_LITTLE) {
        v = _crypt_swap32(v);
    }
    memcpy(p, &v, sizeof(v));
}
// 按大端写 8 字节
static inline void _crypt_write64be(void *p, uint64_t v) {
    if (IS_LITTLE) {
        v = _crypt_swap64(v);
    }
    memcpy(p, &v, sizeof(v));
}

#endif//CRYPT_PUB_H_
