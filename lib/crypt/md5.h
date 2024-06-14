#ifndef MD5_H_
#define MD5_H_

#include "base/macro.h"

#define MD5_BLOCK_SIZE 16

typedef struct md5_ctx {
    uint32_t datalen;
    unsigned long long bitlen;
    uint32_t state[4];
    unsigned char data[64];
} md5_ctx;

void md5_init(md5_ctx *md5);
void md5_update(md5_ctx *md5, const void *data, size_t lens);
void md5_final(md5_ctx *md5, char hash[MD5_BLOCK_SIZE]);

#endif//MD5_H_

