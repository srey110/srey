#ifndef MD2_H_
#define MD2_H_

#include "base/macro.h"

#define MD2_BLOCK_SIZE 16 // MD2 摘要输出长度（字节）

typedef struct md2_ctx {
    uint8_t data[16];       // 当前未满一块的输入缓冲
    uint8_t state[48];      // MD2 内部状态（分三段：前16字节为当前状态）
    uint8_t checksum[16];   // 校验和
    int32_t lens;           // 当前缓冲中的字节数
} md2_ctx;

/// <summary>初始化 MD2 上下文</summary>
/// <param name="md2">md2_ctx</param>
void md2_init(md2_ctx *md2);
/// <summary>向 MD2 上下文输入数据</summary>
/// <param name="md2">md2_ctx</param>
/// <param name="data">数据</param>
/// <param name="lens">数据长度</param>
void md2_update(md2_ctx *md2, const void *data, size_t lens);
/// <summary>完成 MD2 计算，输出摘要</summary>
/// <param name="md2">md2_ctx</param>
/// <param name="hash">输出摘要，长度 MD2_BLOCK_SIZE</param>
void md2_final(md2_ctx *md2, char hash[MD2_BLOCK_SIZE]);

#endif//MD2_H_
