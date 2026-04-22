#ifndef MD4_H_
#define MD4_H_

#include "base/macro.h"

#define MD4_BLOCK_SIZE 16

typedef struct md4_ctx {
    uint32_t  state[4];
    uint32_t count[2];
    uint8_t data[64];
}md4_ctx;

void md4_init(md4_ctx *md4);
void md4_update(md4_ctx *md4, const void *data, size_t lens);
void md4_final(md4_ctx *md4, char hash[MD4_BLOCK_SIZE]);

#endif//MD4_H_
