#ifndef BINARY_H_
#define BINARY_H_

#include "base/structs.h"
#include "utils/utils.h"

typedef struct binary_ctx {
    char *data;
    size_t inc;//增加基数
    size_t size;//总长度
    size_t offset;//数据长度
}binary_ctx;
/// <summary>
/// 连续内存读写初始化。
/// buf=NULL：内部托管（malloc + 自动扩容），可用全部 binary_set_* / binary_get_* 接口；
/// buf!=NULL：外部托管（不接管所有权，调用方负责 buf 生命周期），仅可用 binary_get_* /
/// binary_offset / binary_at 等不扩容接口；调用 binary_set_* / binary_set_va 会 ASSERT 失败。
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <param name="buf">外部缓冲区指针；NULL 时切换为内部托管模式</param>
/// <param name="lens">外部模式下为 buf 长度；内部模式下为初始容量提示</param>
/// <param name="inc">扩容增量基数；外部模式下被忽略并标记为 0</param>
void binary_init(binary_ctx *ctx, char *buf, size_t lens, size_t inc);
/// <summary>
/// 释放内部托管缓冲区；对外部托管 buf（inc==0）不做任何操作。
/// 调用后 data=NULL、size=offset=0，可安全重复调用。
/// </summary>
/// <param name="ctx">binary_ctx</param>
void binary_free(binary_ctx *ctx);
/// <summary>
/// 设置偏移值
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <param name="off">偏移</param>
void binary_offset(binary_ctx *ctx, size_t off);
/// <summary>
/// 写入int8
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <param name="val">值</param>
void binary_set_int8(binary_ctx *ctx, int8_t val);
/// <summary>
/// 写入uint8
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <param name="val">值</param>
void binary_set_uint8(binary_ctx *ctx, uint8_t val);
/// <summary>
/// 写入整数
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <param name="val">值</param>
/// <param name="lens">val字节数</param>
/// <param name="islittle">1 小端序列 0大端序列</param>
void binary_set_integer(binary_ctx *ctx, int64_t val, size_t lens, int32_t islittle);
/// <summary>
/// 写入无符号整数
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <param name="val">值</param>
/// <param name="lens">val字节数</param>
/// <param name="islittle">1 小端序列 0大端序列</param>
void binary_set_uinteger(binary_ctx *ctx, uint64_t val, size_t lens, int32_t islittle);
/// <summary>
/// 写入float
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <param name="val">值</param>
/// <param name="islittle">1 小端序列 0大端序列</param>
void binary_set_float(binary_ctx *ctx, float val, int32_t islittle);
/// <summary>
/// 写入double
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <param name="val">值</param>
/// <param name="islittle">1 小端序列 0大端序列</param>
void binary_set_double(binary_ctx *ctx, double val, int32_t islittle);
/// <summary>
/// 写入char *
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <param name="buf">值</param>
/// <param name="lens">字节数；0 表示 buf 是 C 字符串（自动 strlen 并写入 NUL 终止），
///                    大于 0 表示 buf 是二进制数据（纯写入指定长度，不含 NUL）</param>
/// <remarks>lens=0 时 buf 必须以 NUL 终止，否则 strlen 越界读</remarks>
void binary_set_string(binary_ctx *ctx, const char *buf, size_t lens);
/// <summary>
/// 填充
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <param name="val">以该值填充</param>
/// <param name="lens">填充长度</param>
void binary_set_fill(binary_ctx *ctx, char val, size_t lens);
/// <summary>
/// 跳过指定长度
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <param name="lens">长度</param>
void binary_set_skip(binary_ctx *ctx, size_t lens);
/// <summary>
/// 写入变参数据
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <param name="fmt">格式化</param>
/// <param name="...">变参</param>
void binary_set_va(binary_ctx *ctx, const char *fmt, ...);
/// <summary>
/// 获取指定位置的指针
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <param name="pos">位置</param>
/// <returns>char *</returns>
char *binary_at(binary_ctx *ctx, size_t pos);
/// <summary>
/// 获取int8
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <returns>int8_t</returns>
int8_t binary_get_int8(binary_ctx *ctx);
/// <summary>
/// 获取uint8
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <returns>uint8_t</returns>
uint8_t binary_get_uint8(binary_ctx *ctx);
/// <summary>
/// 获取整数值
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <param name="lens">字节数</param>
/// <param name="islittle">1 小端序列 0大端序列</param>
/// <returns>int64_t</returns>
int64_t binary_get_integer(binary_ctx *ctx, size_t lens, int32_t islittle);
/// <summary>
/// 获取无符号整数值
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <param name="lens">字节数</param>
/// <param name="islittle">1 小端序列 0大端序列</param>
/// <returns>uint64_t</returns>
uint64_t binary_get_uinteger(binary_ctx *ctx, size_t lens, int32_t islittle);
/// <summary>
/// 获取float值
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <param name="islittle">1 小端序列 0大端序列</param>
/// <returns>float</returns>
float binary_get_float(binary_ctx *ctx, int32_t islittle);
/// <summary>
/// 获取double值
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <param name="islittle">1 小端序列 0大端序列</param>
/// <returns>double</returns>
double binary_get_double(binary_ctx *ctx, int32_t islittle);
/// <summary>
/// 获取字符串值
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <param name="lens">长度,0 取到'\0'结束</param>
/// <returns>char *</returns>
char *binary_get_string(binary_ctx *ctx, size_t lens);
/// <summary>
/// 跳过指定字节
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <param name="lens">长度</param>
void binary_get_skip(binary_ctx *ctx, size_t lens);

#endif//BINARY_H_
