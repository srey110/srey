#ifndef SHA256_H_
#define SHA256_H_

#include "algo/algo.h"

#define SHA256_BLOCK_SIZE 32

typedef struct {
    word_t datalen;
    unsigned long long bitlen;
    word_t state[8];
    unsigned char data[64];
} sha256_ctx;

void sha256_init(sha256_ctx *ctx);
void sha256_update(sha256_ctx *ctx, const unsigned char *data, size_t len);
void sha256_final(sha256_ctx *ctx, unsigned char hash[SHA256_BLOCK_SIZE]);

#endif//SHA256_H_
