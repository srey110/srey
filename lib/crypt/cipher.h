#ifndef CIPHER_H_
#define CIPHER_H_

#include "crypt/aes.h"
#include "crypt/des.h"
#include "crypt/padding.h"

#define CIPHER_BLOCK_SIZE AES_BLOCK_SIZE
typedef char *(*_cipher_cb)(void *, const void *);
//加解密类型
typedef enum engine_type {
    DES = 0x01,
    DES3,
    AES
}engine_type;
//加解密模式
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
/// <summary>
/// 加解密初始化
/// </summary>
/// <param name="cipher">cipher_ctx</param>
/// <param name="engine">加解密类型</param>
/// <param name="model">加解密模式</param>
/// <param name="key">密码</param>
/// <param name="klens">密码长度</param>
/// <param name="keybits">engine为AES,值 128 192 256</param>
/// <param name="encrypt">1 加密 0 解密</param>
void cipher_init(cipher_ctx *cipher, engine_type engine, cipher_model model,
    const char *key, size_t klens, int32_t keybits, int32_t encrypt);
/// <summary>
/// 获取块长度
/// </summary>
/// <param name="cipher">cipher_ctx</param>
/// <returns>块长度</returns>
size_t cipher_size(cipher_ctx *cipher);
/// <summary>
/// 设置填充模式.ECB CBC需要  CFB OFB CTR可选
/// </summary>
/// <param name="cipher">cipher_ctx</param>
/// <param name="padding">填充模式</param>
void cipher_padding(cipher_ctx *cipher, padding_model padding);
/// <summary>
/// 设置IV. CBC CFB OFB CTR需要
/// </summary>
/// <param name="cipher">cipher_ctx</param>
/// <param name="iv">IV</param>
/// <param name="ilens">IV长度,小于块长度会自动填充</param>
void cipher_iv(cipher_ctx *cipher, const char *iv, size_t ilens);
/// <summary>
/// 重置,准备新一轮加解密
/// </summary>
/// <param name="cipher">cipher_ctx</param>
void cipher_reset(cipher_ctx *cipher);
/// <summary>
/// 加解密一块数据
/// </summary>
/// <param name="cipher">cipher_ctx</param>
/// <param name="data">要加解密的数据</param>
/// <param name="lens">数据长度,小于等于块长度</param>
/// <param name="size">加解密后的长度</param>
/// <returns>加解密后的数据</returns>
void *cipher_block(cipher_ctx *cipher, const void *data, size_t lens, size_t *size);
/// <summary>
/// 加解密一段数据
/// </summary>
/// <param name="cipher">cipher_ctx</param>
/// <param name="data">要加解密的数据</param>
/// <param name="lens">数据长度</param>
/// <param name="output">加解密后的数据,长度:data长度 + 块长度</param>
/// <returns>加解密后的长度</returns>
size_t cipher_dofinal(cipher_ctx *cipher, const void *data, size_t lens, char *output);

#endif//CIPHER_H_
