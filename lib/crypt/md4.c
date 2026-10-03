#include "crypt/md4.h"
#include "crypt/crypt_pub.h"

#define S11 3
#define S12 7
#define S13 11
#define S14 19
#define S21 3
#define S22 5
#define S23 9
#define S24 13
#define S31 3
#define S32 9
#define S33 11
#define S34 15
#define F(x, y, z) (((x) & (y)) | ((~x) & (z))) // 轮函数 F：选择函数
#define H(x, y, z) ((x) ^ (y) ^ (z)) // 轮函数 H：奇偶函数
#define ROTATE_LEFT(x, n) (((x) << (n)) | ((x) >> (32-(n)))) // 循环左移
// 每步先加与本步新值无关的项，最后才加依赖 b 的那项
// 第一轮操作
#define FF(a, b, c, d, x, s) { (a) += (x); CRYPT_OPAQUE(a); (a) += F ((b), (c), (d)); \
                               (a) = ROTATE_LEFT ((a), (s)); }
// 第二轮操作：多数函数 = (c & d) | (b & (c ^ d))，两半不相交，按加法拆开结果不变
#define GG(a, b, c, d, x, s) { (a) += (x) + (uint32_t)0x5a827999 + ((c) & (d)); CRYPT_OPAQUE(a); (a) += (b) & ((c) ^ (d)); \
                               (a) = ROTATE_LEFT ((a), (s)); }
// 第三轮操作
#define HH(a, b, c, d, x, s) { (a) += (x) + (uint32_t)0x6ed9eba1; CRYPT_OPAQUE(a); (a) += H ((b), (c), (d)); \
                               (a) = ROTATE_LEFT ((a), (s)); }

// MD4 核心变换：依次压缩 nblk 个 64 字节块并更新状态
static void _md4_transform(md4_ctx *md4, const uint8_t *data, size_t nblk) {
    uint32_t i, a, b, c, d, x[16];
    for (; nblk > 0; --nblk, data += 64) {
        for (i = 0; i < 16; ++i) {
            x[i] = read_le32(data + i * 4);
        }
        a = md4->state[0];
        b = md4->state[1];
        c = md4->state[2];
        d = md4->state[3];
        // 第一轮：使用 F 函数
        FF(a, b, c, d, x[0], S11); /* 1 */
        FF(d, a, b, c, x[1], S12); /* 2 */
        FF(c, d, a, b, x[2], S13); /* 3 */
        FF(b, c, d, a, x[3], S14); /* 4 */
        FF(a, b, c, d, x[4], S11); /* 5 */
        FF(d, a, b, c, x[5], S12); /* 6 */
        FF(c, d, a, b, x[6], S13); /* 7 */
        FF(b, c, d, a, x[7], S14); /* 8 */
        FF(a, b, c, d, x[8], S11); /* 9 */
        FF(d, a, b, c, x[9], S12); /* 10 */
        FF(c, d, a, b, x[10], S13); /* 11 */
        FF(b, c, d, a, x[11], S14); /* 12 */
        FF(a, b, c, d, x[12], S11); /* 13 */
        FF(d, a, b, c, x[13], S12); /* 14 */
        FF(c, d, a, b, x[14], S13); /* 15 */
        FF(b, c, d, a, x[15], S14); /* 16 */
        // 第二轮：使用 G 函数
        GG(a, b, c, d, x[0], S21); /* 17 */
        GG(d, a, b, c, x[4], S22); /* 18 */
        GG(c, d, a, b, x[8], S23); /* 19 */
        GG(b, c, d, a, x[12], S24); /* 20 */
        GG(a, b, c, d, x[1], S21); /* 21 */
        GG(d, a, b, c, x[5], S22); /* 22 */
        GG(c, d, a, b, x[9], S23); /* 23 */
        GG(b, c, d, a, x[13], S24); /* 24 */
        GG(a, b, c, d, x[2], S21); /* 25 */
        GG(d, a, b, c, x[6], S22); /* 26 */
        GG(c, d, a, b, x[10], S23); /* 27 */
        GG(b, c, d, a, x[14], S24); /* 28 */
        GG(a, b, c, d, x[3], S21); /* 29 */
        GG(d, a, b, c, x[7], S22); /* 30 */
        GG(c, d, a, b, x[11], S23); /* 31 */
        GG(b, c, d, a, x[15], S24); /* 32 */
        // 第三轮：使用 H 函数
        HH(a, b, c, d, x[0], S31); /* 33 */
        HH(d, a, b, c, x[8], S32); /* 34 */
        HH(c, d, a, b, x[4], S33); /* 35 */
        HH(b, c, d, a, x[12], S34); /* 36 */
        HH(a, b, c, d, x[2], S31); /* 37 */
        HH(d, a, b, c, x[10], S32); /* 38 */
        HH(c, d, a, b, x[6], S33); /* 39 */
        HH(b, c, d, a, x[14], S34); /* 40 */
        HH(a, b, c, d, x[1], S31); /* 41 */
        HH(d, a, b, c, x[9], S32); /* 42 */
        HH(c, d, a, b, x[5], S33); /* 43 */
        HH(b, c, d, a, x[13], S34); /* 44 */
        HH(a, b, c, d, x[3], S31); /* 45 */
        HH(d, a, b, c, x[11], S32); /* 46 */
        HH(c, d, a, b, x[7], S33); /* 47 */
        HH(b, c, d, a, x[15], S34); /* 48 */
        // 先擦调度表再累加状态，理由同 sha1.c
        secure_zero(x, sizeof(x));
        md4->state[0] += a;
        md4->state[1] += b;
        md4->state[2] += c;
        md4->state[3] += d;
    }
}
void md4_init(md4_ctx *md4) {
    md4->count[0] = md4->count[1] = 0;
    md4->state[0] = 0x67452301;
    md4->state[1] = 0xefcdab89;
    md4->state[2] = 0x98badcfe;
    md4->state[3] = 0x10325476;
}
void md4_update(md4_ctx *md4, const void *data, size_t lens) {
    const uint8_t *p = (const uint8_t *)data;
    // 已缓存字节数藏在位计数器低位,必须赶在累加本次长度之前取
    uint32_t index = (uint32_t)((md4->count[0] >> 3) & 0x3f);
    // 位数先按 64 位算再拆进 count[0]/count[1]:直接对 lens 做 32 位移位会在
    // lens 过 4GiB 时丢高位,分支判定拿到截断值而 memcpy 用完整 lens,写爆 data[64]
    uint64_t bits = (uint64_t)lens << 3;
    uint32_t low = (uint32_t)bits;
    if ((md4->count[0] += low) < low) {
        md4->count[1]++;
    }
    md4->count[1] += (uint32_t)(bits >> 32);
    size_t left = 64 - index;
    if (index > 0 && lens >= left) {
        memcpy(&md4->data[index], p, left);
        _md4_transform(md4, md4->data, 1);
        p += left;
        lens -= left;
        index = 0;
    }
    if (lens >= 64) {
        _md4_transform(md4, p, lens / 64);
        p += lens & ~(size_t)63;
        lens &= 63;
    }
    if (lens > 0) {
        memcpy(&md4->data[index], p, lens);
    }
}
void md4_final(md4_ctx *md4, char hash[MD4_BLOCK_SIZE]) {
    uint32_t i = (uint32_t)((md4->count[0] >> 3) & 0x3f);
    md4->data[i++] = 0x80;
    if (i > 56) {
        memset(md4->data + i, 0, 64 - i);
        _md4_transform(md4, md4->data, 1);
        i = 0;
    }
    memset(md4->data + i, 0, 56 - i);
    write_le32(md4->data + 56, md4->count[0]);
    write_le32(md4->data + 60, md4->count[1]);
    _md4_transform(md4, md4->data, 1);
    for (i = 0; i < 4; ++i) {
        write_le32((uint8_t *)hash + i * 4, md4->state[i]);
    }
    secure_zero(md4, sizeof(md4_ctx));
}
