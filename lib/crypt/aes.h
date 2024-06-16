#ifndef AES_H_
#define AES_H_

#include "base/macro.h"

#define AES_BLOCK_SIZE 16
typedef struct aes_ctx {
    int32_t encrypt;
    int32_t nrounds;
    uint8_t output[AES_BLOCK_SIZE];
    uint32_t schedule[256 / 8 + 28];    
}aes_ctx;
//key: 16 24 32 keybits: 128 192 256
void aes_init(aes_ctx *aes, const char *key, size_t klens, int32_t keybits, int32_t encrypt);
//input: DES_BLOCK_SIZE
char *aes_crypt(aes_ctx *aes, const void *data);

#endif//AES_H_
