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
/// <summary>
/// 绑定 NULL 参数（任意类型均适用）
/// </summary>
/// <param name="bind">pgsql_bind_ctx 指针</param>
void pgsql_bind_null(pgsql_bind_ctx *bind);
/// <summary>
/// 绑定文本类型参数，适用于 TEXT / VARCHAR / BPCHAR（文本格式）
/// </summary>
/// <param name="bind">pgsql_bind_ctx 指针</param>
/// <param name="value">UTF-8 字符串指针（无需以 '\0' 结尾）</param>
/// <param name="lens">字符串字节长度</param>
void pgsql_bind_text(pgsql_bind_ctx *bind, const char *value, size_t lens);
/// <summary>
/// 绑定 BYTEA 类型参数（二进制格式，原始字节直传）
/// </summary>
/// <param name="bind">pgsql_bind_ctx 指针</param>
/// <param name="value">原始字节指针</param>
/// <param name="lens">字节长度</param>
void pgsql_bind_bytea(pgsql_bind_ctx *bind, const char *value, size_t lens);
/// <summary>
/// 绑定 TIMESTAMP 类型参数（大端二进制格式）
/// </summary>
/// <param name="bind">pgsql_bind_ctx 指针</param>
/// <param name="usec">相对 PostgreSQL 纪元（2000-01-01 00:00:00）的微秒数</param>
void pgsql_bind_timestamp(pgsql_bind_ctx *bind, int64_t usec);
/// <summary>
/// 绑定 TIMESTAMPTZ 类型参数（大端二进制格式，编码与 TIMESTAMP 相同）
/// </summary>
/// <param name="bind">pgsql_bind_ctx 指针</param>
/// <param name="usec">相对 PostgreSQL UTC 纪元（2000-01-01 00:00:00 UTC）的微秒数</param>
void pgsql_bind_timestamptz(pgsql_bind_ctx *bind, int64_t usec);
/// <summary>
/// 绑定 DATE 类型参数（大端二进制格式）
/// </summary>
/// <param name="bind">pgsql_bind_ctx 指针</param>
/// <param name="days">相对 PostgreSQL 纪元（2000-01-01）的天数</param>
void pgsql_bind_date(pgsql_bind_ctx *bind, int32_t days);
/// <summary>
/// 绑定 UUID 类型参数（二进制格式，16 字节原始 UUID）
/// </summary>
/// <param name="bind">pgsql_bind_ctx 指针</param>
/// <param name="uuid">16 字节 UUID 原始数据</param>
void pgsql_bind_uuid(pgsql_bind_ctx *bind, const char uuid[16]);

#endif//PGSQL_BIND_H_
