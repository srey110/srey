#ifndef SHA1_H_
#define SHA1_H_

#include "base/macro.h"

#define SHA1_BLOCK_SIZE 20

typedef struct {
    uint32_t datalen;
    uint64_t bitlen;
    uint32_t state[5];
    uint32_t k[4];
    uint8_t data[64];
} sha1_ctx;

void sha1_init(sha1_ctx *sha1);
void sha1_update(sha1_ctx *sha1, const void *data, size_t lens);
void sha1_final(sha1_ctx *sha1, char hash[SHA1_BLOCK_SIZE]);

#endif//SHA1_H_
