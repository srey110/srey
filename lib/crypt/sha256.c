#include "crypt/sha256.h"
#include "crypt/crypt_pub.h"

#define ROTRIGHT(a,b) (((a) >> (b)) | ((a) << (32-(b))))
#define CH(x,y,z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTRIGHT(x,2) ^ ROTRIGHT(x,13) ^ ROTRIGHT(x,22))
#define EP1(x) (ROTRIGHT(x,6) ^ ROTRIGHT(x,11) ^ ROTRIGHT(x,25))
#define SIG0(x) (ROTRIGHT(x,7) ^ ROTRIGHT(x,18) ^ ((x) >> 3))
#define SIG1(x) (ROTRIGHT(x,17) ^ ROTRIGHT(x,19) ^ ((x) >> 10))
// 按大端取 32 位字，不要求对齐
#if defined(OS_WIN)
#define LOAD32BE(p) _crypt_read32be(p)// Windows 下走公共头的读函数
#else
#define LOAD32BE(p) (((uint32_t)(p)[0] << 24) | ((uint32_t)(p)[1] << 16) | ((uint32_t)(p)[2] << 8) | ((uint32_t)(p)[3]))
#endif
// 一轮压缩：进来时 t1 已是本轮消息字。a~h 不搬动，靠调用方每轮把名字轮换一位
#define ROUND(i,a,b,c,d,e,f,g,h) do { \
    t1 += (h) + EP1(e) + CH(e, f, g) + k[i]; \
    (h) = EP0(a) + MAJ(a, b, c); \
    (d) += t1; \
    (h) += t1; \
} while (0)
// 前 16 轮：消息字直接取自输入
#define ROUND_00_15(i,a,b,c,d,e,f,g,h) do { \
    t1 = m[i] = LOAD32BE(data + (i) * 4); \
    ROUND(i, a, b, c, d, e, f, g, h); \
} while (0)
// 后 48 轮：调度表只留最近 16 个字循环复用
#define ROUND_16_63(i,a,b,c,d,e,f,g,h) do { \
    t1 = m[(i) & 15] += SIG0(m[((i) + 1) & 15]) + SIG1(m[((i) + 14) & 15]) + m[((i) + 9) & 15]; \
    ROUND(i, a, b, c, d, e, f, g, h); \
} while (0)

static const uint32_t k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
// SHA-256 核心变换：连续处理 blocks 个 64 字节块
static void _sha256_transform(uint32_t state[8], const uint8_t *data, size_t blocks) {
    uint32_t a, b, c, d, e, f, g, h, t1, m[16];
    size_t i;
    for (; blocks > 0; --blocks, data += 64) {
        a = state[0];
        b = state[1];
        c = state[2];
        d = state[3];
        e = state[4];
        f = state[5];
        g = state[6];
        h = state[7];
        for (i = 0; i < 16; i += 8) {
            CRYPT_ROUNDS8(ROUND_00_15, i);
        }
        for (; i < 64; i += 8) {
            CRYPT_ROUNDS8(ROUND_16_63, i);
        }
        // 先擦调度表再累加状态，理由同 sha1.c
        secure_zero(m, sizeof(m));
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    }
}
void sha256_init(sha256_ctx *sha256) {
    sha256->datalen = 0;
    sha256->bitlen = 0;
    sha256->state[0] = 0x6a09e667;
    sha256->state[1] = 0xbb67ae85;
    sha256->state[2] = 0x3c6ef372;
    sha256->state[3] = 0xa54ff53a;
    sha256->state[4] = 0x510e527f;
    sha256->state[5] = 0x9b05688c;
    sha256->state[6] = 0x1f83d9ab;
    sha256->state[7] = 0x5be0cd19;
}
void sha256_update(sha256_ctx *sha256, const void *data, size_t lens) {
    const uint8_t *p = (const uint8_t *)data;
    size_t left, blocks;
    if (0 == lens) {
        return;
    }
    left = 64 - sha256->datalen;
    if (sha256->datalen > 0 && lens >= left) {
        memcpy(sha256->data + sha256->datalen, p, left);
        _sha256_transform(sha256->state, sha256->data, 1);
        sha256->bitlen += 512;
        sha256->datalen = 0;
        p += left;
        lens -= left;
    }
    if (lens >= 64) {
        blocks = lens / 64;
        _sha256_transform(sha256->state, p, blocks);
        sha256->bitlen += (uint64_t)blocks * 512;
        p += blocks * 64;
        lens -= blocks * 64;
    }
    if (lens > 0) {
        memcpy(sha256->data + sha256->datalen, p, lens);
        sha256->datalen += (uint32_t)lens;
    }
}
void sha256_final(sha256_ctx *sha256, char hash[SHA256_BLOCK_SIZE]) {
    uint32_t i;
    i = sha256->datalen;
    if (sha256->datalen < 56) {
        sha256->data[i++] = 0x80;
        while (i < 56) {
            sha256->data[i++] = 0x00;
        }
    } else {
        sha256->data[i++] = 0x80;
        while (i < 64) {
            sha256->data[i++] = 0x00;
        }
        _sha256_transform(sha256->state, sha256->data, 1);
        memset(sha256->data, 0, 56);
    }
    sha256->bitlen += sha256->datalen * 8;
    _crypt_write64be(sha256->data + 56, sha256->bitlen);
    _sha256_transform(sha256->state, sha256->data, 1);
    for (i = 0; i < 8; ++i) {
        _crypt_write32be(hash + i * 4, sha256->state[i]);
    }
    secure_zero(sha256, sizeof(sha256_ctx));
}
