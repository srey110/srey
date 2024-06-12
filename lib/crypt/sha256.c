#include "crypt/sha256.h"

#define ROTLEFT(a,b) (((a) << (b)) | ((a) >> (32-(b))))
#define ROTRIGHT(a,b) (((a) >> (b)) | ((a) << (32-(b))))
#define CH(x,y,z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTRIGHT(x,2) ^ ROTRIGHT(x,13) ^ ROTRIGHT(x,22))
#define EP1(x) (ROTRIGHT(x,6) ^ ROTRIGHT(x,11) ^ ROTRIGHT(x,25))
#define SIG0(x) (ROTRIGHT(x,7) ^ ROTRIGHT(x,18) ^ ((x) >> 3))
#define SIG1(x) (ROTRIGHT(x,17) ^ ROTRIGHT(x,19) ^ ((x) >> 10))
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
static void _transform(sha256_ctx *sha256, const unsigned char *data) {
    uint32_t a, b, c, d, e, f, g, h, i, j, t1, t2, m[64];
    for (i = 0, j = 0; i < 16; ++i, j += 4) {
        m[i] = ((uint32_t)data[j] << 24) | ((uint32_t)data[j + 1] << 16) | ((uint32_t)data[j + 2] << 8) | ((uint32_t)data[j + 3]);
    }
    for (; i < 64; ++i) {
        m[i] = SIG1(m[i - 2]) + m[i - 7] + SIG0(m[i - 15]) + m[i - 16];
    }
    a = sha256->state[0];
    b = sha256->state[1];
    c = sha256->state[2];
    d = sha256->state[3];
    e = sha256->state[4];
    f = sha256->state[5];
    g = sha256->state[6];
    h = sha256->state[7];
    for (i = 0; i < 64; ++i) {
        t1 = h + EP1(e) + CH(e, f, g) + k[i] + m[i];
        t2 = EP0(a) + MAJ(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    sha256->state[0] += a;
    sha256->state[1] += b;
    sha256->state[2] += c;
    sha256->state[3] += d;
    sha256->state[4] += e;
    sha256->state[5] += f;
    sha256->state[6] += g;
    sha256->state[7] += h;
    ZERO(m, sizeof(m));
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
void sha256_update(sha256_ctx *sha256, const unsigned char *data, size_t lens) {
    for (size_t i = 0; i < lens; ++i) {
        sha256->data[sha256->datalen] = data[i];
        sha256->datalen++;
        if (64 == sha256->datalen) {
            _transform(sha256, sha256->data);
            sha256->bitlen += 512;
            sha256->datalen = 0;
        }
    }
}
void sha256_final(sha256_ctx *sha256, unsigned char hash[SHA256_BLOCK_SIZE]) {
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
        _transform(sha256, sha256->data);
        memset(sha256->data, 0, 56);
    }
    sha256->bitlen += sha256->datalen * 8;
    sha256->data[63] = (unsigned char)(sha256->bitlen);
    sha256->data[62] = (unsigned char)(sha256->bitlen >> 8);
    sha256->data[61] = (unsigned char)(sha256->bitlen >> 16);
    sha256->data[60] = (unsigned char)(sha256->bitlen >> 24);
    sha256->data[59] = (unsigned char)(sha256->bitlen >> 32);
    sha256->data[58] = (unsigned char)(sha256->bitlen >> 40);
    sha256->data[57] = (unsigned char)(sha256->bitlen >> 48);
    sha256->data[56] = (unsigned char)(sha256->bitlen >> 56);
    _transform(sha256, sha256->data);
    for (i = 0; i < 4; ++i) {
        hash[i] = (sha256->state[0] >> (24 - i * 8)) & 0xff;
        hash[i + 4] = (sha256->state[1] >> (24 - i * 8)) & 0xff;
        hash[i + 8] = (sha256->state[2] >> (24 - i * 8)) & 0xff;
        hash[i + 12] = (sha256->state[3] >> (24 - i * 8)) & 0xff;
        hash[i + 16] = (sha256->state[4] >> (24 - i * 8)) & 0xff;
        hash[i + 20] = (sha256->state[5] >> (24 - i * 8)) & 0xff;
        hash[i + 24] = (sha256->state[6] >> (24 - i * 8)) & 0xff;
        hash[i + 28] = (sha256->state[7] >> (24 - i * 8)) & 0xff;
    }
    ZERO(sha256, sizeof(sha256_ctx));
}
