#include "crypt/md5.h"

#define ROTLEFT(a,b) ((a << b) | (a >> (32-b)))
#define F(x,y,z) ((x & y) | (~x & z))
#define G(x,y,z) ((x & z) | (y & ~z))
#define H(x,y,z) (x ^ y ^ z)
#define I(x,y,z) (y ^ (x | ~z))
#define FF(a,b,c,d,m,s,t) { a += F(b,c,d) + m + t; \
                            a = b + ROTLEFT(a,s); }
#define GG(a,b,c,d,m,s,t) { a += G(b,c,d) + m + t; \
                            a = b + ROTLEFT(a,s); }
#define HH(a,b,c,d,m,s,t) { a += H(b,c,d) + m + t; \
                            a = b + ROTLEFT(a,s); }
#define II(a,b,c,d,m,s,t) { a += I(b,c,d) + m + t; \
                            a = b + ROTLEFT(a,s); }

static void _transform(md5_ctx *md5, const uint8_t *data) {
    uint32_t a, b, c, d, m[16], i, j;
    for (i = 0, j = 0; i < 16; ++i, j += 4) {
        m[i] = ((uint32_t)data[j]) | ((uint32_t)data[j + 1] << 8) | ((uint32_t)data[j + 2] << 16) | ((uint32_t)data[j + 3] << 24);
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
    md5->state[0] += a;
    md5->state[1] += b;
    md5->state[2] += c;
    md5->state[3] += d;
    ZERO(m, sizeof(m));
}
void md5_init(md5_ctx *md5) {
    md5->datalen = 0;
    md5->bitlen = 0;
    md5->state[0] = 0x67452301;
    md5->state[1] = 0xEFCDAB89;
    md5->state[2] = 0x98BADCFE;
    md5->state[3] = 0x10325476;
}
void md5_update(md5_ctx *md5, const void *data, size_t lens) {
    uint8_t *p = (uint8_t *)data;
    for (size_t i = 0; i < lens; ++i) {
        md5->data[md5->datalen] = p[i];
        md5->datalen++;
        if (64 == md5->datalen) {
            _transform(md5, md5->data);
            md5->bitlen += 512;
            md5->datalen = 0;
        }
    }
}
void md5_final(md5_ctx *md5, char hash[MD5_BLOCK_SIZE]) {
    size_t i = md5->datalen;
    if (md5->datalen < 56) {
        md5->data[i++] = 0x80;
        while (i < 56) {
            md5->data[i++] = 0x00;
        }
    } else if (md5->datalen >= 56) {
        md5->data[i++] = 0x80;
        while (i < 64) {
            md5->data[i++] = 0x00;
        }
        _transform(md5, md5->data);
        memset(md5->data, 0, 56);
    }
    md5->bitlen += md5->datalen * 8;
    md5->data[56] = (uint8_t)(md5->bitlen);
    md5->data[57] = (uint8_t)(md5->bitlen >> 8);
    md5->data[58] = (uint8_t)(md5->bitlen >> 16);
    md5->data[59] = (uint8_t)(md5->bitlen >> 24);
    md5->data[60] = (uint8_t)(md5->bitlen >> 32);
    md5->data[61] = (uint8_t)(md5->bitlen >> 40);
    md5->data[62] = (uint8_t)(md5->bitlen >> 48);
    md5->data[63] = (uint8_t)(md5->bitlen >> 56);
    _transform(md5, md5->data);
    for (i = 0; i < 4; ++i) {
        hash[i] = (md5->state[0] >> (i * 8)) & 0xff;
        hash[i + 4] = (md5->state[1] >> (i * 8)) & 0xff;
        hash[i + 8] = (md5->state[2] >> (i * 8)) & 0xff;
        hash[i + 12] = (md5->state[3] >> (i * 8)) & 0xff;
    }
    ZERO(md5, sizeof(md5_ctx));
}
