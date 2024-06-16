#ifndef CRYPT_H_
#define CRYPT_H_

#include "crypt/aes.h"
#include "crypt/des.h"
#include "crypt/padding.h"

#define MAX_BLOCK_SIZE AES_BLOCK_SIZE
typedef char *(*_crypt_cb)(void *, const void *);
typedef enum engine_type {
    DES = 0x01,
    DES3,
    AES
}engine_type;
typedef enum cipher_model {
    ECB = 0x01,
    CBC,
    CFB,
    OFB,
    CTR
}cipher_model;
typedef union engine_ctx {
    aes_ctx aes;
    des_ctx des;
}engine_ctx;
typedef struct crypt_ctx {
    int32_t encrypt;
    cipher_model model;
    padding_model padding;
    size_t block_lens;
    void *cur_ctx;
    uint8_t iv[MAX_BLOCK_SIZE];
    uint8_t cur_iv[MAX_BLOCK_SIZE];
    uint8_t pd_data[MAX_BLOCK_SIZE];
    uint8_t xor_data[MAX_BLOCK_SIZE];
    engine_ctx eng_ctx;
    _crypt_cb _crypt;
}crypt_ctx;
//keybits: for AES 128 192 256
void crypt_init(crypt_ctx *crypt, engine_type engine, cipher_model model,
    const char *key, size_t klens, int32_t keybits, int32_t encrypt);
size_t crypt_block_size(crypt_ctx *crypt);
//ECB CBC需要  CBC CFB OFB CTR可选
void crypt_padding(crypt_ctx *crypt, padding_model padding);
//CBC CFB OFB CTR
void crypt_iv(crypt_ctx *crypt, const char *iv, size_t ilens);
void crypt_clear(crypt_ctx *crypt);
//lens <= block_lens
void *crypt_block(crypt_ctx *crypt, const void *data, size_t lens, size_t *size);
//output:lens + crypt_block_size
size_t crypt_dofinal(crypt_ctx *crypt, const void *data, size_t lens, char *output);

#endif//CRYPT_H_
