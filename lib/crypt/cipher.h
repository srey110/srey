#ifndef CIPHER_H_
#define CIPHER_H_

#include "crypt/aes.h"
#include "crypt/des.h"
#include "crypt/padding.h"

#define CIPHER_BLOCK_SIZE AES_BLOCK_SIZE
typedef char *(*_cipher_cb)(void *, const void *);

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
typedef struct cipher_ctx {
    int32_t encrypt;
    cipher_model model;
    padding_model padding;
    size_t block_lens;
    void *cur_ctx;
    _cipher_cb _cipher;
    uint8_t iv[CIPHER_BLOCK_SIZE];
    uint8_t cur_iv[CIPHER_BLOCK_SIZE];
    uint8_t pd_data[CIPHER_BLOCK_SIZE];
    uint8_t xor_data[CIPHER_BLOCK_SIZE];
    union {
        aes_ctx aes;
        des_ctx des;
    }eng_ctx;
}cipher_ctx;
//keybits: for AES 128 192 256
void cipher_init(cipher_ctx *cipher, engine_type engine, cipher_model model,
    const char *key, size_t klens, int32_t keybits, int32_t encrypt);
//block_lens
size_t cipher_size(cipher_ctx *cipher);
//ECB CBC需要  CFB OFB CTR可选
void cipher_padding(cipher_ctx *cipher, padding_model padding);
//CBC CFB OFB CTR
void cipher_iv(cipher_ctx *cipher, const char *iv, size_t ilens);
void cipher_reset(cipher_ctx *cipher);
//lens <= block_lens
void *cipher_block(cipher_ctx *cipher, const void *data, size_t lens, size_t *size);
//output:lens + crypt_block_size
size_t cipher_dofinal(cipher_ctx *cipher, const void *data, size_t lens, char *output);

#endif//CIPHER_H_
