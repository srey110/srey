#include "crypt/sha1.h"
#include "crypt/crypt_pub.h"

#define ROTLEFT(a, b) (((a) << (b)) | ((a) >> (32 - (b))))
#define F0(b, c, d) ((((c) ^ (d)) & (b)) ^ (d))
#define F1(b, c, d) ((b) ^ (c) ^ (d))
#define F2(b, c, d) (((b) & (c)) | (((b) | (c)) & (d)))
// 调度表只留 16 项滚动复用：第 i 项覆盖 i-16 那格
#define W(i) (m[(i) & 15] = ROTLEFT(m[((i) + 13) & 15] ^ m[((i) + 8) & 15] ^ m[((i) + 2) & 15] ^ m[(i) & 15], 1))
// 每轮不搬动变量，靠下一轮轮换实参位置代替 e=d, d=c... 的赋值
#define R0(a, b, c, d, e, i) e += ROTLEFT(a, 5) + F0(b, c, d) + 0x5a827999 + m[i]; b = ROTLEFT(b, 30)
#define R1(a, b, c, d, e, i) e += ROTLEFT(a, 5) + F0(b, c, d) + 0x5a827999 + W(i); b = ROTLEFT(b, 30)
#define R2(a, b, c, d, e, i) e += ROTLEFT(a, 5) + F1(b, c, d) + 0x6ed9eba1 + W(i); b = ROTLEFT(b, 30)
#define R3(a, b, c, d, e, i) e += ROTLEFT(a, 5) + F2(b, c, d) + 0x8f1bbcdc + W(i); b = ROTLEFT(b, 30)
#define R4(a, b, c, d, e, i) e += ROTLEFT(a, 5) + F1(b, c, d) + 0xca62c1d6 + W(i); b = ROTLEFT(b, 30)

// SHA-1 核心变换：依次压缩 nblk 个 64 字节块并更新状态
static void _sha1_transform(sha1_ctx *sha1, const uint8_t *data, size_t nblk) {
    uint32_t a, b, c, d, e, i, m[16];
    for (; nblk > 0; --nblk, data += 64) {
        for (i = 0; i < 16; ++i) {
            m[i] = _crypt_read32be(data + i * 4);
        }
        a = sha1->state[0];
        b = sha1->state[1];
        c = sha1->state[2];
        d = sha1->state[3];
        e = sha1->state[4];
        R0(a, b, c, d, e, 0); R0(e, a, b, c, d, 1); R0(d, e, a, b, c, 2); R0(c, d, e, a, b, 3); R0(b, c, d, e, a, 4);
        R0(a, b, c, d, e, 5); R0(e, a, b, c, d, 6); R0(d, e, a, b, c, 7); R0(c, d, e, a, b, 8); R0(b, c, d, e, a, 9);
        R0(a, b, c, d, e, 10); R0(e, a, b, c, d, 11); R0(d, e, a, b, c, 12); R0(c, d, e, a, b, 13); R0(b, c, d, e, a, 14);
        R0(a, b, c, d, e, 15); R1(e, a, b, c, d, 16); R1(d, e, a, b, c, 17); R1(c, d, e, a, b, 18); R1(b, c, d, e, a, 19);
        R2(a, b, c, d, e, 20); R2(e, a, b, c, d, 21); R2(d, e, a, b, c, 22); R2(c, d, e, a, b, 23); R2(b, c, d, e, a, 24);
        R2(a, b, c, d, e, 25); R2(e, a, b, c, d, 26); R2(d, e, a, b, c, 27); R2(c, d, e, a, b, 28); R2(b, c, d, e, a, 29);
        R2(a, b, c, d, e, 30); R2(e, a, b, c, d, 31); R2(d, e, a, b, c, 32); R2(c, d, e, a, b, 33); R2(b, c, d, e, a, 34);
        R2(a, b, c, d, e, 35); R2(e, a, b, c, d, 36); R2(d, e, a, b, c, 37); R2(c, d, e, a, b, 38); R2(b, c, d, e, a, 39);
        R3(a, b, c, d, e, 40); R3(e, a, b, c, d, 41); R3(d, e, a, b, c, 42); R3(c, d, e, a, b, 43); R3(b, c, d, e, a, 44);
        R3(a, b, c, d, e, 45); R3(e, a, b, c, d, 46); R3(d, e, a, b, c, 47); R3(c, d, e, a, b, 48); R3(b, c, d, e, a, 49);
        R3(a, b, c, d, e, 50); R3(e, a, b, c, d, 51); R3(d, e, a, b, c, 52); R3(c, d, e, a, b, 53); R3(b, c, d, e, a, 54);
        R3(a, b, c, d, e, 55); R3(e, a, b, c, d, 56); R3(d, e, a, b, c, 57); R3(c, d, e, a, b, 58); R3(b, c, d, e, a, 59);
        R4(a, b, c, d, e, 60); R4(e, a, b, c, d, 61); R4(d, e, a, b, c, 62); R4(c, d, e, a, b, 63); R4(b, c, d, e, a, 64);
        R4(a, b, c, d, e, 65); R4(e, a, b, c, d, 66); R4(d, e, a, b, c, 67); R4(c, d, e, a, b, 68); R4(b, c, d, e, a, 69);
        R4(a, b, c, d, e, 70); R4(e, a, b, c, d, 71); R4(d, e, a, b, c, 72); R4(c, d, e, a, b, 73); R4(b, c, d, e, a, 74);
        R4(a, b, c, d, e, 75); R4(e, a, b, c, d, 76); R4(d, e, a, b, c, 77); R4(c, d, e, a, b, 78); R4(b, c, d, e, a, 79);
        // 先擦调度表再累加状态，顺序别颠倒：颠倒后首块之后的中间状态会残留在栈上
        secure_zero(m, sizeof(m));
        sha1->state[0] += a;
        sha1->state[1] += b;
        sha1->state[2] += c;
        sha1->state[3] += d;
        sha1->state[4] += e;
    }
}
void sha1_init(sha1_ctx *sha1) {
    sha1->datalen = 0;
    sha1->bitlen = 0;
    sha1->state[0] = 0x67452301;
    sha1->state[1] = 0xefcdab89;
    sha1->state[2] = 0x98badcfe;
    sha1->state[3] = 0x10325476;
    sha1->state[4] = 0xc3d2e1f0;
}
void sha1_update(sha1_ctx *sha1, const void *data, size_t lens) {
    const uint8_t *p = (const uint8_t *)data;
    size_t left;
    if (0 == lens) {
        return;
    }
    left = 64 - sha1->datalen;
    if (sha1->datalen > 0 && lens >= left) {
        memcpy(sha1->data + sha1->datalen, p, left);
        _sha1_transform(sha1, sha1->data, 1);
        sha1->bitlen += 512;
        sha1->datalen = 0;
        p += left;
        lens -= left;
    }
    if (lens >= 64) {
        _sha1_transform(sha1, p, lens / 64);
        sha1->bitlen += (uint64_t)(lens / 64) * 512;
        p += lens & ~(size_t)63;
        lens &= 63;
    }
    if (lens > 0) {
        memcpy(sha1->data + sha1->datalen, p, lens);
        sha1->datalen += (uint32_t)lens;
    }
}
void sha1_final(sha1_ctx *sha1, char hash[SHA1_BLOCK_SIZE]) {
    uint32_t i = sha1->datalen;
    if (sha1->datalen < 56) {
        sha1->data[i++] = 0x80;
        while (i < 56) {
            sha1->data[i++] = 0x00;
        }
    } else {
        sha1->data[i++] = 0x80;
        while (i < 64) {
            sha1->data[i++] = 0x00;
        }
        _sha1_transform(sha1, sha1->data, 1);
        memset(sha1->data, 0, 56);
    }
    sha1->bitlen += sha1->datalen * 8;
    _crypt_write32be(sha1->data + 56, (uint32_t)(sha1->bitlen >> 32));
    _crypt_write32be(sha1->data + 60, (uint32_t)sha1->bitlen);
    _sha1_transform(sha1, sha1->data, 1);
    for (i = 0; i < 5; ++i) {
        _crypt_write32be((uint8_t *)hash + i * 4, sha1->state[i]);
    }
    secure_zero(sha1, sizeof(sha1_ctx));
}
