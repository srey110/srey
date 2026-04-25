#ifndef SHA512_H_
#define SHA512_H_

#include "base/macro.h"

#define SHA512_BLOCK_SIZE 64 // SHA-512 摘要输出长度（字节）

typedef struct sha512_ctx {
    uint64_t state[8];    // 摘要状态（a~h 八个 64 位字）
    uint64_t bitlen[2];   // 已处理的总位数（128 位，低位在前）
    uint8_t data[128];    // 输入缓冲区
} sha512_ctx;

/// <summary>初始化 SHA-512 上下文</summary>
/// <param name="sha512">sha512_ctx</param>
void sha512_init(sha512_ctx *sha512);
/// <summary>向 SHA-512 上下文输入数据</summary>
/// <param name="sha512">sha512_ctx</param>
/// <param name="data">数据</param>
/// <param name="lens">数据长度</param>
void sha512_update(sha512_ctx *sha512, const void *data, size_t lens);
/// <summary>完成 SHA-512 计算，输出摘要</summary>
/// <param name="sha512">sha512_ctx</param>
/// <param name="hash">输出摘要，长度 SHA512_BLOCK_SIZE</param>
void sha512_final(sha512_ctx *sha512, char hash[SHA512_BLOCK_SIZE]);

#endif//SHA512_H_
