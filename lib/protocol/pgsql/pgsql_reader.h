#ifndef PGSQL_READER_H_
#define PGSQL_READER_H_

#include "protocol/pgsql/pgsql_struct.h"

/// <summary>
/// 从 pgpack_ctx 中提取并初始化查询结果读取器
/// </summary>
/// <param name="pgpack">pgpack_ctx 指针，类型必须为 PGPACK_OK 且 pack 不为 NULL</param>
/// <param name="format">期望的数据格式（文本或二进制），用于后续字段解析</param>
/// <returns>pgsql_reader_ctx 指针，失败返回 NULL</returns>
pgsql_reader_ctx *pgsql_reader_init(pgpack_ctx *pgpack, pgpack_format format);
/// <summary>
/// 释放查询结果读取器及其持有的所有行数据
/// </summary>
/// <param name="reader">pgsql_reader_ctx 指针</param>
void pgsql_reader_free(pgsql_reader_ctx *reader);
/// <summary>
/// 获取结果集的总行数
/// </summary>
/// <param name="reader">pgsql_reader_ctx 指针</param>
/// <returns>结果集行数</returns>
size_t pgsql_reader_size(pgsql_reader_ctx *reader);
/// <summary>
/// 将游标跳转到指定行位置
/// </summary>
/// <param name="reader">pgsql_reader_ctx 指针</param>
/// <param name="pos">目标行索引（从 0 开始），超出范围时不移动</param>
void pgsql_reader_seek(pgsql_reader_ctx *reader, size_t pos);
/// <summary>
/// 判断游标是否已到达结果集末尾
/// </summary>
/// <param name="reader">pgsql_reader_ctx 指针</param>
/// <returns>1 表示已到末尾（无数据），0 表示还有数据</returns>
int32_t pgsql_reader_eof(pgsql_reader_ctx *reader);
/// <summary>
/// 将游标移动到下一行
/// </summary>
/// <param name="reader">pgsql_reader_ctx 指针</param>
void pgsql_reader_next(pgsql_reader_ctx *reader);
/// <summary>
/// 按列索引获取当前行中指定列的值
/// </summary>
/// <param name="reader">pgsql_reader_ctx 指针</param>
/// <param name="index">列索引（从 0 开始）</param>
/// <param name="field">输出字段描述指针，可为 NULL</param>
/// <returns>pgpack_row 指针，游标越界或列索引无效时返回 NULL</returns>
pgpack_row *pgsql_reader_index(pgsql_reader_ctx *reader, int16_t index, pgpack_field **field);
/// <summary>
/// 按列名获取当前行中指定列的值
/// </summary>
/// <param name="reader">pgsql_reader_ctx 指针</param>
/// <param name="name">列名字符串</param>
/// <param name="field">输出字段描述指针，可为 NULL</param>
/// <returns>pgpack_row 指针，列名不存在时返回 NULL</returns>
pgpack_row *pgsql_reader_name(pgsql_reader_ctx *reader, const char *name, pgpack_field **field);
/// <summary>
/// 按列名读取当前行中布尔类型字段的值
/// </summary>
/// <param name="reader">pgsql_reader_ctx 指针</param>
/// <param name="name">列名字符串（字段类型必须为 BOOLOID）</param>
/// <param name="err">输出错误码：ERR_OK 成功，1 为 NULL 值，ERR_FAILED 失败</param>
/// <returns>布尔值（0 或 1），出错时返回 0</returns>
int32_t pgsql_reader_bool(pgsql_reader_ctx *reader, const char *name, int32_t *err);
/// <summary>
/// 按列名读取当前行中整数类型字段的值（支持 int2/int4/int8）
/// </summary>
/// <param name="reader">pgsql_reader_ctx 指针</param>
/// <param name="name">列名字符串</param>
/// <param name="err">输出错误码：ERR_OK 成功，1 为 NULL 值，ERR_FAILED 失败</param>
/// <returns>int64_t 整数值，出错时返回 0</returns>
int64_t pgsql_reader_integer(pgsql_reader_ctx *reader, const char *name, int32_t *err);
/// <summary>
/// 按列名读取当前行中浮点类型字段的值（支持 float4/float8）
/// </summary>
/// <param name="reader">pgsql_reader_ctx 指针</param>
/// <param name="name">列名字符串</param>
/// <param name="err">输出错误码：ERR_OK 成功，1 为 NULL 值，ERR_FAILED 失败</param>
/// <returns>double 浮点值，出错时返回 0.0</returns>
double pgsql_reader_double(pgsql_reader_ctx *reader, const char *name, int32_t *err);
/// <summary>
/// 判断当前行指定列是否为 NULL
/// </summary>
/// <param name="reader">pgsql_reader_ctx 指针</param>
/// <param name="name">列名字符串</param>
/// <returns>1 为 NULL，0 为非 NULL 或列不存在</returns>
int32_t pgsql_reader_isnull(pgsql_reader_ctx *reader, const char *name);
/// <summary>
/// 按列名读取当前行中文本类型字段的值（支持 TEXT / VARCHAR / BPCHAR / NAME / UNKNOWN）
/// 返回指向行内部缓冲区的指针，不含 '\0' 结尾，生命周期与 pgsql_reader_ctx 相同
/// </summary>
/// <param name="reader">pgsql_reader_ctx 指针</param>
/// <param name="name">列名字符串</param>
/// <param name="lens">输出字节长度</param>
/// <param name="err">输出错误码：ERR_OK 成功，1 为 NULL 值，ERR_FAILED 失败</param>
/// <returns>UTF-8 字节指针（非 NULL 结尾），出错时返回 NULL</returns>
const char *pgsql_reader_text(pgsql_reader_ctx *reader, const char *name, int32_t *lens, int32_t *err);
/// <summary>
/// 按列名读取当前行中 BYTEA 类型字段的值
/// 二进制格式：返回原始字节指针；文本格式：返回 '\x' 前缀十六进制字符串（调用方自行解码）
/// 返回指向行内部缓冲区的指针，生命周期与 pgsql_reader_ctx 相同
/// </summary>
/// <param name="reader">pgsql_reader_ctx 指针</param>
/// <param name="name">列名字符串</param>
/// <param name="lens">输出字节长度</param>
/// <param name="err">输出错误码：ERR_OK 成功，1 为 NULL 值，ERR_FAILED 失败</param>
/// <returns>字节指针，出错时返回 NULL</returns>
const char *pgsql_reader_bytea(pgsql_reader_ctx *reader, const char *name, int32_t *lens, int32_t *err);
/// <summary>
/// 按列名读取当前行中 TIMESTAMP / TIMESTAMPTZ 类型字段的值
/// 二进制格式：直接解包大端序 int64；文本格式：解析 "YYYY-MM-DD HH:MM:SS[.ffffff]"
/// </summary>
/// <param name="reader">pgsql_reader_ctx 指针</param>
/// <param name="name">列名字符串</param>
/// <param name="err">输出错误码：ERR_OK 成功，1 为 NULL 值，ERR_FAILED 失败</param>
/// <returns>相对 PostgreSQL 纪元（2000-01-01 00:00:00）的微秒数，出错时返回 0</returns>
int64_t pgsql_reader_timestamp(pgsql_reader_ctx *reader, const char *name, int32_t *err);
/// <summary>
/// 按列名读取当前行中 DATE 类型字段的值
/// 二进制格式：直接解包大端序 int32；文本格式：解析 "YYYY-MM-DD"
/// </summary>
/// <param name="reader">pgsql_reader_ctx 指针</param>
/// <param name="name">列名字符串</param>
/// <param name="err">输出错误码：ERR_OK 成功，1 为 NULL 值，ERR_FAILED 失败</param>
/// <returns>相对 PostgreSQL 纪元（2000-01-01）的天数，出错时返回 0</returns>
int32_t pgsql_reader_date(pgsql_reader_ctx *reader, const char *name, int32_t *err);
/// <summary>
/// 按列名读取当前行中 UUID 类型字段的值，写入 16 字节缓冲区
/// 二进制格式：直接复制 16 字节；文本格式：解析 "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
/// </summary>
/// <param name="reader">pgsql_reader_ctx 指针</param>
/// <param name="name">列名字符串</param>
/// <param name="uuid">输出缓冲区，至少 16 字节</param>
/// <param name="err">输出错误码：ERR_OK 成功，1 为 NULL 值，ERR_FAILED 失败</param>
/// <returns>ERR_OK 成功，ERR_FAILED 失败（uuid 内容无效）</returns>
int32_t pgsql_reader_uuid(pgsql_reader_ctx *reader, const char *name, char uuid[16], int32_t *err);

#endif//PGSQL_READER_H_
