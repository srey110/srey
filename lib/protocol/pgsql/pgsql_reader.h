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

#endif//PGSQL_READER_H_
