#include "crypt/sha1.h"

#define ROTLEFT(a, b) ((a << b) | (a >> (32 - b)))

static void _transform(sha1_ctx *sha1, const unsigned char *data) {
    uint32_t a, b, c, d, e, i, j, t, m[80];
    for (i = 0, j = 0; i < 16; ++i, j += 4) {
        m[i] = ((uint32_t)data[j] << 24) | ((uint32_t)data[j + 1] << 16) | ((uint32_t)data[j + 2] << 8) | ((uint32_t)data[j + 3]);
    }
    for (; i < 80; ++i) {
        m[i] = (m[i - 3] ^ m[i - 8] ^ m[i - 14] ^ m[i - 16]);
        m[i] = (m[i] << 1) | (m[i] >> 31);
    }
    a = sha1->state[0];
    b = sha1->state[1];
    c = sha1->state[2];
    d = sha1->state[3];
    e = sha1->state[4];
    for (i = 0; i < 20; ++i) {
        t = ROTLEFT(a, 5) + ((b & c) ^ (~b & d)) + e + sha1->k[0] + m[i];
        e = d;
        d = c;
        c = ROTLEFT(b, 30);
        b = a;
        a = t;
    }
    for (; i < 40; ++i) {
        t = ROTLEFT(a, 5) + (b ^ c ^ d) + e + sha1->k[1] + m[i];
        e = d;
        d = c;
        c = ROTLEFT(b, 30);
        b = a;
        a = t;
    }
    for (; i < 60; ++i) {
        t = ROTLEFT(a, 5) + ((b & c) ^ (b & d) ^ (c & d)) + e + sha1->k[2] + m[i];
        e = d;
        d = c;
        c = ROTLEFT(b, 30);
        b = a;
        a = t;
    }
    for (; i < 80; ++i) {
        t = ROTLEFT(a, 5) + (b ^ c ^ d) + e + sha1->k[3] + m[i];
        e = d;
        d = c;
        c = ROTLEFT(b, 30);
        b = a;
        a = t;
    }
    sha1->state[0] += a;
    sha1->state[1] += b;
    sha1->state[2] += c;
    sha1->state[3] += d;
    sha1->state[4] += e;
    ZERO(m, sizeof(m));
}
void sha1_init(sha1_ctx *sha1) {
    sha1->datalen = 0;
    sha1->bitlen = 0;
    sha1->state[0] = 0x67452301;
    sha1->state[1] = 0xEFCDAB89;
    sha1->state[2] = 0x98BADCFE;
    sha1->state[3] = 0x10325476;
    sha1->state[4] = 0xc3d2e1f0;
    sha1->k[0] = 0x5a827999;
    sha1->k[1] = 0x6ed9eba1;
    sha1->k[2] = 0x8f1bbcdc;
    sha1->k[3] = 0xca62c1d6;
}
void sha1_update(sha1_ctx *sha1, const void *data, size_t lens) {
    unsigned char *p = (unsigned char *)data;
    for (size_t i = 0; i < lens; ++i) {
        sha1->data[sha1->datalen] = p[i];
        sha1->datalen++;
        if (64 == sha1->datalen) {
            _transform(sha1, sha1->data);
            sha1->bitlen += 512;
            sha1->datalen = 0;
        }
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
        _transform(sha1, sha1->data);
        memset(sha1->data, 0, 56);
    }
    sha1->bitlen += sha1->datalen * 8;
    sha1->data[63] = (unsigned char)(sha1->bitlen);
    sha1->data[62] = (unsigned char)(sha1->bitlen >> 8);
    sha1->data[61] = (unsigned char)(sha1->bitlen >> 16);
    sha1->data[60] = (unsigned char)(sha1->bitlen >> 24);
    sha1->data[59] = (unsigned char)(sha1->bitlen >> 32);
    sha1->data[58] = (unsigned char)(sha1->bitlen >> 40);
    sha1->data[57] = (unsigned char)(sha1->bitlen >> 48);
    sha1->data[56] = (unsigned char)(sha1->bitlen >> 56);
    _transform(sha1, sha1->data);
    for (i = 0; i < 4; ++i) {
        hash[i] = (sha1->state[0] >> (24 - i * 8)) & 0xff;
        hash[i + 4] = (sha1->state[1] >> (24 - i * 8)) & 0xff;
        hash[i + 8] = (sha1->state[2] >> (24 - i * 8)) & 0xff;
        hash[i + 12] = (sha1->state[3] >> (24 - i * 8)) & 0xff;
        hash[i + 16] = (sha1->state[4] >> (24 - i * 8)) & 0xff;
    }
    ZERO(sha1, sizeof(sha1_ctx));
}
