#ifndef MYSQL_READER_H_
#define MYSQL_READER_H_

#include "protocol/mysql/mysql.h"

// 各取值接口收哪些列类型：按类型查一次表得到它属于哪组，取值接口只认自己那组。
// 有符号与无符号整数读取共用 MYSQL_CLS_INT，该用哪个看列的 UNSIGNED 标志(mysql_reader_unsigned)
#define MYSQL_CLS_INT      0x01
#define MYSQL_CLS_FLOAT    0x02
#define MYSQL_CLS_DOUBLE   0x04
#define MYSQL_CLS_STRING   0x08
#define MYSQL_CLS_DATETIME 0x10
#define MYSQL_CLS_TIME     0x20

/// <summary>
/// 取出结果集读取器
/// </summary>
/// <param name="mpack">mpack_ctx；成功后其内部结果集已被摘走，框架的 mpack 回收不再释放它</param>
/// <returns>mysql_reader_ctx（所有权归调用方，必须用 mysql_reader_free 释放，否则整份结果集泄漏）；
/// 包类型不是 MPACK_QUERY / MPACK_STMT_EXECUTE，或结果集已被取走时返回 NULL</returns>
mysql_reader_ctx *mysql_reader_init(mpack_ctx *mpack);
/// <summary>
/// mysql_reader 释放
/// </summary>
/// <param name="reader">mysql_reader_ctx</param>
void mysql_reader_free(mysql_reader_ctx *reader);
/// <summary>
/// 有多少条数据
/// </summary>
/// <param name="reader">mysql_reader_ctx</param>
/// <returns>数据条数</returns>
size_t mysql_reader_size(mysql_reader_ctx *reader);
/// <summary>
/// 移到第几条数据
/// </summary>
/// <param name="reader">mysql_reader_ctx</param>
/// <param name="pos">条数</param>
void mysql_reader_seek(mysql_reader_ctx *reader, size_t pos);
/// <summary>
/// 是否有数据
/// </summary>
/// <param name="reader">mysql_reader_ctx</param>
/// <returns>1 无数据 0 有数据</returns>
int32_t mysql_reader_eof(mysql_reader_ctx *reader);
/// <summary>
/// 下一条数据
/// </summary>
/// <param name="reader">mysql_reader_ctx</param>
void mysql_reader_next(mysql_reader_ctx *reader);
/// <summary>
/// 按列名取该列属于哪个取值分组，给按列类型自动选取值接口的调用方用
/// </summary>
/// <param name="reader">mysql_reader_ctx</param>
/// <param name="name">字段名</param>
/// <returns>MYSQL_CLS_* 之一；没有这一列或列类型不在任何分组时为 0</returns>
uint8_t mysql_reader_cls(mysql_reader_ctx *reader, const char *name);
/// <summary>
/// 按列名取该列是否带 UNSIGNED 标志，给在 mysql_reader_integer / mysql_reader_uinteger 之间选的调用方用
/// </summary>
/// <param name="reader">mysql_reader_ctx</param>
/// <param name="name">字段名</param>
/// <returns>1 带 UNSIGNED 标志；0 不带或没有这一列</returns>
int32_t mysql_reader_unsigned(mysql_reader_ctx *reader, const char *name);
/// <summary>
/// 当前行指定列是否为 SQL NULL，不看列类型（不在任何取值分组的列也能判）
/// </summary>
/// <param name="reader">mysql_reader_ctx</param>
/// <param name="name">字段名</param>
/// <returns>1 为 NULL；0 为非 NULL、没有这一列或没有当前行</returns>
int32_t mysql_reader_isnull(mysql_reader_ctx *reader, const char *name);
/// <summary>
/// 根据名称获取字段整数值
/// </summary>
/// <param name="reader">mysql_reader_ctx</param>
/// <param name="name">字段</param>
/// <param name="err">ERR_OK 成功  ERR_FAILED 失败 1 nil</param>
/// <returns>值</returns>
int64_t mysql_reader_integer(mysql_reader_ctx *reader, const char *name, int32_t *err);
/// <summary>
/// 根据名称获取字段无符号整数值
/// </summary>
/// <param name="reader">mysql_reader_ctx</param>
/// <param name="name">字段</param>
/// <param name="err">ERR_OK 成功  ERR_FAILED 失败 1 nil</param>
/// <returns>值</returns>
uint64_t mysql_reader_uinteger(mysql_reader_ctx *reader, const char *name, int32_t *err);
/// <summary>
/// 根据名称获取字段单精度浮点值
/// </summary>
/// <param name="reader">mysql_reader_ctx</param>
/// <param name="name">字段</param>
/// <param name="err">ERR_OK 成功  ERR_FAILED 失败 1 nil</param>
/// <returns>值</returns>
float mysql_reader_float(mysql_reader_ctx *reader, const char *name, int32_t *err);
/// <summary>
/// 根据名称获取字段双精度浮点值
/// </summary>
/// <param name="reader">mysql_reader_ctx</param>
/// <param name="name">字段</param>
/// <param name="err">ERR_OK 成功  ERR_FAILED 失败 1 nil</param>
/// <returns>值</returns>
double mysql_reader_double(mysql_reader_ctx *reader, const char *name, int32_t *err);
/// <summary>
/// 根据名称获取字段字符串值
/// </summary>
/// <param name="reader">mysql_reader_ctx</param>
/// <param name="name">字段</param>
/// <param name="lens">长度</param>
/// <param name="err">ERR_OK 成功  ERR_FAILED 失败 1 nil</param>
/// <returns>值</returns>
char *mysql_reader_string(mysql_reader_ctx *reader, const char *name, size_t *lens, int32_t *err);
/// <summary>
/// 根据名称获取字段时间戳值
/// </summary>
/// <param name="reader">mysql_reader_ctx</param>
/// <param name="name">字段</param>
/// <param name="err">ERR_OK 成功  ERR_FAILED 失败 1 nil</param>
/// <returns>Unix 微秒时间戳（秒×1000000＋微秒）</returns>
int64_t mysql_reader_datetime(mysql_reader_ctx *reader, const char *name, int32_t *err);
/// <summary>
/// 根据名称获取字段时间值
/// </summary>
/// <param name="reader">mysql_reader_ctx</param>
/// <param name="name">字段</param>
/// <param name="time">struct tm（tm_mday 存天数）；必须非 NULL，取不到值时被清零</param>
/// <param name="usec">微秒分量（0~999999）；必须非 NULL，取不到值时被清零</param>
/// <param name="err">ERR_OK 成功  ERR_FAILED 失败 1 nil</param>
/// <returns>1负 0 正</returns>
int32_t mysql_reader_time(mysql_reader_ctx *reader, const char *name, struct tm *time, uint32_t *usec, int32_t *err);

#endif//MYSQL_READER_H_
