#ifndef MD5_H_
#define MD5_H_

#include "crypto/crypto.h"

typedef struct {
    unsigned char data[64];
    word_t datalen;
    unsigned long long bitlen;
    word_t state[4];
} md5_ctx;

void md5_init(md5_ctx *ctx);
void md5_update(md5_ctx *ctx, const unsigned char *data, size_t len);
void md5_final(md5_ctx *ctx, unsigned char hash[16]);

#endif//MD5_H_

