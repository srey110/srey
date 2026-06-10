#ifndef MD5_H_
#define MD5_H_

#include "base/macro.h"

#define MD5_BLOCK_SIZE 16 // MD5 摘要输出长度（字节）

typedef struct md5_ctx {
    uint32_t datalen;   // 当前缓冲区中的字节数
    uint64_t bitlen;    // 已处理的总位数
    uint32_t state[4];  // 摘要状态（A、B、C、D 四个字）
    uint8_t data[64];   // 输入缓冲区
} md5_ctx;

/// <summary>初始化 MD5 上下文</summary>
/// <param name="md5">md5_ctx</param>
void md5_init(md5_ctx *md5);
/// <summary>向 MD5 上下文输入数据</summary>
/// <param name="md5">md5_ctx</param>
/// <param name="data">数据</param>
/// <param name="lens">数据长度</param>
void md5_update(md5_ctx *md5, const void *data, size_t lens);
/// <summary>完成 MD5 计算，输出摘要</summary>
/// <param name="md5">md5_ctx</param>
/// <param name="hash">输出摘要，长度 MD5_BLOCK_SIZE</param>
void md5_final(md5_ctx *md5, char hash[MD5_BLOCK_SIZE]);

#endif//MD5_H_

