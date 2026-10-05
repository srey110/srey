#ifndef PGSQL_READER_H_
#define PGSQL_READER_H_

#include "protocol/pgsql/pgsql_struct.h"

/// <summary>
/// 逐条取出带结果集的语句的读取器：每调一次给下一个。
/// 每个结果只能被取走一次，所以反复调用即可遍历整个响应；内部游标只前进不回退，与
/// pgsql_reader_at 混用也不会重复吐出同一个；BEGIN / SET 这类没有结果集的
/// 语句自动跳过。要按语句序号定位某一条用 pgsql_reader_at
/// </summary>
/// <param name="pgpack">pgpack_ctx 指针，类型必须为 PGPACK_OK</param>
/// <param name="format">期望的数据格式（文本或二进制），用于后续字段解析</param>
/// <returns>pgsql_reader_ctx 指针（所有权归调用方，用 pgsql_reader_free 释放）；
/// 类型不符或已无未取走的结果集时返回 NULL</returns>
pgsql_reader_ctx *pgsql_reader_iter(pgpack_ctx *pgpack, pgpack_format format);
/// <summary>
/// 从 pgpack_ctx 中取出第 idx 个查询结果的读取器；
/// 多语句 simple query 每条语句一个结果，下标按语句顺序，结果总数见 pgsql_result_count
/// </summary>
/// <param name="pgpack">pgpack_ctx 指针，类型必须为 PGPACK_OK</param>
/// <param name="idx">结果下标，从 0 开始</param>
/// <param name="format">期望的数据格式（文本或二进制），用于后续字段解析</param>
/// <returns>pgsql_reader_ctx 指针（所有权归调用方，用 pgsql_reader_free 释放，同一下标只能取走一次）；
/// 类型不符、下标越界、该语句无结果集（INSERT/UPDATE 无 RETURNING）或已被取走时返回 NULL</returns>
pgsql_reader_ctx *pgsql_reader_at(pgpack_ctx *pgpack, uint32_t idx, pgpack_format format);
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
/// 按列名取列下标：同一列要连取几次时先取一次下标，之后走 pgsql_reader_index 或 *_at 系列，不再每次按名扫列
/// </summary>
/// <param name="reader">pgsql_reader_ctx 指针</param>
/// <param name="name">列名，按 nlens 整段比较，不要求 '\0' 结尾</param>
/// <param name="nlens">列名字节数</param>
/// <returns>列下标；没有这一列或没有字段描述为 -1，重名列取第一个</returns>
int16_t pgsql_reader_col(pgsql_reader_ctx *reader, const char *name, size_t nlens);
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
/// </summary>
/// <param name="reader">pgsql_reader_ctx 指针</param>
/// <param name="name">列名字符串</param>
/// <param name="lens">输出字节长度</param>
/// <param name="err">输出错误码：ERR_OK 成功，1 为 NULL 值，ERR_FAILED 失败</param>
/// <returns>UTF-8 字节指针（不含 '\0' 结尾，指向行内部缓冲区，生命周期与 pgsql_reader_ctx 相同）；
/// 出错时返回 NULL</returns>
const char *pgsql_reader_text(pgsql_reader_ctx *reader, const char *name, int32_t *lens, int32_t *err);
/// <summary>
/// 按列名读取当前行中 BYTEA 类型字段的值：
/// 二进制格式给原始字节，文本格式给 '\x' 前缀十六进制字符串（调用方自行解码）
/// </summary>
/// <param name="reader">pgsql_reader_ctx 指针</param>
/// <param name="name">列名字符串</param>
/// <param name="lens">输出字节长度</param>
/// <param name="err">输出错误码：ERR_OK 成功，1 为 NULL 值，ERR_FAILED 失败</param>
/// <returns>字节指针（指向行内部缓冲区，生命周期与 pgsql_reader_ctx 相同）；出错时返回 NULL</returns>
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
/// <summary>
/// 同 pgsql_reader_bool，按列下标取
/// </summary>
/// <param name="reader">pgsql_reader_ctx 指针</param>
/// <param name="col">列下标（pgsql_reader_col 的结果）；不在 [0, 列数) 内（含 -1）按列不存在处理</param>
/// <param name="err">同 pgsql_reader_bool</param>
/// <returns>同 pgsql_reader_bool</returns>
int32_t pgsql_reader_bool_at(pgsql_reader_ctx *reader, int16_t col, int32_t *err);
/// <summary>
/// 同 pgsql_reader_integer，按列下标取
/// </summary>
/// <param name="reader">pgsql_reader_ctx 指针</param>
/// <param name="col">同 pgsql_reader_bool_at</param>
/// <param name="err">同 pgsql_reader_integer</param>
/// <returns>同 pgsql_reader_integer</returns>
int64_t pgsql_reader_integer_at(pgsql_reader_ctx *reader, int16_t col, int32_t *err);
/// <summary>
/// 同 pgsql_reader_double，按列下标取
/// </summary>
/// <param name="reader">pgsql_reader_ctx 指针</param>
/// <param name="col">同 pgsql_reader_bool_at</param>
/// <param name="err">同 pgsql_reader_double</param>
/// <returns>同 pgsql_reader_double</returns>
double pgsql_reader_double_at(pgsql_reader_ctx *reader, int16_t col, int32_t *err);
/// <summary>
/// 同 pgsql_reader_isnull，按列下标取
/// </summary>
/// <param name="reader">pgsql_reader_ctx 指针</param>
/// <param name="col">同 pgsql_reader_bool_at</param>
/// <returns>同 pgsql_reader_isnull</returns>
int32_t pgsql_reader_isnull_at(pgsql_reader_ctx *reader, int16_t col);
/// <summary>
/// 同 pgsql_reader_text，按列下标取
/// </summary>
/// <param name="reader">pgsql_reader_ctx 指针</param>
/// <param name="col">同 pgsql_reader_bool_at</param>
/// <param name="lens">同 pgsql_reader_text</param>
/// <param name="err">同 pgsql_reader_text</param>
/// <returns>同 pgsql_reader_text</returns>
const char *pgsql_reader_text_at(pgsql_reader_ctx *reader, int16_t col, int32_t *lens, int32_t *err);
/// <summary>
/// 同 pgsql_reader_bytea，按列下标取
/// </summary>
/// <param name="reader">pgsql_reader_ctx 指针</param>
/// <param name="col">同 pgsql_reader_bool_at</param>
/// <param name="lens">同 pgsql_reader_bytea</param>
/// <param name="err">同 pgsql_reader_bytea</param>
/// <returns>同 pgsql_reader_bytea</returns>
const char *pgsql_reader_bytea_at(pgsql_reader_ctx *reader, int16_t col, int32_t *lens, int32_t *err);
/// <summary>
/// 同 pgsql_reader_timestamp，按列下标取
/// </summary>
/// <param name="reader">pgsql_reader_ctx 指针</param>
/// <param name="col">同 pgsql_reader_bool_at</param>
/// <param name="err">同 pgsql_reader_timestamp</param>
/// <returns>同 pgsql_reader_timestamp</returns>
int64_t pgsql_reader_timestamp_at(pgsql_reader_ctx *reader, int16_t col, int32_t *err);
/// <summary>
/// 同 pgsql_reader_date，按列下标取
/// </summary>
/// <param name="reader">pgsql_reader_ctx 指针</param>
/// <param name="col">同 pgsql_reader_bool_at</param>
/// <param name="err">同 pgsql_reader_date</param>
/// <returns>同 pgsql_reader_date</returns>
int32_t pgsql_reader_date_at(pgsql_reader_ctx *reader, int16_t col, int32_t *err);
/// <summary>
/// 同 pgsql_reader_uuid，按列下标取
/// </summary>
/// <param name="reader">pgsql_reader_ctx 指针</param>
/// <param name="col">同 pgsql_reader_bool_at</param>
/// <param name="uuid">同 pgsql_reader_uuid</param>
/// <param name="err">同 pgsql_reader_uuid</param>
/// <returns>同 pgsql_reader_uuid</returns>
int32_t pgsql_reader_uuid_at(pgsql_reader_ctx *reader, int16_t col, char uuid[16], int32_t *err);

#endif//PGSQL_READER_H_
