#ifndef MD5_H_
#define MD5_H_

#include "crypto/crypto.h"

#define MD5_BLOCK_SIZE 16

typedef struct {
    word_t datalen;
    unsigned long long bitlen;
    word_t state[4];
    unsigned char data[64];
} md5_ctx;

void md5_init(md5_ctx *ctx);
void md5_update(md5_ctx *ctx, const unsigned char *data, size_t len);
void md5_final(md5_ctx *ctx, unsigned char hash[MD5_BLOCK_SIZE]);

#endif//MD5_H_

