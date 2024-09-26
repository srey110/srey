#ifndef MYSQL_H_
#define MYSQL_H_

#include "protocol/mysql/mysql_struct.h"
#include "event/event.h"
#include "srey/spub.h"

void _mysql_pkfree(void *pack);
void _mysql_init(void *hspush);
void _mysql_udfree(ud_cxt *ud);
void _mysql_closed(ud_cxt *ud);
int32_t _mysql_ssl_exchanged(ev_ctx *ev, ud_cxt *ud);
//mysql 验证 数据解包
void *mysql_unpack(ev_ctx *ev, buffer_ctx *buf, ud_cxt *ud, int32_t *status);
/// <summary>
/// mysql参数初始化
/// </summary>
/// <param name="mysql">mysql_ctx</param>
/// <param name="ip">IP</param>
/// <param name="port">端口 3306</param>
/// <param name="evssl">evssl_ctx 非NULL 启用SSL链接</param>
/// <param name="user">用户名</param>
/// <param name="password">密码</param>
/// <param name="database">数据库, NULL 或 "" 不设置</param>
/// <param name="charset">编码格式</param>
/// <param name="maxpk">最大数据包, 0: ONEK * ONEK</param>
/// <param name="relink">1 ping的时候自动重链</param>
/// <returns>ERR_OK 成功</returns>
int32_t mysql_init(mysql_ctx *mysql, const char *ip, uint16_t port, struct evssl_ctx *evssl,
    const char *user, const char *password, const char *database, const char *charset, uint32_t maxpk, int32_t relink);
/// <summary>
/// 链接数据库
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="mysql">mysql_ctx</param>
/// <returns>ERR_OK 请求成功</returns>
int32_t mysql_try_connect(task_ctx *task, mysql_ctx *mysql);
/// <summary>
/// 获取版本信息
/// </summary>
/// <param name="mysql">mysql_ctx</param>
/// <returns>版本信息</returns>
const char *mysql_version(mysql_ctx *mysql);
/// <summary>
/// 获取错误信息
/// </summary>
/// <param name="mysql">mysql_ctx</param>
/// <param name="code">错误码, NULL不设置</param>
/// <returns>错误信息</returns>
const char *mysql_erro(mysql_ctx *mysql, int32_t *code);
/// <summary>
/// 清除错误信息
/// </summary>
/// <param name="mysql">mysql_ctx</param>
void mysql_erro_clear(mysql_ctx *mysql);
/// <summary>
/// 获取插入的ID
/// </summary>
/// <param name="mysql">mysql_ctx</param>
/// <returns>ID</returns>
int64_t mysql_last_id(mysql_ctx *mysql);
/// <summary>
/// 获取影响的行数
/// </summary>
/// <param name="mysql">mysql_ctx</param>
/// <returns>影响的行数</returns>
int64_t mysql_affected_rows(mysql_ctx *mysql);
/// <summary>
/// stmt 关闭
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="stmt">mysql_stmt_ctx</param>
void mysql_stmt_close(task_ctx *task, mysql_stmt_ctx *stmt);

#endif//MYSQL_H_
