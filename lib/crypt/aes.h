#ifndef AES_H_
#define AES_H_

#include "base/macro.h"

#define AES_BLOCK_SIZE 16
typedef struct aes_ctx {
    uint32_t rk[256/8 + 28];
    int32_t encrypt;
    int32_t nrounds;
    uint8_t output[AES_BLOCK_SIZE];
}aes_ctx;
//key: 16 24 32 keybits: 128 192 256
int32_t aes_init(aes_ctx *aes, const char *key, int32_t keybits, int32_t encrypt);
//input: DES_BLOCK_SIZE
char *aes_crypt(aes_ctx *aes, const void *input);

#endif//AES_H_
