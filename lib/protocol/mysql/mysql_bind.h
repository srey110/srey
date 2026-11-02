#ifndef MYSQL_BIND_H_
#define MYSQL_BIND_H_

#include "protocol/mysql/mysql_struct.h"

/// <summary>
/// 参数绑定 初始化
/// </summary>
/// <param name="mbind">mysql_bind_ctx</param>
void mysql_bind_init(mysql_bind_ctx *mbind);
/// <summary>
/// 参数绑定 释放。可重复调用；释放后再绑定参数等同于刚 init 的空上下文
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
/// 绑定字符串 / 二进制参数，一律按 MYSQL_TYPE_STRING 发，由服务端按列类型转换（VARCHAR、BLOB、ENUM、SET、
/// DECIMAL、JSON 等列都用它）。value 为 NULL 时同 mysql_bind_nil
/// </summary>
/// <param name="mbind">mysql_bind_ctx</param>
/// <param name="name">名称</param>
/// <param name="value">数据</param>
/// <param name="lens">长度</param>
void mysql_bind_string(mysql_bind_ctx *mbind, const char *name, char *value, size_t lens);
/// <summary>
/// 绑定有符号整数参数，按值的范围选 MYSQL_TYPE_TINY / SHORT / LONG / LONGLONG 中最小的
/// </summary>
/// <param name="mbind">mysql_bind_ctx</param>
/// <param name="name">名称</param>
/// <param name="value">值</param>
void mysql_bind_integer(mysql_bind_ctx *mbind, const char *name, int64_t value);
/// <summary>
/// 绑定无符号整数参数，按值的范围选 MYSQL_TYPE_TINY / SHORT / LONG / LONGLONG 中最小的
/// </summary>
/// <param name="mbind">mysql_bind_ctx</param>
/// <param name="name">参数名称</param>
/// <param name="value">无符号整数值</param>
void mysql_bind_uinteger(mysql_bind_ctx *mbind, const char *name, uint64_t value);
/// <summary>
/// 绑定单精度浮点参数 MYSQL_TYPE_FLOAT
/// </summary>
/// <param name="mbind">mysql_bind_ctx</param>
/// <param name="name">名称</param>
/// <param name="value">值</param>
void mysql_bind_float(mysql_bind_ctx *mbind, const char *name, float value);
/// <summary>
/// 绑定双精度浮点参数 MYSQL_TYPE_DOUBLE
/// </summary>
/// <param name="mbind">mysql_bind_ctx</param>
/// <param name="name">名称</param>
/// <param name="value">值</param>
void mysql_bind_double(mysql_bind_ctx *mbind, const char *name, double value);
/// <summary>
/// MYSQL_TYPE_DATE MYSQL_TYPE_DATETIME MYSQL_TYPE_TIMESTAMP。按本地时区换成日期时间，本地时间正好 0 点整时
/// 只发日期(4 字节)；本地时间换算失败(如 Windows 上 1970 年前或 3000 年后)或年份不在 0~9999 时按零日期发，不报错
/// </summary>
/// <param name="mbind">mysql_bind_ctx</param>
/// <param name="name">名称</param>
/// <param name="ts">Unix 时间戳（秒）</param>
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
