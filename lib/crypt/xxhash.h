#ifndef XXHASH_H_
#define XXHASH_H_

#include "base/macro.h"

// 非密码学哈希，不能用作 MAC 或口令派生。
// canonical 字节 = pack_integer(out, h, N, 0)（大端）；xxh128_t 先写 high 再写 low
#define XXH32_BLOCK_SIZE 4 // XXH32 canonical 输出字节数
#define XXH64_BLOCK_SIZE 8 // XXH64 canonical 输出字节数
#define XXH128_BLOCK_SIZE 16 // XXH3_128 canonical 输出字节数
#define XXH3_SECRET_SIZE 192 // XXH3 secret 字节数
#define XXH3_BUFFER_SIZE 256 // XXH3 流式缓冲字节数

typedef struct xxh128_t {
    uint64_t low;   // 低 64 位
    uint64_t high;  // 高 64 位
}xxh128_t;
typedef struct xxh32_ctx {
    uint8_t buf[16];    // 未满一条的输入
    uint32_t bufsize;   // buf 中的字节数
    uint32_t total;     // 已输入字节数的低 32 位
    uint32_t large;     // 是否曾累计到 16 字节及以上
    uint32_t acc[4];    // 累加器
}xxh32_ctx;
typedef struct xxh64_ctx {
    uint8_t buf[32];    // 未满一条的输入
    uint32_t bufsize;   // buf 中的字节数
    uint64_t total;     // 已输入字节数
    uint64_t acc[4];    // 累加器
}xxh64_ctx;
typedef struct xxh3_ctx {// 64 位和 128 位共用；只存值、不存自指针，可以直接 memcpy 复制
    uint8_t buf[XXH3_BUFFER_SIZE];      // 攒满才处理，并保留最后一条供 digest 重叠读取
    uint8_t secret[XXH3_SECRET_SIZE];   // 由 seed 派生，seed 为 0 时即默认 secret
    uint32_t bufsize;                   // buf 中的字节数
    uint32_t nstripes;                  // 当前块已处理的条数
    uint64_t total;                     // 已输入字节数
    uint64_t seed;                      // 种子
    uint64_t acc[8];                    // 累加器
}xxh3_ctx;

/// <summary>
/// XXH32 一次性计算
/// </summary>
/// <param name="data">数据，lens 为 0 时可为 NULL</param>
/// <param name="lens">数据长度</param>
/// <param name="seed">种子</param>
/// <returns>哈希值</returns>
uint32_t xxh32(const void *data, size_t lens, uint32_t seed);
/// <summary>
/// XXH32 流式初始化，整个 ctx 重新初始化
/// </summary>
/// <param name="ctx">xxh32_ctx</param>
/// <param name="seed">种子</param>
void xxh32_init(xxh32_ctx *ctx, uint32_t seed);
/// <summary>
/// XXH32 流式输入数据
/// </summary>
/// <param name="ctx">xxh32_ctx</param>
/// <param name="data">数据，lens 为 0 时可为 NULL</param>
/// <param name="lens">数据长度</param>
void xxh32_update(xxh32_ctx *ctx, const void *data, size_t lens);
/// <summary>
/// XXH32 流式取结果，不改 ctx，之后还能继续 update
/// </summary>
/// <param name="ctx">xxh32_ctx</param>
/// <returns>到目前为止全部输入的哈希值，与对同样输入、同一 seed 调 xxh32 的结果相同</returns>
uint32_t xxh32_digest(const xxh32_ctx *ctx);
/// <summary>
/// XXH64 一次性计算，同 xxh32
/// </summary>
/// <param name="data">数据，lens 为 0 时可为 NULL</param>
/// <param name="lens">数据长度</param>
/// <param name="seed">种子</param>
/// <returns>哈希值</returns>
uint64_t xxh64(const void *data, size_t lens, uint64_t seed);
/// <summary>
/// XXH64 流式初始化，同 xxh32_init
/// </summary>
/// <param name="ctx">xxh64_ctx</param>
/// <param name="seed">种子</param>
void xxh64_init(xxh64_ctx *ctx, uint64_t seed);
/// <summary>
/// XXH64 流式输入数据，同 xxh32_update
/// </summary>
/// <param name="ctx">xxh64_ctx</param>
/// <param name="data">数据，lens 为 0 时可为 NULL</param>
/// <param name="lens">数据长度</param>
void xxh64_update(xxh64_ctx *ctx, const void *data, size_t lens);
/// <summary>
/// XXH64 流式取结果，同 xxh32_digest
/// </summary>
/// <param name="ctx">xxh64_ctx</param>
/// <returns>到目前为止全部输入的哈希值，与对同样输入、同一 seed 调 xxh64 的结果相同</returns>
uint64_t xxh64_digest(const xxh64_ctx *ctx);
/// <summary>
/// XXH3 64 位一次性计算
/// </summary>
/// <param name="data">数据，lens 为 0 时可为 NULL</param>
/// <param name="lens">数据长度</param>
/// <param name="seed">种子</param>
/// <returns>哈希值</returns>
uint64_t xxh3_64(const void *data, size_t lens, uint64_t seed);
/// <summary>
/// XXH3 128 位一次性计算，同 xxh3_64
/// </summary>
/// <param name="data">数据，lens 为 0 时可为 NULL</param>
/// <param name="lens">数据长度</param>
/// <param name="seed">种子</param>
/// <returns>哈希值</returns>
xxh128_t xxh3_128(const void *data, size_t lens, uint64_t seed);
/// <summary>
/// XXH3 流式初始化，64 位与 128 位共用，整个 ctx 重新初始化
/// </summary>
/// <param name="ctx">xxh3_ctx</param>
/// <param name="seed">种子</param>
void xxh3_init(xxh3_ctx *ctx, uint64_t seed);
/// <summary>
/// XXH3 流式输入数据，同 xxh32_update
/// </summary>
/// <param name="ctx">xxh3_ctx</param>
/// <param name="data">数据，lens 为 0 时可为 NULL</param>
/// <param name="lens">数据长度</param>
void xxh3_update(xxh3_ctx *ctx, const void *data, size_t lens);
/// <summary>
/// XXH3 流式取 64 位结果，同 xxh32_digest；同一个 ctx 可以同时取 64 位和 128 位
/// </summary>
/// <param name="ctx">xxh3_ctx</param>
/// <returns>到目前为止全部输入的哈希值，与对同样输入、同一 seed 调 xxh3_64 的结果相同</returns>
uint64_t xxh3_digest64(const xxh3_ctx *ctx);
/// <summary>
/// XXH3 流式取 128 位结果，同 xxh3_digest64
/// </summary>
/// <param name="ctx">xxh3_ctx</param>
/// <returns>到目前为止全部输入的哈希值，与对同样输入、同一 seed 调 xxh3_128 的结果相同</returns>
xxh128_t xxh3_digest128(const xxh3_ctx *ctx);

#endif//XXHASH_H_
