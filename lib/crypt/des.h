#ifndef DES_H_
#define DES_H_

#include "base/macro.h"

#define DES_BLOCK_SIZE 8 // DES 分组大小（字节）
typedef struct des_ctx {
    int32_t des3;                   // 1 为 3DES，0 为标准 DES
    uint8_t output[DES_BLOCK_SIZE]; // 加解密结果缓冲区
    uint8_t schedule[3 * 16 * 6];  // 轮密钥调度表（3DES 包含三组）
}des_ctx;
/// <summary>
/// des 初始化
/// </summary>
/// <param name="des">des_ctx</param>
/// <param name="key">密码</param>
/// <param name="klens">密码长度。单重 DES 不足 8 字节补零；3DES 按 NIST SP 800-67 的三种取法补齐：满 24 字节三段子密钥独立，9 ~ 16 字节 K3 取 K1，不超过 8 字节时三段相同——该取法等价于单重 DES，强度只有 56 位</param>
/// <param name="des3">1 3des, 0 des</param>
/// <param name="encrypt">1 加密 0 解密</param>
void des_init(des_ctx *des, const char *key, size_t klens, int32_t des3, int32_t encrypt);
/// <summary>
/// des加解密
/// </summary>
/// <param name="des">des_ctx</param>
/// <param name="data">待加解密数据,长度:DES_BLOCK_SIZE</param>
/// <returns>加解密后的数据,长度:DES_BLOCK_SIZE</returns>
char *des_crypt(des_ctx *des, const void *data);

#endif//DES_H_
