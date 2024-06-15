#ifndef DES_H_
#define DES_H_

#include "base/macro.h"

#define DES_BLOCK_SIZE 8
typedef struct des_ctx {
    uint8_t schedule[3 * 16 * 6];
    uint8_t output[DES_BLOCK_SIZE];
    int32_t des3;
}des_ctx;
//key:8  3des 3 * 8
int32_t des_init(des_ctx *des, const char *key, int32_t des3, int32_t encrypt);
//input: DES_BLOCK_SIZE
char *des_crypt(des_ctx *des, const void *input);

#endif//DES_H_
