#ifndef AES_H_
#define AES_H_

#include "base/macro.h"

#define AES_BLOCK_SIZE 16 // AES 分组大小（字节）
typedef struct aes_ctx {
    int32_t encrypt;                    // 1 加密，0 解密
    int32_t nrounds;                    // 加密轮数，由密钥长度决定
    uint8_t output[AES_BLOCK_SIZE];     // 加解密结果缓冲区
    uint32_t schedule[256 / 8 + 28];   // 轮密钥调度表
}aes_ctx;
/// <summary>
/// aes 初始化
/// </summary>
/// <param name="aes">aes_ctx</param>
/// <param name="key">密码</param>
/// <param name="klens">密码长度, 不足16 24 32 会填充0</param>
/// <param name="keybits">128 192 256</param>
/// <param name="encrypt">1 加密, 0 解密</param>
void aes_init(aes_ctx *aes, const char *key, size_t klens, int32_t keybits, int32_t encrypt);
/// <summary>
/// aes加解密
/// </summary>
/// <param name="aes">aes_ctx</param>
/// <param name="data">待加解密数据,长度:AES_BLOCK_SIZE</param>
/// <returns>加解密后的数据,长度:AES_BLOCK_SIZE</returns>
char *aes_crypt(aes_ctx *aes, const void *data);

#endif//AES_H_
