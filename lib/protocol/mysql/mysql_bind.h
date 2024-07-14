#ifndef MYSQL_BIND_H_
#define MYSQL_BIND_H_

#include "protocol/mysql/mysql_struct.h"

/// <summary>
/// 参数绑定 初始化
/// </summary>
/// <param name="mbind">mysql_bind_ctx</param>
void mysql_bind_init(mysql_bind_ctx *mbind);
/// <summary>
/// 参数绑定 释放
/// </summary>
/// <param name="mbind">mysql_bind_ctx</param>
void mysql_bind_free(mysql_bind_ctx *mbind);
/// <summary>
/// 参数绑定 重置
/// </summary>
/// <param name="mbind">mysql_bind_ctx</param>
void mysql_bind_clear(mysql_bind_ctx *mbind);
/// <summary>
/// MYSQL_TYPE_NULL
/// </summary>
/// <param name="mbind">mysql_bind_ctx</param>
/// <param name="name">名称</param>
void mysql_bind_nil(mysql_bind_ctx *mbind, const char *name);
/// <summary>
/// MYSQL_TYPE_STRING MYSQL_TYPE_VARCHAR MYSQL_TYPE_VAR_STRING MYSQL_TYPE_ENUM MYSQL_TYPE_SET
/// MYSQL_TYPE_LONG_BLOB MYSQL_TYPE_MEDIUM_BLOB MYSQL_TYPE_BLOB MYSQL_TYPE_TINY_BLOB
/// MYSQL_TYPE_GEOMETRY MYSQL_TYPE_BIT MYSQL_TYPE_DECIMAL MYSQL_TYPE_NEWDECIMAL MYSQL_TYPE_JSON
/// </summary>
/// <param name="mbind">mysql_bind_ctx</param>
/// <param name="name">名称</param>
/// <param name="value">数据</param>
/// <param name="lens">长度</param>
void mysql_bind_string(mysql_bind_ctx *mbind, const char *name, char *value, size_t lens);
/// <summary>
/// MYSQL_TYPE_TINY MYSQL_TYPE_SHORT MYSQL_TYPE_YEAR MYSQL_TYPE_LONG MYSQL_TYPE_LONGLONG
/// </summary>
/// <param name="mbind">mysql_bind_ctx</param>
/// <param name="name">名称</param>
/// <param name="value">值</param>
void mysql_bind_integer(mysql_bind_ctx *mbind, const char *name, int64_t value);
void mysql_bind_uinteger(mysql_bind_ctx *mbind, const char *name, uint64_t value);
/// <summary>
/// MYSQL_TYPE_FLOAT MYSQL_TYPE_DOUBLE
/// </summary>
/// <param name="mbind">mysql_bind_ctx</param>
/// <param name="name">名称</param>
/// <param name="value">值</param>
void mysql_bind_double(mysql_bind_ctx *mbind, const char *name, double value);
/// <summary>
/// MYSQL_TYPE_DATE MYSQL_TYPE_DATETIME MYSQL_TYPE_TIMESTAMP
/// </summary>
/// <param name="mbind">mysql_bind_ctx</param>
/// <param name="name">名称</param>
/// <param name="ts">时间戳</param>
void mysql_bind_datetime(mysql_bind_ctx *mbind, const char *name, time_t ts);
/// <summary>
/// MYSQL_TYPE_TIME
/// </summary>
/// <param name="mbind">mysql_bind_ctx</param>
/// <param name="name">名称</param>
/// <param name="is_negative">1负 0 正</param>
/// <param name="days">天</param>
/// <param name="hour">小时</param>
/// <param name="minute">分</param>
/// <param name="second">秒</param>
void mysql_bind_time(mysql_bind_ctx *mbind, const char *name,
    int8_t is_negative, int32_t days, int8_t hour, int8_t minute, int8_t second);

#endif//MYSQL_BIND_H_
