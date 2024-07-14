#ifndef MYSQL_READER_H_
#define MYSQL_READER_H_

#include "protocol/mysql/mysql.h"

/// <summary>
/// mysql_reader 初始化
/// </summary>
/// <param name="mpack">mpack_ctx</param>
/// <returns>mysql_reader_ctx NULL 失败</returns>
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
/// 第几条数据
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
/// 根据名称获取字段浮点数值
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
/// <returns>时间戳</returns>
uint64_t mysql_reader_datetime(mysql_reader_ctx *reader, const char *name, int32_t *err);
/// <summary>
/// 根据名称获取字段时间值
/// </summary>
/// <param name="reader">mysql_reader_ctx</param>
/// <param name="name">字段</param>
/// <param name="time">struct tm</param>
/// <param name="err">ERR_OK 成功  ERR_FAILED 失败 1 nil</param>
/// <returns>1负 0 正</returns>
int32_t mysql_reader_time(mysql_reader_ctx *reader, const char *name, struct tm *time, int32_t *err);

#endif//MYSQL_READER_H_
