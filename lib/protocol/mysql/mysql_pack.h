#ifndef MYSQL_PACK_H_
#define MYSQL_PACK_H_

#include "protocol/mysql/mysql_bind.h"

/// <summary>
/// 构造 COM_QUIT 请求包（断开连接）
/// </summary>
/// <param name="mysql">mysql_ctx</param>
/// <param name="size">输出包大小（字节）</param>
/// <returns>请求包数据，调用方负责释放</returns>
void *mysql_pack_quit(mysql_ctx *mysql, size_t *size);
/// <summary>
/// 构造 COM_INIT_DB 请求包（切换数据库）
/// </summary>
/// <param name="mysql">mysql_ctx</param>
/// <param name="database">目标数据库名</param>
/// <param name="size">输出包大小（字节）</param>
/// <returns>请求包数据，调用方负责释放</returns>
void *mysql_pack_selectdb(mysql_ctx *mysql, const char *database, size_t *size);
/// <summary>
/// 构造 COM_PING 请求包（检测连接是否存活）
/// </summary>
/// <param name="mysql">mysql_ctx</param>
/// <param name="size">输出包大小（字节）</param>
/// <returns>请求包数据，调用方负责释放</returns>
void *mysql_pack_ping(mysql_ctx *mysql, size_t *size);
/// <summary>
/// 构造 COM_QUERY 请求包（执行 SQL 语句）
/// </summary>
/// <param name="mysql">mysql_ctx</param>
/// <param name="sql">SQL 语句字符串</param>
/// <param name="mbind">查询属性参数绑定，NULL 表示无参数</param>
/// <param name="size">输出包大小（字节）</param>
/// <returns>请求包数据，调用方负责释放</returns>
void *mysql_pack_query(mysql_ctx *mysql, const char *sql, mysql_bind_ctx *mbind, size_t *size);
/// <summary>
/// 构造 COM_STMT_PREPARE 请求包（预处理语句准备）
/// </summary>
/// <param name="mysql">mysql_ctx</param>
/// <param name="sql">SQL 语句字符串</param>
/// <param name="size">输出包大小（字节）</param>
/// <returns>请求包数据，调用方负责释放</returns>
void *mysql_pack_stmt_prepare(mysql_ctx *mysql, const char *sql, size_t *size);
/// <summary>
/// 构造 COM_STMT_EXECUTE 请求包（预处理语句执行）
/// </summary>
/// <param name="stmt">mysql_stmt_ctx</param>
/// <param name="mbind">绑定参数上下文，NULL 表示无参数</param>
/// <param name="size">输出包大小（字节）</param>
/// <returns>请求包数据，调用方负责释放</returns>
void *mysql_pack_stmt_execute(mysql_stmt_ctx *stmt, mysql_bind_ctx *mbind, size_t *size);
/// <summary>
/// 构造 COM_STMT_RESET 请求包（重置预处理语句状态）
/// </summary>
/// <param name="stmt">mysql_stmt_ctx</param>
/// <param name="size">输出包大小（字节）</param>
/// <returns>请求包数据，调用方负责释放</returns>
void *mysql_pack_stmt_reset(mysql_stmt_ctx *stmt, size_t *size);
/// <summary>
/// 构造 COM_STMT_CLOSE 请求包并释放语句资源
/// </summary>
/// <param name="stmt">mysql_stmt_ctx，调用后 stmt 将被释放，不可再使用</param>
/// <param name="size">输出包大小（字节）</param>
/// <returns>请求包数据，调用方负责释放</returns>
void *mysql_pack_stmt_close(mysql_stmt_ctx *stmt, size_t *size);

#endif//MYSQL_PACK_H_
