#ifndef CORO_UTILS_H_
#define CORO_UTILS_H_

#include "srey/spub.h"
#include "protocol/mysql/mysql.h"
#include "protocol/pgsql/pgsql.h"
#include "protocol/smtp/smtp.h"

/// <summary>
/// dns域名解析
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="domain">域名</param>
/// <param name="ipv6">1 ipv6 0 ipv4</param>
/// <param name="cnt">ip数量</param>
/// <returns>dns_ip 需要FREE</returns>
struct dns_ip *dns_lookup(task_ctx *task, const char *domain, int32_t ipv6, size_t *cnt);
/// <summary>
/// websocket链接
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="evssl">evssl_ctx</param>
/// <param name="ws">ws://host:port</param>
/// <param name="secproto">Sec-WebSocket-Protocol</param>
/// <param name="skid">链接ID</param>
/// <param name="netev">task_netev</param>
/// <returns>socket句柄</returns>
SOCKET wbsock_connect(task_ctx *task, struct evssl_ctx *evssl, const char *ws, const char *secprot, uint64_t *skid, int32_t netev);
/// <summary>
/// redis链接
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="evssl">evssl_ctx</param>
/// <param name="ip">IP</param>
/// <param name="port">端口</param>
/// <param name="key">密码</param>
/// <param name="skid">链接ID</param>
/// <param name="netev">task_netev</param>
/// <returns>socket句柄</returns>
SOCKET redis_connect(task_ctx *task, struct evssl_ctx *evssl, const char *ip, uint16_t port, const char *key, uint64_t *skid, int32_t netev);
/// <summary>
/// myql链接
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="mysql">mysql_ctx, mysql_init</param>
/// <returns>ERR_OK 成功</returns>
int32_t mysql_connect(task_ctx *task, mysql_ctx *mysql);
/// <summary>
/// 选择数据库
/// </summary>
/// <param name="mysql">mysql_ctx</param>
/// <param name="mysql">数据库</param>
/// <returns>ERR_OK 成功</returns>
int32_t mysql_selectdb(mysql_ctx *mysql, const char *database);
/// <summary>
/// ping
/// </summary>
/// <param name="mysql">mysql_ctx</param>
/// <returns>ERR_OK 成功</returns>
int32_t mysql_ping(mysql_ctx *mysql);
/// <summary>
/// 执行SQL语句
/// </summary>
/// <param name="mysql">mysql_ctx</param>
/// <param name="sql">SQL语句</param>
/// <param name="mbind">mysql_bind_ctx</param>
/// <returns>mpack_ctx NULL 失败</returns>
mpack_ctx *mysql_query(mysql_ctx *mysql, const char *sql, mysql_bind_ctx *mbind);
/// <summary>
/// 预处理
/// </summary>
/// <param name="mysql">mysql_ctx</param>
/// <param name="sql">SQL语句</param>
/// <returns>mysql_stmt_ctx NULL 失败</returns>
mysql_stmt_ctx *mysql_stmt_prepare(mysql_ctx *mysql, const char *sql);
/// <summary>
/// 预处理执行
/// </summary>
/// <param name="stmt">mysql_stmt_ctx</param>
/// <param name="mbind">mysql_bind_ctx</param>
/// <returns>mpack_ctx NULL 失败</returns>
mpack_ctx *mysql_stmt_execute(mysql_stmt_ctx *stmt, mysql_bind_ctx *mbind);
/// <summary>
/// 预处理重置
/// </summary>
/// <param name="stmt">mysql_stmt_ctx</param>
/// <returns>ERR_OK 成功</returns>
int32_t mysql_stmt_reset(mysql_stmt_ctx *stmt);
/// <summary>
/// 退出关闭链接
/// </summary>
/// <param name="mysql">mysql_ctx</param>
void mysql_quit(mysql_ctx *mysql);
/// <summary>
/// 电子邮件建立链接
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="smtp">smtp_ctx</param>
/// <returns>ERR_OK 成功</returns>
int32_t smtp_connect(task_ctx *task, smtp_ctx *smtp);
/// <summary>
/// 邮件关闭
/// </summary>
/// <param name="smtp">smtp_ctx</param>
void smtp_quit(smtp_ctx *smtp);
/// <summary>
/// ping测试
/// </summary>
/// <param name="smtp">smtp_ctx</param>
/// <returns>ERR_OK 成功</returns>
int32_t smtp_ping(smtp_ctx *smtp);
/// <summary>
/// 邮件发送
/// </summary>
/// <param name="smtp">smtp_ctx</param>
/// <param name="mail">mail_ctx</param>
/// <returns>ERR_OK 成功</returns>
int32_t smtp_send(smtp_ctx *smtp, mail_ctx *mail);
/// <summary>
/// pgsql链接
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="pg">pgsql_ctx, pgsql_init</param>
/// <returns>ERR_OK 成功</returns>
int32_t pgsql_connect(task_ctx *task, pgsql_ctx *pg);
/// <summary>
/// 关闭链接
/// </summary>
/// <param name="pg">pgsql_ctx</param>
void pgsql_quit(pgsql_ctx *pg);
/// <summary>
/// 选择数据库
/// </summary>
/// <param name="pg">pgsql_ctx</param>
/// <param name="database">数据库</param>
/// <returns>ERR_OK 成功</returns>
int32_t pgsql_selectdb(pgsql_ctx *pg, const char *database);
/// <summary>
/// ping
/// </summary>
/// <param name="pg">pgsql_ctx</param>
/// <returns>ERR_OK 成功</returns>
int32_t pgsql_ping(pgsql_ctx *pg);
/// <summary>
/// 执行SQL语句
/// </summary>
/// <param name="pg">pgsql_ctx</param>
/// <param name="sql">SQL语句</param>
/// <returns>NULL 失败  pgpack_ctx</returns>
pgpack_ctx *pgsql_query(pgsql_ctx *pg, const char *sql);
/// <summary>
/// 预处理
/// </summary>
/// <param name="pg">pgsql_ctx</param>
/// <param name="name">名称</param>
/// <param name="sql">sql语句</param>
/// <param name="nparam">参数数量</param>
/// <param name="oids">参数OID(pgsql_macro.h)</param>
/// <returns>ERR_OK 成功</returns>
int32_t pgsql_stmt_prepare(pgsql_ctx *pg, const char *name, const char *sql, int16_t nparam, uint32_t *oids);
/// <summary>
/// 预处理执行
/// </summary>
/// <param name="pg">pgsql_ctx</param>
/// <param name="name">名称</param>
/// <param name="bind">pgsql_bind_ctx</param>
/// <param name="resultformat">pgpack_format</param>
/// <returns>NULL 失败  pgpack_ctx</returns>
pgpack_ctx *pgsql_stmt_execute(pgsql_ctx *pg, const char *name, pgsql_bind_ctx *bind, pgpack_format resultformat);
/// <summary>
/// 预处理关闭
/// </summary>
/// <param name="pg">pgsql_ctx</param>
/// <param name="name">名称</param>
void pgsql_stmt_close(pgsql_ctx *pg, const char *name);

#endif//CORO_UTILS_H_
