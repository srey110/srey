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
/// <param name="inc">扩容增量基数，取值 [0, INT32_MAX]，超界断言。内部会向上取到 2 的幂
/// （下限 2）——它随后当对齐模数用，非 2 的幂对不齐；0 表示用默认值。外部模式下被忽略并标记为 0</param>
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
/// 写入以'\0'结束的字符串。buf 允许指向本 ctx 自己的缓冲，规则同 binary_set_binary
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <param name="buf">值</param>
void binary_set_string(binary_ctx *ctx, const char *buf);
/// <summary>
/// 写入char *。buf 允许指向本 ctx 自己的缓冲：扩容搬走后会自动换算成新地址，重叠也按 memmove 处理
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <param name="buf">值</param>
/// <param name="lens">字节数</param>
void binary_set_binary(binary_ctx *ctx, const char *buf, size_t lens);
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
// 全部 binary_get_* 越界即 ASSERTAB abort 进程，没有失败回传通道。凡是长度由对端决定的
// 报文，读之前必须先用下面两个判一遍——判定与读挨着写，漏判在 review 里看得见。
// binary_get_string 是唯一预判不了的（越界条件是"剩余字节里没有 NUL"，得先扫），
// 那种场合改用 binary_try_get_string。
/// <summary>
/// 还剩多少字节没读
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <returns>剩余字节数</returns>
static inline size_t binary_remain(binary_ctx *ctx) {
    return ctx->size - ctx->offset;
}
/// <summary>
/// 还够不够读 lens 字节。lens 收 uint64_t 不收 size_t：报文里的长度字段常是 64 位，
/// 调用方若先转窄再比，32 位构建上 0x1_0000_0001 会截成 1 蒙混过关
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <param name="lens">打算读的字节数</param>
/// <returns>够返回非 0</returns>
static inline int32_t binary_have(binary_ctx *ctx, uint64_t lens) {
    return lens <= (uint64_t)(ctx->size - ctx->offset);
}
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
/// 获取字符串值,取到'\0'结束。剩余字节里没有 '\0' 即 ASSERTAB abort,
/// 长度由对端决定的报文改用 binary_try_get_string
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <returns>char *</returns>
char *binary_get_string(binary_ctx *ctx);
/// <summary>
/// 同 binary_get_string,但剩余字节里没有 '\0' 时失败而不是断言。
/// 越界条件是"扫不到 NUL",binary_have 预判不了,所以单给一个接口
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <returns>字符串首址;剩余字节里没有 '\0' 返回 NULL(offset 不动)</returns>
char *binary_try_get_string(binary_ctx *ctx);
/// <summary>
/// 获取指定长度的数据
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <param name="lens">长度</param>
/// <returns>char *;lens 为 0 时返回 NULL</returns>
char *binary_get_binary(binary_ctx *ctx, size_t lens);
/// <summary>
/// 跳过指定字节
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <param name="lens">长度</param>
void binary_get_skip(binary_ctx *ctx, size_t lens);

#endif//BINARY_H_
