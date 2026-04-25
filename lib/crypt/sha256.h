#ifndef SHA256_H_
#define SHA256_H_

#include "base/macro.h"

#define SHA256_BLOCK_SIZE 32 // SHA-256 摘要输出长度（字节）

typedef struct {
    uint32_t datalen;   // 当前缓冲区中的字节数
    uint64_t bitlen;    // 已处理的总位数
    uint32_t state[8];  // 摘要状态（a~h 八个字）
    uint8_t data[64];   // 输入缓冲区
} sha256_ctx;

/// <summary>初始化 SHA-256 上下文</summary>
/// <param name="sha256">sha256_ctx</param>
void sha256_init(sha256_ctx *sha256);
/// <summary>向 SHA-256 上下文输入数据</summary>
/// <param name="sha256">sha256_ctx</param>
/// <param name="data">数据</param>
/// <param name="lens">数据长度</param>
void sha256_update(sha256_ctx *sha256, const void *data, size_t lens);
/// <summary>完成 SHA-256 计算，输出摘要</summary>
/// <param name="sha256">sha256_ctx</param>
/// <param name="hash">输出摘要，长度 SHA256_BLOCK_SIZE</param>
void sha256_final(sha256_ctx *sha256, char hash[SHA256_BLOCK_SIZE]);

#endif//SHA256_H_
