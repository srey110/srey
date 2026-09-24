#include "crypt/sha512.h"
#include "crypt/crypt_pub.h"

#define SHA512_BLOCK_LENGTH 128 // SHA-512 输入块长度（字节）
#define SHA512_SHORT_BLOCK_LENGTH (SHA512_BLOCK_LENGTH - 16) // 末尾块长度阈值（留出 128 位存放长度）
// 128 位计数器加法。n 存局部再用:直接展开会求值两次,带副作用的实参会让进位判错
#define ADDINC128(w,n) do { \
    uint64_t _addinc = (uint64_t)(n); \
    (w)[0] += _addinc; \
    if ((w)[0] < _addinc) { \
        (w)[1]++; \
    } \
} while (0)
#define R(b,x) ((x) >> (b)) // 逻辑右移
#define S64(b,x) (((x) >> (b)) | ((x) << (64 - (b)))) // 64 位循环右移
#define Ch(x,y,z) (((x) & (y)) ^ ((~(x)) & (z))) // 选择函数
#define Maj(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z))) // 多数函数
#define Sigma0_512(x) (S64(28, (x)) ^ S64(34, (x)) ^ S64(39, (x))) // 大 Σ0 函数
#define Sigma1_512(x) (S64(14, (x)) ^ S64(18, (x)) ^ S64(41, (x))) // 大 Σ1 函数
#define sigma0_512(x) (S64( 1, (x)) ^ S64( 8, (x)) ^ R( 7,   (x))) // 小 σ0 函数
#define sigma1_512(x) (S64(19, (x)) ^ S64(61, (x)) ^ R( 6,   (x))) // 小 σ1 函数
// 按大端取 64 位字，不要求对齐
#if defined(OS_WIN)
#define LOAD64BE(p) _crypt_read64be(p)// Windows 下走公共头的读函数
#else
#define LOAD64BE(p) (((uint64_t)(p)[0] << 56) | ((uint64_t)(p)[1] << 48) | ((uint64_t)(p)[2] << 40) | ((uint64_t)(p)[3] << 32) | \
    ((uint64_t)(p)[4] << 24) | ((uint64_t)(p)[5] << 16) | ((uint64_t)(p)[6] << 8) | ((uint64_t)(p)[7]))
#endif
// 一轮压缩：进来时 t1 已是本轮消息字。a~h 不搬动，靠调用方每轮把名字轮换一位
#define ROUND512(i,a,b,c,d,e,f,g,h) do { \
    t1 += (h) + Sigma1_512(e) + Ch(e, f, g) + k512[i]; \
    (h) = Sigma0_512(a) + Maj(a, b, c); \
    (d) += t1; \
    (h) += t1; \
} while (0)
// 前 16 轮：消息字直接取自输入
#define ROUND512_00_15(i,a,b,c,d,e,f,g,h) do { \
    t1 = w[i] = LOAD64BE(data + (i) * 8); \
    ROUND512(i, a, b, c, d, e, f, g, h); \
} while (0)
// 后 64 轮：调度表只留最近 16 个字循环复用
#define ROUND512_16_79(i,a,b,c,d,e,f,g,h) do { \
    t1 = w[(i) & 15] += sigma0_512(w[((i) + 1) & 15]) + sigma1_512(w[((i) + 14) & 15]) + w[((i) + 9) & 15]; \
    ROUND512(i, a, b, c, d, e, f, g, h); \
} while (0)

static const uint64_t k512[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL,
    0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
    0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL,
    0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL,
    0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
    0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL,
    0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL,
    0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL,
    0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL,
    0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL,
    0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL,
    0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL,
    0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL,
    0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL,
    0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL,
    0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL,
    0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL,
    0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL,
    0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL,
    0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
};
static const uint64_t ihv[8] = {
    0x6a09e667f3bcc908ULL,
    0xbb67ae8584caa73bULL,
    0x3c6ef372fe94f82bULL,
    0xa54ff53a5f1d36f1ULL,
    0x510e527fade682d1ULL,
    0x9b05688c2b3e6c1fULL,
    0x1f83d9abfb41bd6bULL,
    0x5be0cd19137e2179ULL
};
void sha512_init(sha512_ctx *sha512) {
    memcpy(sha512->state, ihv, SHA512_BLOCK_SIZE);
    ZERO(sha512->data, SHA512_BLOCK_LENGTH);
    sha512->bitlen[0] = sha512->bitlen[1] = 0;
}
// SHA-512 核心变换：连续处理 blocks 个 128 字节块
static void _sha512_transform(uint64_t state[8], const uint8_t *data, size_t blocks) {
    uint64_t a, b, c, d, e, f, g, h, t1, w[16];
    size_t i;
    for (; blocks > 0; --blocks, data += SHA512_BLOCK_LENGTH) {
        a = state[0];
        b = state[1];
        c = state[2];
        d = state[3];
        e = state[4];
        f = state[5];
        g = state[6];
        h = state[7];
        CRYPT_ROUNDS8(ROUND512_00_15, 0);
        CRYPT_ROUNDS8(ROUND512_00_15, 8);
        for (i = 16; i < 80; i += 16) {
            CRYPT_ROUNDS8(ROUND512_16_79, i);
            CRYPT_ROUNDS8(ROUND512_16_79, i + 8);
        }
        // 先擦调度表再累加状态，理由同 sha1.c
        secure_zero(w, sizeof(w));
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
void sha512_update(sha512_ctx *sha512, const void *data, size_t lens) {
    if (0 == lens) {
        return;
    }
    const uint8_t *p = (const uint8_t *)data;
    size_t blocks;
    size_t usedspace = (sha512->bitlen[0] >> 3) % SHA512_BLOCK_LENGTH;
    if (usedspace > 0) {
        size_t freespace = SHA512_BLOCK_LENGTH - usedspace;
        if (lens >= freespace) {
            memcpy(&sha512->data[usedspace], p, freespace);
            ADDINC128(sha512->bitlen, freespace << 3);
            lens -= freespace;
            p += freespace;
            _sha512_transform(sha512->state, sha512->data, 1);
        } else {
            memcpy(&sha512->data[usedspace], p, lens);
            ADDINC128(sha512->bitlen, lens << 3);
            usedspace = freespace = 0;
            return;
        }
    }
    if (lens >= SHA512_BLOCK_LENGTH) {
        blocks = lens / SHA512_BLOCK_LENGTH;
        _sha512_transform(sha512->state, p, blocks);
        ADDINC128(sha512->bitlen, (uint64_t)blocks << 10);
        lens -= blocks * SHA512_BLOCK_LENGTH;
        p += blocks * SHA512_BLOCK_LENGTH;
    }
    if (lens > 0) {
        memcpy(sha512->data, p, lens);
        ADDINC128(sha512->bitlen, lens << 3);
    }
}
// 处理末尾块：填充消息并附加总长度，然后执行最后一次变换
static void _sha512_last(sha512_ctx *sha512) {
    size_t usedspace = (sha512->bitlen[0] >> 3) % SHA512_BLOCK_LENGTH;
    if (usedspace > 0) {
        sha512->data[usedspace++] = 0x80;
        if (usedspace <= SHA512_SHORT_BLOCK_LENGTH) {
            ZERO(&sha512->data[usedspace], SHA512_SHORT_BLOCK_LENGTH - usedspace);
        } else {
            if (usedspace < SHA512_BLOCK_LENGTH) {
                ZERO(&sha512->data[usedspace], SHA512_BLOCK_LENGTH - usedspace);
            }
            _sha512_transform(sha512->state, sha512->data, 1);
            ZERO(sha512->data, SHA512_SHORT_BLOCK_LENGTH);
        }
    } else {
        ZERO(sha512->data, SHA512_SHORT_BLOCK_LENGTH);
        sha512->data[0] = 0x80;
    }
    // 128 位长度按大端放在末 16 字节：高 64 位在前
    _crypt_write64be(sha512->data + SHA512_SHORT_BLOCK_LENGTH, sha512->bitlen[1]);
    _crypt_write64be(sha512->data + SHA512_SHORT_BLOCK_LENGTH + 8, sha512->bitlen[0]);
    _sha512_transform(sha512->state, sha512->data, 1);
}
void sha512_final(sha512_ctx *sha512, char hash[SHA512_BLOCK_SIZE]) {
    size_t j;
    _sha512_last(sha512);
    for (j = 0; j < 8; j++) {
        _crypt_write64be(hash + j * 8, sha512->state[j]);
    }
    secure_zero(sha512, sizeof(sha512_ctx));
}
