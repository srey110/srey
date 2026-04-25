#ifndef SHA1_H_
#define SHA1_H_

#include "base/macro.h"

#define SHA1_BLOCK_SIZE 20 // SHA-1 摘要输出长度（字节）

typedef struct {
    uint32_t datalen;   // 当前缓冲区中的字节数
    uint64_t bitlen;    // 已处理的总位数
    uint32_t state[5];  // 摘要状态（H0~H4）
    uint32_t k[4];      // 四轮常量
    uint8_t data[64];   // 输入缓冲区
} sha1_ctx;

/// <summary>初始化 SHA-1 上下文</summary>
/// <param name="sha1">sha1_ctx</param>
void sha1_init(sha1_ctx *sha1);
/// <summary>向 SHA-1 上下文输入数据</summary>
/// <param name="sha1">sha1_ctx</param>
/// <param name="data">数据</param>
/// <param name="lens">数据长度</param>
void sha1_update(sha1_ctx *sha1, const void *data, size_t lens);
/// <summary>完成 SHA-1 计算，输出摘要</summary>
/// <param name="sha1">sha1_ctx</param>
/// <param name="hash">输出摘要，长度 SHA1_BLOCK_SIZE</param>
void sha1_final(sha1_ctx *sha1, char hash[SHA1_BLOCK_SIZE]);

#endif//SHA1_H_
