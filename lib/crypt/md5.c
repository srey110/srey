#include "crypt/md5.h"
#include "crypt/crypt_pub.h"

#define ROTLEFT(a,b) (((a) << (b)) | ((a) >> (32-(b))))
#define F(x,y,z) ((x & y) | (~x & z))
#define H(x,y,z) (x ^ y ^ z)
#define I(x,y,z) (y ^ (x | ~z))
// 每步先加与本步新值无关的项，最后才加依赖 b 的那项
#define FF(a,b,c,d,m,s,t) { a += (m) + (t); CRYPT_OPAQUE(a); a += F(b,c,d); \
                            a = b + ROTLEFT(a,s); }
// G = (b & d) | (c & ~d)，两半不相交，按加法拆开结果不变
#define GG(a,b,c,d,m,s,t) { a += (m) + (t) + ((c) & ~(d)); CRYPT_OPAQUE(a); a += (b) & (d); \
                            a = b + ROTLEFT(a,s); }
#define HH(a,b,c,d,m,s,t) { a += (m) + (t); CRYPT_OPAQUE(a); a += H(b,c,d); \
                            a = b + ROTLEFT(a,s); }
#define II(a,b,c,d,m,s,t) { a += (m) + (t); CRYPT_OPAQUE(a); a += I(b,c,d); \
                            a = b + ROTLEFT(a,s); }

// MD5 核心变换：依次压缩 nblk 个 64 字节块并更新状态
static void _md5_transform(md5_ctx *md5, const uint8_t *data, size_t nblk) {
    uint32_t a, b, c, d, m[16], i;
    for (; nblk > 0; --nblk, data += 64) {
        for (i = 0; i < 16; ++i) {
            m[i] = read_le32(data + i * 4);
        }
        a = md5->state[0];
        b = md5->state[1];
        c = md5->state[2];
        d = md5->state[3];
        FF(a, b, c, d, m[0], 7, 0xd76aa478);
        FF(d, a, b, c, m[1], 12, 0xe8c7b756);
        FF(c, d, a, b, m[2], 17, 0x242070db);
        FF(b, c, d, a, m[3], 22, 0xc1bdceee);
        FF(a, b, c, d, m[4], 7, 0xf57c0faf);
        FF(d, a, b, c, m[5], 12, 0x4787c62a);
        FF(c, d, a, b, m[6], 17, 0xa8304613);
        FF(b, c, d, a, m[7], 22, 0xfd469501);
        FF(a, b, c, d, m[8], 7, 0x698098d8);
        FF(d, a, b, c, m[9], 12, 0x8b44f7af);
        FF(c, d, a, b, m[10], 17, 0xffff5bb1);
        FF(b, c, d, a, m[11], 22, 0x895cd7be);
        FF(a, b, c, d, m[12], 7, 0x6b901122);
        FF(d, a, b, c, m[13], 12, 0xfd987193);
        FF(c, d, a, b, m[14], 17, 0xa679438e);
        FF(b, c, d, a, m[15], 22, 0x49b40821);
        GG(a, b, c, d, m[1], 5, 0xf61e2562);
        GG(d, a, b, c, m[6], 9, 0xc040b340);
        GG(c, d, a, b, m[11], 14, 0x265e5a51);
        GG(b, c, d, a, m[0], 20, 0xe9b6c7aa);
        GG(a, b, c, d, m[5], 5, 0xd62f105d);
        GG(d, a, b, c, m[10], 9, 0x02441453);
        GG(c, d, a, b, m[15], 14, 0xd8a1e681);
        GG(b, c, d, a, m[4], 20, 0xe7d3fbc8);
        GG(a, b, c, d, m[9], 5, 0x21e1cde6);
        GG(d, a, b, c, m[14], 9, 0xc33707d6);
        GG(c, d, a, b, m[3], 14, 0xf4d50d87);
        GG(b, c, d, a, m[8], 20, 0x455a14ed);
        GG(a, b, c, d, m[13], 5, 0xa9e3e905);
        GG(d, a, b, c, m[2], 9, 0xfcefa3f8);
        GG(c, d, a, b, m[7], 14, 0x676f02d9);
        GG(b, c, d, a, m[12], 20, 0x8d2a4c8a);
        HH(a, b, c, d, m[5], 4, 0xfffa3942);
        HH(d, a, b, c, m[8], 11, 0x8771f681);
        HH(c, d, a, b, m[11], 16, 0x6d9d6122);
        HH(b, c, d, a, m[14], 23, 0xfde5380c);
        HH(a, b, c, d, m[1], 4, 0xa4beea44);
        HH(d, a, b, c, m[4], 11, 0x4bdecfa9);
        HH(c, d, a, b, m[7], 16, 0xf6bb4b60);
        HH(b, c, d, a, m[10], 23, 0xbebfbc70);
        HH(a, b, c, d, m[13], 4, 0x289b7ec6);
        HH(d, a, b, c, m[0], 11, 0xeaa127fa);
        HH(c, d, a, b, m[3], 16, 0xd4ef3085);
        HH(b, c, d, a, m[6], 23, 0x04881d05);
        HH(a, b, c, d, m[9], 4, 0xd9d4d039);
        HH(d, a, b, c, m[12], 11, 0xe6db99e5);
        HH(c, d, a, b, m[15], 16, 0x1fa27cf8);
        HH(b, c, d, a, m[2], 23, 0xc4ac5665);
        II(a, b, c, d, m[0], 6, 0xf4292244);
        II(d, a, b, c, m[7], 10, 0x432aff97);
        II(c, d, a, b, m[14], 15, 0xab9423a7);
        II(b, c, d, a, m[5], 21, 0xfc93a039);
        II(a, b, c, d, m[12], 6, 0x655b59c3);
        II(d, a, b, c, m[3], 10, 0x8f0ccc92);
        II(c, d, a, b, m[10], 15, 0xffeff47d);
        II(b, c, d, a, m[1], 21, 0x85845dd1);
        II(a, b, c, d, m[8], 6, 0x6fa87e4f);
        II(d, a, b, c, m[15], 10, 0xfe2ce6e0);
        II(c, d, a, b, m[6], 15, 0xa3014314);
        II(b, c, d, a, m[13], 21, 0x4e0811a1);
        II(a, b, c, d, m[4], 6, 0xf7537e82);
        II(d, a, b, c, m[11], 10, 0xbd3af235);
        II(c, d, a, b, m[2], 15, 0x2ad7d2bb);
        II(b, c, d, a, m[9], 21, 0xeb86d391);
        // 先擦调度表再累加状态，理由同 sha1.c
        secure_zero(m, sizeof(m));
        md5->state[0] += a;
        md5->state[1] += b;
        md5->state[2] += c;
        md5->state[3] += d;
    }
}
void md5_init(md5_ctx *md5) {
    md5->datalen = 0;
    md5->bitlen = 0;
    md5->state[0] = 0x67452301;
    md5->state[1] = 0xefcdab89;
    md5->state[2] = 0x98badcfe;
    md5->state[3] = 0x10325476;
}
void md5_update(md5_ctx *md5, const void *data, size_t lens) {
    const uint8_t *p = (const uint8_t *)data;
    size_t left;
    if (0 == lens) {
        return;
    }
    left = 64 - md5->datalen;
    if (md5->datalen > 0 && lens >= left) {
        memcpy(md5->data + md5->datalen, p, left);
        _md5_transform(md5, md5->data, 1);
        md5->bitlen += 512;
        md5->datalen = 0;
        p += left;
        lens -= left;
    }
    if (lens >= 64) {
        _md5_transform(md5, p, lens / 64);
        md5->bitlen += (uint64_t)(lens / 64) * 512;
        p += lens & ~(size_t)63;
        lens &= 63;
    }
    if (lens > 0) {
        memcpy(md5->data + md5->datalen, p, lens);
        md5->datalen += (uint32_t)lens;
    }
}
void md5_final(md5_ctx *md5, char hash[MD5_BLOCK_SIZE]) {
    size_t i = md5->datalen;
    md5->data[i++] = 0x80;
    if (i > 56) {
        memset(md5->data + i, 0, 64 - i);
        _md5_transform(md5, md5->data, 1);
        i = 0;
    }
    memset(md5->data + i, 0, 56 - i);
    md5->bitlen += md5->datalen * 8;
    write_le32(md5->data + 56, (uint32_t)md5->bitlen);
    write_le32(md5->data + 60, (uint32_t)(md5->bitlen >> 32));
    _md5_transform(md5, md5->data, 1);
    for (i = 0; i < 4; ++i) {
        write_le32((uint8_t *)hash + i * 4, md5->state[i]);
    }
    secure_zero(md5, sizeof(md5_ctx));
}
