#ifndef MD4_H_
#define MD4_H_

#include "base/macro.h"

#define MD4_BLOCK_SIZE 16 // MD4 摘要输出长度（字节）

typedef struct md4_ctx {
    uint32_t state[4];  // 摘要状态（A、B、C、D 四个字）
    uint32_t count[2];  // 已处理位数（低32位、高32位）
    uint8_t data[64];   // 输入缓冲区
}md4_ctx;

/// <summary>初始化 MD4 上下文</summary>
/// <param name="md4">md4_ctx</param>
void md4_init(md4_ctx *md4);
/// <summary>向 MD4 上下文输入数据</summary>
/// <param name="md4">md4_ctx</param>
/// <param name="data">数据</param>
/// <param name="lens">数据长度</param>
void md4_update(md4_ctx *md4, const void *data, size_t lens);
/// <summary>完成 MD4 计算，输出摘要</summary>
/// <param name="md4">md4_ctx</param>
/// <param name="hash">输出摘要，长度 MD4_BLOCK_SIZE</param>
void md4_final(md4_ctx *md4, char hash[MD4_BLOCK_SIZE]);

#endif//MD4_H_
