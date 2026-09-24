#include "crypt/xxhash.h"
#include "crypt/crypt_pub.h"
#if defined(_MSC_VER)
#include <intrin.h>
#endif

#define XXH_PRIME32_1 0x9E3779B1U
#define XXH_PRIME32_2 0x85EBCA77U
#define XXH_PRIME32_3 0xC2B2AE3DU
#define XXH_PRIME32_4 0x27D4EB2FU
#define XXH_PRIME32_5 0x165667B1U
#define XXH_PRIME64_1 0x9E3779B185EBCA87ULL
#define XXH_PRIME64_2 0xC2B2AE3D27D4EB4FULL
#define XXH_PRIME64_3 0x165667B19E3779F9ULL
#define XXH_PRIME64_4 0x85EBCA77C2B2AE63ULL
#define XXH_PRIME64_5 0x27D4EB2F165667C5ULL
#define XXH_PRIME_MX1 0x165667919E3779F9ULL
#define XXH_PRIME_MX2 0x9FB21C651E98DF25ULL
#define XXH3_MIDSIZE_MAX 240 // 不超过它走短/中键路径
#define XXH3_MIDSIZE_STARTOFFSET 3
#define XXH3_MIDSIZE_LASTOFFSET 17
#define XXH3_SECRET_SIZE_MIN 136
#define XXH3_STRIPE_LEN 64 // 一条的字节数
#define XXH3_SECRET_CONSUME_RATE 8 // 每条 secret 前移的字节数
#define XXH3_ACC_NB 8
#define XXH3_SECRET_LASTACC_START 7
#define XXH3_SECRET_MERGEACCS_START 11
#define XXH3_SECRET_LIMIT (XXH3_SECRET_SIZE - XXH3_STRIPE_LEN) // scramble 用的 secret 偏移
#define XXH3_STRIPES_PER_BLOCK (XXH3_SECRET_LIMIT / XXH3_SECRET_CONSUME_RATE) // 每块 16 条（1024 字节）
#define XXH3_BUFFER_STRIPES (XXH3_BUFFER_SIZE / XXH3_STRIPE_LEN)
#define ROTL32(x, r) (((x) << (r)) | ((x) >> (32 - (r))))
#define ROTL64(x, r) (((x) << (r)) | ((x) >> (64 - (r))))
#define MUL32TO64(x, y) ((uint64_t)(uint32_t)(x) * (uint64_t)(uint32_t)(y))
#if defined(__GNUC__) || defined(__clang__)
#define XXH_OPAQUE(v) __asm__("" : "+r"(v))
#else
#define XXH_OPAQUE(v) (void)0
#endif
#if defined(__SSE4_1__) || defined(__aarch64__)
#define XXH32_KEEP_SCALAR(v) XXH_OPAQUE(v)
#else
#define XXH32_KEEP_SCALAR(v) (void)0
#endif

static const uint8_t _kSecret[XXH3_SECRET_SIZE] = {
    0xb8, 0xfe, 0x6c, 0x39, 0x23, 0xa4, 0x4b, 0xbe, 0x7c, 0x01, 0x81, 0x2c, 0xf7, 0x21, 0xad, 0x1c,
    0xde, 0xd4, 0x6d, 0xe9, 0x83, 0x90, 0x97, 0xdb, 0x72, 0x40, 0xa4, 0xa4, 0xb7, 0xb3, 0x67, 0x1f,
    0xcb, 0x79, 0xe6, 0x4e, 0xcc, 0xc0, 0xe5, 0x78, 0x82, 0x5a, 0xd0, 0x7d, 0xcc, 0xff, 0x72, 0x21,
    0xb8, 0x08, 0x46, 0x74, 0xf7, 0x43, 0x24, 0x8e, 0xe0, 0x35, 0x90, 0xe6, 0x81, 0x3a, 0x26, 0x4c,
    0x3c, 0x28, 0x52, 0xbb, 0x91, 0xc3, 0x00, 0xcb, 0x88, 0xd0, 0x65, 0x8b, 0x1b, 0x53, 0x2e, 0xa3,
    0x71, 0x64, 0x48, 0x97, 0xa2, 0x0d, 0xf9, 0x4e, 0x38, 0x19, 0xef, 0x46, 0xa9, 0xde, 0xac, 0xd8,
    0xa8, 0xfa, 0x76, 0x3f, 0xe3, 0x9c, 0x34, 0x3f, 0xf9, 0xdc, 0xbb, 0xc7, 0xc7, 0x0b, 0x4f, 0x1d,
    0x8a, 0x51, 0xe0, 0x4b, 0xcd, 0xb4, 0x59, 0x31, 0xc8, 0x9f, 0x7e, 0xc9, 0xd9, 0x78, 0x73, 0x64,
    0xea, 0xc5, 0xac, 0x83, 0x34, 0xd3, 0xeb, 0xc3, 0xc5, 0x81, 0xa0, 0xff, 0xfa, 0x13, 0x63, 0xeb,
    0x17, 0x0d, 0xdd, 0x51, 0xb7, 0xf0, 0xda, 0x49, 0xd3, 0x16, 0x55, 0x26, 0x29, 0xd4, 0x68, 0x9e,
    0x2b, 0x16, 0xbe, 0x58, 0x7d, 0x47, 0xa1, 0xfc, 0x8f, 0xf8, 0xb8, 0xd1, 0x7a, 0xd0, 0x31, 0xce,
    0x45, 0xcb, 0x3a, 0x8f, 0x95, 0x16, 0x04, 0x28, 0xaf, 0xd7, 0xfb, 0xca, 0xbb, 0x4b, 0x40, 0x7e,
};
static const uint64_t _kInitAcc[XXH3_ACC_NB] = {
    XXH_PRIME32_3, XXH_PRIME64_1, XXH_PRIME64_2, XXH_PRIME64_3,
    XXH_PRIME64_4, XXH_PRIME32_2, XXH_PRIME64_5, XXH_PRIME32_1
};

// 拷贝不足 16 字节的数据：首尾两段定长拷贝可以重叠，覆盖整个区间
static inline void _xxh_copy15(uint8_t *dst, const uint8_t *src, size_t lens) {
    uint64_t a, b;
    uint32_t x, y;
    if (lens >= 8) {
        memcpy(&a, src, 8);
        memcpy(&b, src + lens - 8, 8);
        memcpy(dst, &a, 8);
        memcpy(dst + lens - 8, &b, 8);
    } else if (lens >= 4) {
        memcpy(&x, src, 4);
        memcpy(&y, src + lens - 4, 4);
        memcpy(dst, &x, 4);
        memcpy(dst + lens - 4, &y, 4);
    } else if (lens > 0) {
        dst[0] = src[0];
        dst[lens >> 1] = src[lens >> 1];
        dst[lens - 1] = src[lens - 1];
    }
}
// 拷贝不足 32 字节的数据，同 _xxh_copy15
static inline void _xxh_copy31(uint8_t *dst, const uint8_t *src, size_t lens) {
    uint64_t a, b, c, d;
    if (lens >= 16) {
        memcpy(&a, src, 8);
        memcpy(&b, src + 8, 8);
        memcpy(&c, src + lens - 16, 8);
        memcpy(&d, src + lens - 8, 8);
        memcpy(dst, &a, 8);
        memcpy(dst + 8, &b, 8);
        memcpy(dst + lens - 16, &c, 8);
        memcpy(dst + lens - 8, &d, 8);
    } else {
        _xxh_copy15(dst, src, lens);
    }
}
// 64x64->128 位乘法
static inline xxh128_t _xxh_mul128(uint64_t a, uint64_t b) {
    xxh128_t r;
#if defined(__SIZEOF_INT128__)
    unsigned __int128 p = (unsigned __int128)a * b;
    r.low = (uint64_t)p;
    r.high = (uint64_t)(p >> 64);
#elif defined(_MSC_VER) && defined(_M_X64) && !defined(_M_ARM64EC)
    r.low = _umul128(a, b, &r.high);
#elif defined(_MSC_VER) && (defined(_M_ARM64) || defined(_M_ARM64EC))
    r.low = a * b;
    r.high = __umulh(a, b);
#else
    uint64_t lolo = MUL32TO64(a, b);
    uint64_t hilo = MUL32TO64(a >> 32, b);
    uint64_t lohi = MUL32TO64(a, b >> 32);
    uint64_t hihi = MUL32TO64(a >> 32, b >> 32);
    uint64_t cross = (lolo >> 32) + (hilo & 0xFFFFFFFFULL) + lohi;
    r.high = (hilo >> 32) + (cross >> 32) + hihi;
    r.low = (cross << 32) | (lolo & 0xFFFFFFFFULL);
#endif
    return r;
}
static inline uint32_t _xxh32_round(uint32_t acc, uint32_t input) {
    acc += input * XXH_PRIME32_2;
    acc = ROTL32(acc, 13);
    acc *= XXH_PRIME32_1;
    XXH32_KEEP_SCALAR(acc);
    return acc;
}
static inline uint32_t _xxh32_avalanche(uint32_t h) {
    h ^= h >> 15;
    h *= XXH_PRIME32_2;
    h ^= h >> 13;
    h *= XXH_PRIME32_3;
    h ^= h >> 16;
    return h;
}
static inline void _xxh32_init_acc(uint32_t acc[4], uint32_t seed) {
    acc[0] = seed + XXH_PRIME32_1 + XXH_PRIME32_2;
    acc[1] = seed + XXH_PRIME32_2;
    acc[2] = seed;
    acc[3] = seed - XXH_PRIME32_1;
}
// 按 16 字节一条累加 lens / 16 条，返回剩余部分的起点
static inline const uint8_t *_xxh32_consume(uint32_t acc[4], const uint8_t *p, size_t lens) {
    uint32_t v0 = acc[0], v1 = acc[1], v2 = acc[2], v3 = acc[3];
    size_t n;
    for (n = lens / 16; n > 0; n--) {
        v0 = _xxh32_round(v0, _crypt_read32le(p));
        v1 = _xxh32_round(v1, _crypt_read32le(p + 4));
        v2 = _xxh32_round(v2, _crypt_read32le(p + 8));
        v3 = _xxh32_round(v3, _crypt_read32le(p + 12));
        p += 16;
    }
    acc[0] = v0;
    acc[1] = v1;
    acc[2] = v2;
    acc[3] = v3;
    return p;
}
static inline uint32_t _xxh32_merge(const uint32_t acc[4]) {
    return ROTL32(acc[0], 1) + ROTL32(acc[1], 7) + ROTL32(acc[2], 12) + ROTL32(acc[3], 18);
}
// 处理末尾不足 16 字节的部分
static inline uint32_t _xxh32_finalize(uint32_t h, const uint8_t *p, size_t lens) {
    lens &= 15;
    while (lens >= 4) {
        h += _crypt_read32le(p) * XXH_PRIME32_3;
        h = ROTL32(h, 17) * XXH_PRIME32_4;
        p += 4;
        lens -= 4;
    }
    while (lens > 0) {
        h += (uint32_t)(*p) * XXH_PRIME32_5;
        h = ROTL32(h, 11) * XXH_PRIME32_1;
        p++;
        lens--;
    }
    return _xxh32_avalanche(h);
}
uint32_t xxh32(const void *data, size_t lens, uint32_t seed) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t acc[4];
    uint32_t h;
    if (lens >= 16) {
        _xxh32_init_acc(acc, seed);
        p = _xxh32_consume(acc, p, lens);
        h = _xxh32_merge(acc);
    } else {
        h = seed + XXH_PRIME32_5;
    }
    h += (uint32_t)lens;
    return _xxh32_finalize(h, p, lens);
}
void xxh32_init(xxh32_ctx *ctx, uint32_t seed) {
    ZERO(ctx, offsetof(xxh32_ctx, acc));// acc 在末尾，由下一行写入
    _xxh32_init_acc(ctx->acc, seed);
}
void xxh32_update(xxh32_ctx *ctx, const void *data, size_t lens) {
    const uint8_t *p = (const uint8_t *)data;
    size_t fill;
    if (0 == lens) {
        return;
    }
    ctx->total += (uint32_t)lens;
    ctx->large |= (uint32_t)((lens >= 16) | (ctx->total >= 16));
    if (lens < sizeof(ctx->buf) - ctx->bufsize) {
        _xxh_copy15(ctx->buf + ctx->bufsize, p, lens);
        ctx->bufsize += (uint32_t)lens;
        return;
    }
    if (0 != ctx->bufsize) {
        fill = sizeof(ctx->buf) - ctx->bufsize;
        _xxh_copy15(ctx->buf + ctx->bufsize, p, fill);
        _xxh32_consume(ctx->acc, ctx->buf, sizeof(ctx->buf));
        ctx->bufsize = 0;
        p += fill;
        lens -= fill;
    }
    if (lens >= sizeof(ctx->buf)) {
        p = _xxh32_consume(ctx->acc, p, lens);
        lens &= 15;
    }
    if (lens > 0) {
        _xxh_copy15(ctx->buf, p, lens);
        ctx->bufsize = (uint32_t)lens;
    }
}
uint32_t xxh32_digest(const xxh32_ctx *ctx) {
    uint32_t h;
    if (ctx->large) {
        h = _xxh32_merge(ctx->acc);
    } else {
        h = ctx->acc[2] + XXH_PRIME32_5;
    }
    h += ctx->total;
    return _xxh32_finalize(h, ctx->buf, ctx->bufsize);
}

static inline uint64_t _xxh64_round(uint64_t acc, uint64_t input) {
    acc += input * XXH_PRIME64_2;
    acc = ROTL64(acc, 31);
    return acc * XXH_PRIME64_1;
}
static inline uint64_t _xxh64_merge_round(uint64_t acc, uint64_t val) {
    acc ^= _xxh64_round(0, val);
    return acc * XXH_PRIME64_1 + XXH_PRIME64_4;
}
static inline uint64_t _xxh64_avalanche(uint64_t h) {
    h ^= h >> 33;
    h *= XXH_PRIME64_2;
    h ^= h >> 29;
    h *= XXH_PRIME64_3;
    h ^= h >> 32;
    return h;
}
static inline void _xxh64_init_acc(uint64_t acc[4], uint64_t seed) {
    acc[0] = seed + XXH_PRIME64_1 + XXH_PRIME64_2;
    acc[1] = seed + XXH_PRIME64_2;
    acc[2] = seed;
    acc[3] = seed - XXH_PRIME64_1;
}
// 按 32 字节一条累加 lens / 32 条，返回剩余部分的起点
static inline const uint8_t *_xxh64_consume(uint64_t acc[4], const uint8_t *p, size_t lens) {
    uint64_t v0 = acc[0], v1 = acc[1], v2 = acc[2], v3 = acc[3];
    size_t n;
    for (n = lens / 32; n > 0; n--) {
        v0 = _xxh64_round(v0, _crypt_read64le(p));
        v1 = _xxh64_round(v1, _crypt_read64le(p + 8));
        v2 = _xxh64_round(v2, _crypt_read64le(p + 16));
        v3 = _xxh64_round(v3, _crypt_read64le(p + 24));
        p += 32;
    }
    acc[0] = v0;
    acc[1] = v1;
    acc[2] = v2;
    acc[3] = v3;
    return p;
}
static inline uint64_t _xxh64_merge(const uint64_t acc[4]) {
    uint64_t h = ROTL64(acc[0], 1) + ROTL64(acc[1], 7) + ROTL64(acc[2], 12) + ROTL64(acc[3], 18);
    h = _xxh64_merge_round(h, acc[0]);
    h = _xxh64_merge_round(h, acc[1]);
    h = _xxh64_merge_round(h, acc[2]);
    return _xxh64_merge_round(h, acc[3]);
}
// 处理末尾不足 32 字节的部分
static inline uint64_t _xxh64_finalize(uint64_t h, const uint8_t *p, size_t lens) {
    lens &= 31;
    while (lens >= 8) {
        h ^= _xxh64_round(0, _crypt_read64le(p));
        h = ROTL64(h, 27) * XXH_PRIME64_1 + XXH_PRIME64_4;
        p += 8;
        lens -= 8;
    }
    if (lens >= 4) {
        h ^= (uint64_t)_crypt_read32le(p) * XXH_PRIME64_1;
        h = ROTL64(h, 23) * XXH_PRIME64_2 + XXH_PRIME64_3;
        p += 4;
        lens -= 4;
    }
    while (lens > 0) {
        h ^= (uint64_t)(*p) * XXH_PRIME64_5;
        h = ROTL64(h, 11) * XXH_PRIME64_1;
        p++;
        lens--;
    }
    return _xxh64_avalanche(h);
}
uint64_t xxh64(const void *data, size_t lens, uint64_t seed) {
    const uint8_t *p = (const uint8_t *)data;
    uint64_t acc[4];
    uint64_t h;
    if (lens >= 32) {
        _xxh64_init_acc(acc, seed);
        p = _xxh64_consume(acc, p, lens);
        h = _xxh64_merge(acc);
    } else {
        h = seed + XXH_PRIME64_5;
    }
    h += (uint64_t)lens;
    return _xxh64_finalize(h, p, lens);
}
void xxh64_init(xxh64_ctx *ctx, uint64_t seed) {
    ZERO(ctx, offsetof(xxh64_ctx, acc));// 同 xxh32_init
    _xxh64_init_acc(ctx->acc, seed);
}
void xxh64_update(xxh64_ctx *ctx, const void *data, size_t lens) {
    const uint8_t *p = (const uint8_t *)data;
    size_t fill;
    if (0 == lens) {
        return;
    }
    ctx->total += lens;
    if (lens < sizeof(ctx->buf) - ctx->bufsize) {
        _xxh_copy31(ctx->buf + ctx->bufsize, p, lens);
        ctx->bufsize += (uint32_t)lens;
        return;
    }
    if (0 != ctx->bufsize) {
        fill = sizeof(ctx->buf) - ctx->bufsize;
        _xxh_copy31(ctx->buf + ctx->bufsize, p, fill);
        _xxh64_consume(ctx->acc, ctx->buf, sizeof(ctx->buf));
        ctx->bufsize = 0;
        p += fill;
        lens -= fill;
    }
    if (lens >= sizeof(ctx->buf)) {
        p = _xxh64_consume(ctx->acc, p, lens);
        lens &= 31;
    }
    if (lens > 0) {
        _xxh_copy31(ctx->buf, p, lens);
        ctx->bufsize = (uint32_t)lens;
    }
}
uint64_t xxh64_digest(const xxh64_ctx *ctx) {
    uint64_t h;
    if (ctx->total >= 32) {
        h = _xxh64_merge(ctx->acc);
    } else {
        h = ctx->acc[2] + XXH_PRIME64_5;
    }
    h += ctx->total;
    return _xxh64_finalize(h, ctx->buf, ctx->bufsize);
}

static inline uint64_t _xxh3_avalanche(uint64_t h) {
    h ^= h >> 37;
    h *= XXH_PRIME_MX1;
    h ^= h >> 32;
    return h;
}
static inline uint64_t _xxh3_rrmxmx(uint64_t h, uint64_t lens) {
    h ^= ROTL64(h, 49) ^ ROTL64(h, 24);
    h *= XXH_PRIME_MX2;
    h ^= (h >> 35) + lens;
    h *= XXH_PRIME_MX2;
    h ^= h >> 28;
    return h;
}
static inline uint64_t _xxh3_mul_fold(uint64_t a, uint64_t b) {
    xxh128_t r = _xxh_mul128(a, b);
    return r.low ^ r.high;
}
static FORCE_INLINE uint64_t _xxh3_mix16(const uint8_t *p, const uint8_t *secret, uint64_t seed) {
    uint64_t lo = _crypt_read64le(p);
    uint64_t hi = _crypt_read64le(p + 8);
    return _xxh3_mul_fold(lo ^ (_crypt_read64le(secret) + seed), hi ^ (_crypt_read64le(secret + 8) - seed));
}
// 由 seed 派生 secret，seed 为 0 时结果就是默认 secret
static void _xxh3_init_secret(uint8_t secret[XXH3_SECRET_SIZE], uint64_t seed) {
    const uint8_t *ksecret = _kSecret;
    size_t i;
#if defined(__clang__)
    XXH_OPAQUE(ksecret);
#endif
    for (i = 0; i < XXH3_SECRET_SIZE; i += 16) {
        _crypt_write64le(secret + i, _crypt_read64le(ksecret + i) + seed);
        _crypt_write64le(secret + i + 8, _crypt_read64le(ksecret + i + 8) - seed);
    }
}
// 长输入：累加一条（64 字节）
static inline void _xxh3_accumulate_512(uint64_t acc[XXH3_ACC_NB], const uint8_t *p, const uint8_t *secret) {
    size_t i;
    uint64_t val, key;
    for (i = 0; i < XXH3_ACC_NB; i++) {
        val = _crypt_read64le(p + 8 * i);
        key = val ^ _crypt_read64le(secret + 8 * i);
        acc[i ^ 1] += val;
        acc[i] += MUL32TO64(key, key >> 32);
    }
}
static inline void _xxh3_accumulate(uint64_t acc[XXH3_ACC_NB], const uint8_t *p, const uint8_t *secret, size_t nstripes) {
    uint64_t local[XXH3_ACC_NB];
    size_t n;
    memcpy(local, acc, sizeof(local));
    for (n = 0; n < nstripes; n++) {
        _xxh3_accumulate_512(local, p + n * XXH3_STRIPE_LEN, secret + n * XXH3_SECRET_CONSUME_RATE);
    }
    memcpy(acc, local, sizeof(local));
}
// 长输入：每块结束时打散累加器
static inline void _xxh3_scramble(uint64_t acc[XXH3_ACC_NB], const uint8_t *secret) {
    size_t i;
    uint64_t a;
    for (i = 0; i < XXH3_ACC_NB; i++) {
        a = acc[i];
        a ^= a >> 47;
        a ^= _crypt_read64le(secret + 8 * i);
        acc[i] = a * XXH_PRIME32_1;
    }
}
// 一次性长输入主循环，lens 须 > XXH3_MIDSIZE_MAX；每满一块（16 条）scramble 一次
static void _xxh3_hash_long(uint64_t acc[XXH3_ACC_NB], const uint8_t *p, size_t lens, const uint8_t *secret) {
    uint64_t local[XXH3_ACC_NB];
    size_t nstripes = (lens - 1) / XXH3_STRIPE_LEN;
    size_t n, s;
    memcpy(local, acc, sizeof(local));
    for (n = 0, s = 0; n < nstripes; n++) {
        _xxh3_accumulate_512(local, p + n * XXH3_STRIPE_LEN, secret + s * XXH3_SECRET_CONSUME_RATE);
        if (++s == XXH3_STRIPES_PER_BLOCK) {
            _xxh3_scramble(local, secret + XXH3_SECRET_LIMIT);
            s = 0;
        }
    }
    _xxh3_accumulate_512(local, p + lens - XXH3_STRIPE_LEN, secret + XXH3_SECRET_LIMIT - XXH3_SECRET_LASTACC_START);
    memcpy(acc, local, sizeof(local));
}
static uint64_t _xxh3_merge(const uint64_t acc[XXH3_ACC_NB], const uint8_t *secret, uint64_t start) {
    uint64_t h = start;
    size_t i;
    for (i = 0; i < 4; i++) {
        h += _xxh3_mul_fold(acc[2 * i] ^ _crypt_read64le(secret + 16 * i), acc[2 * i + 1] ^ _crypt_read64le(secret + 16 * i + 8));
    }
    return _xxh3_avalanche(h);
}
// 流式：处理 n 条，跨块时 scramble；*sofar 为当前块已处理的条数
static void _xxh3_consume(uint64_t acc[XXH3_ACC_NB], uint32_t *sofar, const uint8_t *p, size_t n, const uint8_t *secret) {
    const uint8_t *cur = secret + *sofar * XXH3_SECRET_CONSUME_RATE;
    size_t cnt;
    if (n >= XXH3_STRIPES_PER_BLOCK - *sofar) {
        cnt = XXH3_STRIPES_PER_BLOCK - *sofar;
        do {
            _xxh3_accumulate(acc, p, cur, cnt);
            _xxh3_scramble(acc, secret + XXH3_SECRET_LIMIT);
            p += cnt * XXH3_STRIPE_LEN;
            n -= cnt;
            cnt = XXH3_STRIPES_PER_BLOCK;
            cur = secret;
        } while (n >= XXH3_STRIPES_PER_BLOCK);
        *sofar = 0;
    }
    if (n > 0) {
        _xxh3_accumulate(acc, p, cur, n);
        *sofar += (uint32_t)n;
    }
}
// 流式：在 acc 副本上补完缓冲里的条和最后一条，不改 ctx；ctx->total 须 > XXH3_MIDSIZE_MAX
static void _xxh3_digest_long(const xxh3_ctx *ctx, uint64_t acc[XXH3_ACC_NB]) {
    uint8_t last[XXH3_STRIPE_LEN];
    const uint8_t *lastp;
    uint32_t sofar = ctx->nstripes;
    size_t catchup;
    memcpy(acc, ctx->acc, sizeof(ctx->acc));
    if (ctx->bufsize >= XXH3_STRIPE_LEN) {
        _xxh3_consume(acc, &sofar, ctx->buf, (ctx->bufsize - 1) / XXH3_STRIPE_LEN, ctx->secret);
        lastp = ctx->buf + ctx->bufsize - XXH3_STRIPE_LEN;
    } else {
        catchup = XXH3_STRIPE_LEN - ctx->bufsize;
        memcpy(last, ctx->buf + XXH3_BUFFER_SIZE - catchup, catchup);
        memcpy(last + catchup, ctx->buf, ctx->bufsize);
        lastp = last;
    }
    _xxh3_accumulate_512(acc, lastp, ctx->secret + XXH3_SECRET_LIMIT - XXH3_SECRET_LASTACC_START);
}
// 长输入收尾：返回 64 位结果，high 非 NULL 时另算 128 位的高 64 位
static uint64_t _xxh3_long_final(const uint64_t acc[XXH3_ACC_NB], const uint8_t *secret, uint64_t lens, uint64_t *high) {
    if (NULL != high) {
        *high = _xxh3_merge(acc, secret + XXH3_SECRET_LIMIT - XXH3_SECRET_MERGEACCS_START, ~(lens * XXH_PRIME64_2));
    }
    return _xxh3_merge(acc, secret + XXH3_SECRET_MERGEACCS_START, lens * XXH_PRIME64_1);
}
// 一次性长输入（lens > XXH3_MIDSIZE_MAX），64 位与 128 位共用；seed 为 0 直接用默认 secret
NOINLINE static uint64_t _xxh3_long(const uint8_t *p, size_t lens, uint64_t seed, uint64_t *high) {
    uint64_t acc[XXH3_ACC_NB];
    uint8_t secret[XXH3_SECRET_SIZE];
    const uint8_t *s = _kSecret;
    if (0 != seed) {
        _xxh3_init_secret(secret, seed);
        s = secret;
    }
    memcpy(acc, _kInitAcc, sizeof(acc));
    _xxh3_hash_long(acc, p, lens, s);
    return _xxh3_long_final(acc, s, (uint64_t)lens, high);
}

static inline uint64_t _xxh3_64_1to3(const uint8_t *p, size_t lens, uint64_t seed) {
    uint32_t combined = ((uint32_t)p[0] << 16) | ((uint32_t)p[lens >> 1] << 24)
        | (uint32_t)p[lens - 1] | ((uint32_t)lens << 8);
    uint64_t bitflip = (_crypt_read32le(_kSecret) ^ _crypt_read32le(_kSecret + 4)) + seed;
    return _xxh64_avalanche((uint64_t)combined ^ bitflip);
}
static inline uint64_t _xxh3_64_4to8(const uint8_t *p, size_t lens, uint64_t seed) {
    uint64_t bitflip;
    uint64_t input;
    seed ^= (uint64_t)_crypt_swap32((uint32_t)seed) << 32;
    bitflip = (_crypt_read64le(_kSecret + 8) ^ _crypt_read64le(_kSecret + 16)) - seed;
    input = _crypt_read32le(p + lens - 4) + ((uint64_t)_crypt_read32le(p) << 32);
    return _xxh3_rrmxmx(input ^ bitflip, lens);
}
static inline uint64_t _xxh3_64_9to16(const uint8_t *p, size_t lens, uint64_t seed) {
    uint64_t bitflip1 = (_crypt_read64le(_kSecret + 24) ^ _crypt_read64le(_kSecret + 32)) + seed;
    uint64_t bitflip2 = (_crypt_read64le(_kSecret + 40) ^ _crypt_read64le(_kSecret + 48)) - seed;
    uint64_t lo = _crypt_read64le(p) ^ bitflip1;
    uint64_t hi = _crypt_read64le(p + lens - 8) ^ bitflip2;
    return _xxh3_avalanche(lens + _crypt_swap64(lo) + hi + _xxh3_mul_fold(lo, hi));
}
static inline uint64_t _xxh3_64_17to128(const uint8_t *p, size_t lens, uint64_t seed) {
    uint64_t acc = lens * XXH_PRIME64_1;
    if (lens > 32) {
        if (lens > 64) {
            if (lens > 96) {
                acc += _xxh3_mix16(p + 48, _kSecret + 96, seed);
                acc += _xxh3_mix16(p + lens - 64, _kSecret + 112, seed);
            }
            acc += _xxh3_mix16(p + 32, _kSecret + 64, seed);
            acc += _xxh3_mix16(p + lens - 48, _kSecret + 80, seed);
        }
        acc += _xxh3_mix16(p + 16, _kSecret + 32, seed);
        acc += _xxh3_mix16(p + lens - 32, _kSecret + 48, seed);
    }
    acc += _xxh3_mix16(p, _kSecret, seed);
    acc += _xxh3_mix16(p + lens - 16, _kSecret + 16, seed);
    return _xxh3_avalanche(acc);
}
NOINLINE static uint64_t _xxh3_64_129to240(const uint8_t *p, size_t lens, uint64_t seed) {
    const uint8_t *secret = _kSecret;
    uint64_t acc = lens * XXH_PRIME64_1;
    uint64_t acc_end;
    size_t rounds = lens / 16;
    size_t i;
    XXH_OPAQUE(secret);
    for (i = 0; i < 8; i++) {
        acc += _xxh3_mix16(p + 16 * i, secret + 16 * i, seed);
    }
    acc_end = _xxh3_mix16(p + lens - 16, secret + XXH3_SECRET_SIZE_MIN - XXH3_MIDSIZE_LASTOFFSET, seed);
    acc = _xxh3_avalanche(acc);
    for (i = 8; i < rounds; i++) {
        XXH_OPAQUE(acc);
        acc_end += _xxh3_mix16(p + 16 * i, secret + 16 * (i - 8) + XXH3_MIDSIZE_STARTOFFSET, seed);
    }
    return _xxh3_avalanche(acc + acc_end);
}
// lens <= 16
static inline uint64_t _xxh3_64_0to16(const uint8_t *p, size_t lens, uint64_t seed) {
    if (lens > 8) {
        return _xxh3_64_9to16(p, lens, seed);
    }
    if (lens >= 4) {
        return _xxh3_64_4to8(p, lens, seed);
    }
    if (lens > 0) {
        return _xxh3_64_1to3(p, lens, seed);
    }
    return _xxh64_avalanche(seed ^ (_crypt_read64le(_kSecret + 56) ^ _crypt_read64le(_kSecret + 64)));
}
uint64_t xxh3_64(const void *data, size_t lens, uint64_t seed) {
    const uint8_t *p = (const uint8_t *)data;
    if (lens <= 16) {
        return _xxh3_64_0to16(p, lens, seed);
    }
    if (lens <= 128) {
        return _xxh3_64_17to128(p, lens, seed);
    }
    if (lens <= XXH3_MIDSIZE_MAX) {
        return _xxh3_64_129to240(p, lens, seed);
    }
    return _xxh3_long(p, lens, seed, NULL);
}
static inline xxh128_t _xxh3_128_1to3(const uint8_t *p, size_t lens, uint64_t seed) {
    uint32_t combinedl = ((uint32_t)p[0] << 16) | ((uint32_t)p[lens >> 1] << 24)
        | (uint32_t)p[lens - 1] | ((uint32_t)lens << 8);
    uint32_t combinedh = ROTL32(_crypt_swap32(combinedl), 13);
    uint64_t bitflipl = (_crypt_read32le(_kSecret) ^ _crypt_read32le(_kSecret + 4)) + seed;
    uint64_t bitfliph = (_crypt_read32le(_kSecret + 8) ^ _crypt_read32le(_kSecret + 12)) - seed;
    xxh128_t h;
    h.low = _xxh64_avalanche((uint64_t)combinedl ^ bitflipl);
    h.high = _xxh64_avalanche((uint64_t)combinedh ^ bitfliph);
    return h;
}
static inline xxh128_t _xxh3_128_4to8(const uint8_t *p, size_t lens, uint64_t seed) {
    uint64_t bitflip;
    uint64_t input;
    xxh128_t m;
    seed ^= (uint64_t)_crypt_swap32((uint32_t)seed) << 32;
    input = _crypt_read32le(p) + ((uint64_t)_crypt_read32le(p + lens - 4) << 32);
    bitflip = (_crypt_read64le(_kSecret + 16) ^ _crypt_read64le(_kSecret + 24)) + seed;
    m = _xxh_mul128(input ^ bitflip, XXH_PRIME64_1 + ((uint64_t)lens << 2));
    m.high += m.low << 1;
    m.low ^= m.high >> 3;
    m.low ^= m.low >> 35;
    m.low *= XXH_PRIME_MX2;
    m.low ^= m.low >> 28;
    m.high = _xxh3_avalanche(m.high);
    return m;
}
static inline xxh128_t _xxh3_128_9to16(const uint8_t *p, size_t lens, uint64_t seed) {
    uint64_t bitflipl = (_crypt_read64le(_kSecret + 32) ^ _crypt_read64le(_kSecret + 40)) - seed;
    uint64_t bitfliph = (_crypt_read64le(_kSecret + 48) ^ _crypt_read64le(_kSecret + 56)) + seed;
    uint64_t lo = _crypt_read64le(p);
    uint64_t hi = _crypt_read64le(p + lens - 8);
    xxh128_t m = _xxh_mul128(lo ^ hi ^ bitflipl, XXH_PRIME64_1);
    xxh128_t h;
    m.low += (uint64_t)(lens - 1) << 54;
    hi ^= bitfliph;
    m.high += hi + MUL32TO64(hi, XXH_PRIME32_2 - 1);
    m.low ^= _crypt_swap64(m.high);
    h = _xxh_mul128(m.low, XXH_PRIME64_2);
    h.high += m.high * XXH_PRIME64_2;
    h.low = _xxh3_avalanche(h.low);
    h.high = _xxh3_avalanche(h.high);
    return h;
}
static FORCE_INLINE xxh128_t _xxh3_128_mix32(xxh128_t acc, const uint8_t *in1, const uint8_t *in2, const uint8_t *secret, uint64_t seed) {
    acc.low += _xxh3_mix16(in1, secret, seed);
    acc.low ^= _crypt_read64le(in2) + _crypt_read64le(in2 + 8);
    acc.high += _xxh3_mix16(in2, secret + 16, seed);
    acc.high ^= _crypt_read64le(in1) + _crypt_read64le(in1 + 8);
    return acc;
}
// 中键两段路径共用的收尾
static inline xxh128_t _xxh3_128_mid_final(xxh128_t acc, size_t lens, uint64_t seed) {
    xxh128_t h;
    h.low = _xxh3_avalanche(acc.low + acc.high);
    h.high = 0 - _xxh3_avalanche(acc.low * XXH_PRIME64_1 + acc.high * XXH_PRIME64_4 + (lens - seed) * XXH_PRIME64_2);
    return h;
}
static inline xxh128_t _xxh3_128_17to128(const uint8_t *p, size_t lens, uint64_t seed) {
    xxh128_t acc;
    acc.low = lens * XXH_PRIME64_1;
    acc.high = 0;
    if (lens > 32) {
        if (lens > 64) {
            if (lens > 96) {
                acc = _xxh3_128_mix32(acc, p + 48, p + lens - 64, _kSecret + 96, seed);
            }
            acc = _xxh3_128_mix32(acc, p + 32, p + lens - 48, _kSecret + 64, seed);
        }
        acc = _xxh3_128_mix32(acc, p + 16, p + lens - 32, _kSecret + 32, seed);
    }
    acc = _xxh3_128_mix32(acc, p, p + lens - 16, _kSecret, seed);
    return _xxh3_128_mid_final(acc, lens, seed);
}
NOINLINE static xxh128_t _xxh3_128_129to240(const uint8_t *p, size_t lens, uint64_t seed) {
    const uint8_t *secret = _kSecret;
    xxh128_t acc;
    size_t i;
    XXH_OPAQUE(secret);
    acc.low = lens * XXH_PRIME64_1;
    acc.high = 0;
    for (i = 32; i < 160; i += 32) {
        acc = _xxh3_128_mix32(acc, p + i - 32, p + i - 16, secret + i - 32, seed);
    }
    acc.low = _xxh3_avalanche(acc.low);
    acc.high = _xxh3_avalanche(acc.high);
    for (i = 160; i <= lens; i += 32) {
        acc = _xxh3_128_mix32(acc, p + i - 32, p + i - 16, secret + XXH3_MIDSIZE_STARTOFFSET + i - 160, seed);
    }
    acc = _xxh3_128_mix32(acc, p + lens - 16, p + lens - 32,
        secret + XXH3_SECRET_SIZE_MIN - XXH3_MIDSIZE_LASTOFFSET - 16, 0 - seed);
    return _xxh3_128_mid_final(acc, lens, seed);
}
// lens <= 16
static inline xxh128_t _xxh3_128_0to16(const uint8_t *p, size_t lens, uint64_t seed) {
    xxh128_t h;
    if (lens > 8) {
        return _xxh3_128_9to16(p, lens, seed);
    }
    if (lens >= 4) {
        return _xxh3_128_4to8(p, lens, seed);
    }
    if (lens > 0) {
        return _xxh3_128_1to3(p, lens, seed);
    }
    h.low = _xxh64_avalanche(seed ^ (_crypt_read64le(_kSecret + 64) ^ _crypt_read64le(_kSecret + 72)));
    h.high = _xxh64_avalanche(seed ^ (_crypt_read64le(_kSecret + 80) ^ _crypt_read64le(_kSecret + 88)));
    return h;
}
xxh128_t xxh3_128(const void *data, size_t lens, uint64_t seed) {
    const uint8_t *p = (const uint8_t *)data;
    xxh128_t h;
    if (lens <= 16) {
        return _xxh3_128_0to16(p, lens, seed);
    }
    if (lens <= 128) {
        return _xxh3_128_17to128(p, lens, seed);
    }
    if (lens <= XXH3_MIDSIZE_MAX) {
        return _xxh3_128_129to240(p, lens, seed);
    }
    h.low = _xxh3_long(p, lens, seed, &h.high);
    return h;
}
void xxh3_init(xxh3_ctx *ctx, uint64_t seed) {
    ctx->bufsize = 0;// buf 只读 bufsize 以内及写过的末条，secret 下面整段重写，都不必清零
    ctx->nstripes = 0;
    ctx->total = 0;
    memcpy(ctx->acc, _kInitAcc, sizeof(ctx->acc));
    ctx->seed = seed;
    _xxh3_init_secret(ctx->secret, seed);
}
void xxh3_update(xxh3_ctx *ctx, const void *data, size_t lens) {
    const uint8_t *p = (const uint8_t *)data;
    size_t fill;
    size_t n;
    if (0 == lens) {
        return;
    }
    ctx->total += lens;
    if (lens <= XXH3_BUFFER_SIZE - ctx->bufsize) {
        memcpy(ctx->buf + ctx->bufsize, p, lens);
        ctx->bufsize += (uint32_t)lens;
        return;
    }
    if (0 != ctx->bufsize) {
        fill = XXH3_BUFFER_SIZE - ctx->bufsize;
        memcpy(ctx->buf + ctx->bufsize, p, fill);
        _xxh3_consume(ctx->acc, &ctx->nstripes, ctx->buf, XXH3_BUFFER_STRIPES, ctx->secret);
        ctx->bufsize = 0;
        p += fill;
        lens -= fill;
    }
    if (lens > XXH3_BUFFER_SIZE) {
        n = (lens - 1) / XXH3_STRIPE_LEN;
        _xxh3_consume(ctx->acc, &ctx->nstripes, p, n, ctx->secret);
        p += n * XXH3_STRIPE_LEN;
        lens -= n * XXH3_STRIPE_LEN;
        memcpy(ctx->buf + XXH3_BUFFER_SIZE - XXH3_STRIPE_LEN, p - XXH3_STRIPE_LEN, XXH3_STRIPE_LEN);
    }
    memcpy(ctx->buf, p, lens);
    ctx->bufsize = (uint32_t)lens;
}
uint64_t xxh3_digest64(const xxh3_ctx *ctx) {
    uint64_t acc[XXH3_ACC_NB];
    if (ctx->total > XXH3_MIDSIZE_MAX) {
        _xxh3_digest_long(ctx, acc);
        return _xxh3_long_final(acc, ctx->secret, ctx->total, NULL);
    }
    return xxh3_64(ctx->buf, (size_t)ctx->total, ctx->seed);
}
xxh128_t xxh3_digest128(const xxh3_ctx *ctx) {
    uint64_t acc[XXH3_ACC_NB];
    xxh128_t h;
    if (ctx->total > XXH3_MIDSIZE_MAX) {
        _xxh3_digest_long(ctx, acc);
        h.low = _xxh3_long_final(acc, ctx->secret, ctx->total, &h.high);
        return h;
    }
    return xxh3_128(ctx->buf, (size_t)ctx->total, ctx->seed);
}
