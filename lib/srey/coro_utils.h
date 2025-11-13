#ifndef CORO_UTILS_H_
#define CORO_UTILS_H_

#include "srey/spub.h"
#include "protocol/mysql/mysql.h"
#include "protocol/smtp.h"

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
SOCKET wbsock_connect(task_ctx *task, struct evssl_ctx *evssl, const char *ws, const char *secproto, uint64_t *skid, int32_t netev);
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
/// <param name="task">task_ctx</param>
/// <param name="mysql">mysql_ctx</param>
/// <param name="mysql">数据库</param>
/// <returns>ERR_OK 成功</returns>
int32_t mysql_selectdb(task_ctx *task, mysql_ctx *mysql, const char *database);
/// <summary>
/// ping
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="mysql">mysql_ctx</param>
/// <returns>ERR_OK 成功</returns>
int32_t mysql_ping(task_ctx *task, mysql_ctx *mysql);
/// <summary>
/// 执行SQL语句
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="mysql">mysql_ctx</param>
/// <param name="sql">SQL语句</param>
/// <param name="mbind">mysql_bind_ctx</param>
/// <returns>mpack_ctx NULL 失败</returns>
mpack_ctx *mysql_query(task_ctx *task, mysql_ctx *mysql, const char *sql, mysql_bind_ctx *mbind);
/// <summary>
/// 预处理
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="mysql">mysql_ctx</param>
/// <param name="sql">SQL语句</param>
/// <returns>mysql_stmt_ctx NULL 失败</returns>
mysql_stmt_ctx *mysql_stmt_prepare(task_ctx *task, mysql_ctx *mysql, const char *sql);
/// <summary>
/// 预处理执行
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="stmt">mysql_stmt_ctx</param>
/// <param name="mbind">mysql_bind_ctx</param>
/// <returns>mpack_ctx NULL 失败</returns>
mpack_ctx *mysql_stmt_execute(task_ctx *task, mysql_stmt_ctx *stmt, mysql_bind_ctx *mbind);
/// <summary>
/// 预处理重置
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="stmt">mysql_stmt_ctx</param>
/// <returns>ERR_OK 成功</returns>
int32_t mysql_stmt_reset(task_ctx *task, mysql_stmt_ctx *stmt);
/// <summary>
/// 退出关闭链接
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="mysql">mysql_ctx</param>
void mysql_quit(task_ctx *task, mysql_ctx *mysql);

/// <summary>
/// 电子邮件建立链接
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="smtp">smtp_ctx</param>
/// <returns>ERR_OK 成功</returns>
int32_t smtp_connect(task_ctx *task, smtp_ctx *smtp);
/// <summary>
/// 电子邮件信息清空
/// </summary>
/// <param name="smtp">smtp_ctx</param>
/// <returns>ERR_OK 成功</returns>
int32_t smtp_reset(smtp_ctx *smtp);
/// <summary>
/// 邮件关闭
/// </summary>
/// <param name="smtp">smtp_ctx</param>
/// <returns>ERR_OK 成功</returns>
int32_t smtp_quit(smtp_ctx *smtp);
/// <summary>
/// 邮件发送
/// </summary>
/// <param name="smtp">smtp_ctx</param>
/// <param name="from">发件人地址</param>
/// <param name="rcpt">收件人地址</param>
/// <param name="subject">标题</param>
/// <param name="data">内容</param>
/// <returns>ERR_OK 成功</returns>
int32_t smtp_send(smtp_ctx *smtp, const char *from, const char *rcpt, const char *subject, const char *data);

#endif//CORO_UTILS_H_
