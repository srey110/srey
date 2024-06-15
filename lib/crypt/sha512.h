#ifndef SHA512_H_
#define SHA512_H_

#include "base/macro.h"

#define SHA512_BLOCK_SIZE 64

typedef struct sha512_ctx {
    uint64_t state[8];
    uint64_t bitlen[2];
    uint8_t data[128];
} sha512_ctx;

void sha512_init(sha512_ctx *sha512);
void sha512_update(sha512_ctx *sha512, const void *data, size_t lens);
void sha512_final(sha512_ctx *sha512, char hash[SHA512_BLOCK_SIZE]);

#endif//SHA512_H_
