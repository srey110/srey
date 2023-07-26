#ifndef SHA1_H_
#define SHA1_H_

#include "crypto/crypto.h"

typedef struct {
    unsigned char data[64];
    word_t datalen;
    unsigned long long bitlen;
    word_t state[5];
    word_t k[4];
} sha1_ctx;

void sha1_init(sha1_ctx *ctx);
void sha1_update(sha1_ctx *ctx, const unsigned char *data, size_t len);
void sha1_final(sha1_ctx *ctx, unsigned char hash[20]);

#endif//SHA1_H_
