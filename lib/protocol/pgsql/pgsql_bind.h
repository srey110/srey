#ifndef PGSQL_BIND_H_
#define PGSQL_BIND_H_

#include "protocol/pgsql/pgsql_struct.h"

/// <summary>
/// 初始化参数绑定上下文，分配格式与值的序列化缓冲区
/// </summary>
/// <param name="bind">pgsql_bind_ctx 指针</param>
/// <param name="nparam">参数数量</param>
void pgsql_bind_init(pgsql_bind_ctx *bind, uint16_t nparam);
/// <summary>
/// 释放参数绑定上下文内部缓冲区
/// </summary>
/// <param name="bind">pgsql_bind_ctx 指针</param>
void pgsql_bind_free(pgsql_bind_ctx *bind);
/// <summary>
/// 清空已绑定的参数，重置缓冲区偏移量（保留容量以便复用）
/// </summary>
/// <param name="bind">pgsql_bind_ctx 指针</param>
void pgsql_bind_clear(pgsql_bind_ctx *bind);
/// <summary>
/// 绑定一个原始字节参数
/// </summary>
/// <param name="bind">pgsql_bind_ctx 指针</param>
/// <param name="value">参数值数据指针</param>
/// <param name="lens">参数值字节长度</param>
/// <param name="format">参数格式（文本或二进制）</param>
void pgsql_bind(pgsql_bind_ctx *bind, char *value, size_t lens, pgpack_format format);
/// <summary>
/// 绑定 bool 类型参数（二进制格式）
/// </summary>
/// <param name="bind">pgsql_bind_ctx 指针</param>
/// <param name="value">布尔值，非零为真</param>
void pgsql_bind_bool(pgsql_bind_ctx *bind, int8_t value);
/// <summary>
/// 绑定 int16 类型参数（大端二进制格式）
/// </summary>
/// <param name="bind">pgsql_bind_ctx 指针</param>
/// <param name="value">int16_t 整数值</param>
void pgsql_bind_int16(pgsql_bind_ctx *bind, int16_t value);
/// <summary>
/// 绑定 int32 类型参数（大端二进制格式）
/// </summary>
/// <param name="bind">pgsql_bind_ctx 指针</param>
/// <param name="value">int32_t 整数值</param>
void pgsql_bind_int32(pgsql_bind_ctx *bind, int32_t value);
/// <summary>
/// 绑定 int64 类型参数（大端二进制格式）
/// </summary>
/// <param name="bind">pgsql_bind_ctx 指针</param>
/// <param name="value">int64_t 整数值</param>
void pgsql_bind_int64(pgsql_bind_ctx *bind, int64_t value);
/// <summary>
/// 绑定 float 类型参数（大端二进制格式）
/// </summary>
/// <param name="bind">pgsql_bind_ctx 指针</param>
/// <param name="value">float 单精度浮点值</param>
void pgsql_bind_float(pgsql_bind_ctx *bind, float value);
/// <summary>
/// 绑定 double 类型参数（大端二进制格式）
/// </summary>
/// <param name="bind">pgsql_bind_ctx 指针</param>
/// <param name="value">double 双精度浮点值</param>
void pgsql_bind_double(pgsql_bind_ctx *bind, double value);

#endif//PGSQL_BIND_H_
