#ifndef SHA256_H_
#define SHA256_H_

#include "base/macro.h"

#define SHA256_BLOCK_SIZE 32

typedef struct {
    uint32_t datalen;
    unsigned long long bitlen;
    uint32_t state[8];
    unsigned char data[64];
} sha256_ctx;

void sha256_init(sha256_ctx *sha256);
void sha256_update(sha256_ctx *sha256, const void *data, size_t lens);
void sha256_final(sha256_ctx *sha256, char hash[SHA256_BLOCK_SIZE]);

#endif//SHA256_H_
