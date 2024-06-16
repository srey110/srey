#include "crypt/md4.h"

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
#define F(x, y, z) (((x) & (y)) | ((~x) & (z)))
#define G(x, y, z) (((x) & (y)) | ((x) & (z)) | ((y) & (z)))
#define H(x, y, z) ((x) ^ (y) ^ (z))
#define ROTATE_LEFT(x, n) (((x) << (n)) | ((x) >> (32-(n))))
#define FF(a, b, c, d, x, s) { (a) += F ((b), (c), (d)) + (x); \
                               (a) = ROTATE_LEFT ((a), (s)); }
#define GG(a, b, c, d, x, s) { (a) += G ((b), (c), (d)) + (x) + (uint32_t)0x5a827999; \
                               (a) = ROTATE_LEFT ((a), (s)); }
#define HH(a, b, c, d, x, s) { (a) += H ((b), (c), (d)) + (x) + (uint32_t)0x6ed9eba1; \
                               (a) = ROTATE_LEFT ((a), (s)); }

static const uint8_t pd[64] = {
    0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};
static void _transform(md4_ctx *md4, const uint8_t *data) {
    uint32_t i, j, a = md4->state[0], b = md4->state[1], c = md4->state[2], d = md4->state[3], x[16];
    for (i = 0, j = 0; j < 64; i++, j += 4) {
        x[i] = ((uint32_t)data[j]) | (((uint32_t)data[j + 1]) << 8) | (((uint32_t)data[j + 2]) << 16) | (((uint32_t)data[j + 3]) << 24);
    }
    /* Round 1 */
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
    /* Round 2 */
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
    /* Round 3 */
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
    md4->state[0] += a;
    md4->state[1] += b;
    md4->state[2] += c;
    md4->state[3] += d;
    ZERO(x, sizeof(x));
}
void md4_init(md4_ctx *md4) {
    md4->count[0] = md4->count[1] = 0;
    md4->state[0] = 0x67452301;
    md4->state[1] = 0xefcdab89;
    md4->state[2] = 0x98badcfe;
    md4->state[3] = 0x10325476;
}
void md4_update(md4_ctx *md4, const void *data, size_t lens) {
    uint32_t i, index, partlens;
    uint8_t *p = (uint8_t *)data;
    index = (uint32_t)((md4->count[0] >> 3) & 0x3F);
    if ((md4->count[0] += ((uint32_t)lens << 3)) < ((uint32_t)lens << 3)) {
        md4->count[1]++;
    }
    md4->count[1] += ((uint32_t)lens >> 29);
    partlens = 64 - index;
    if ((uint32_t)lens >= partlens) {
        memcpy(&md4->data[index], p, partlens);
        _transform(md4, md4->data);
        for (i = partlens; i + 63 < (uint32_t)lens; i += 64) {
            _transform(md4, &p[i]);
        }
        index = 0;
    } else {
        i = 0;
    }
    memcpy(&md4->data[index], &p[i], lens - i);
}
static void _encode(const uint32_t *data, uint32_t lens, uint8_t *out) {
    uint32_t i, j;
    for (i = 0, j = 0; j < lens; i++, j += 4) {
        out[j] = (uint8_t)(data[i] & 0xff);
        out[j + 1] = (uint8_t)((data[i] >> 8) & 0xff);
        out[j + 2] = (uint8_t)((data[i] >> 16) & 0xff);
        out[j + 3] = (uint8_t)((data[i] >> 24) & 0xff);
    }
}
void md4_final(md4_ctx *md4, char hash[MD4_BLOCK_SIZE]) {
    uint8_t bits[8];
    uint32_t index, padlens;
    _encode(md4->count, 8, bits);
    index = (uint32_t)((md4->count[0] >> 3) & 0x3f);
    padlens = (index < 56) ? (56 - index) : (120 - index);
    md4_update(md4, pd, padlens);
    md4_update(md4, bits, 8);
    _encode(md4->state, 16, (uint8_t *)hash);
    ZERO(md4, sizeof(md4_ctx));
}
