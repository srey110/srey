#ifndef PGSQL_H_
#define PGSQL_H_

#include "protocol/pgsql/pgsql_pack.h"

void _pgsql_init(void *hspush);
void _pgsql_pkfree(void *pack);
void _pgsql_udfree(ud_cxt *ud);
void _pgsql_closed(ud_cxt *ud);
int32_t _pgsql_on_connected(ev_ctx *ev, SOCKET fd, uint64_t skid, ud_cxt *ud, int32_t err);
int32_t _pgsql_ssl_exchanged(ev_ctx *ev, ud_cxt *ud);
int32_t _pgsql_may_resume(void *data);
void *pgsql_unpack(ev_ctx *ev, buffer_ctx *buf, ud_cxt *ud, int32_t *status);
/// <summary>
/// pgsql参数初始化
/// </summary>
/// <param name="pg">pgsql_ctx</param>
/// <param name="ip">IP</param>
/// <param name="port">端口 0:5432</param>
/// <param name="evssl">evssl_ctx</param>
/// <param name="user">用户名</param>
/// <param name="password">密码</param>
/// <param name="database">数据库名</param>
/// <returns>ERR_OK 成功</returns>
int32_t pgsql_init(pgsql_ctx *pg, const char *ip, uint16_t port, struct evssl_ctx *evssl,
    const char *user, const char *password, const char *database);
/// <summary>
/// 链接数据库
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="pg">pgsql_ctx</param>
/// <returns>ERR_OK 请求成功</returns>
int32_t pgsql_try_connect(task_ctx *task, pgsql_ctx *pg);
/// <summary>
/// 设置用户名 密码
/// </summary>
/// <param name="pg">pgsql_ctx</param>
/// <param name="user">用户名</param>
/// <param name="password">密码</param>
void pgsql_set_userpwd(pgsql_ctx *pg, const char *user, const char *password);
/// <summary>
/// 设置数据库名
/// </summary>
/// <param name="pg">pgsql_ctx</param>
/// <param name="database">数据库名</param>
void pgsql_set_db(pgsql_ctx *pg, const char *database);
/// <summary>
/// 获取数据库名
/// </summary>
/// <param name="pg">pgsql_ctx</param>
/// <returns>数据库名</returns>
const char *pgsql_get_db(pgsql_ctx *pg);
/// <summary>
/// 命令执行影响的行数
/// </summary>
/// <param name="pgpack">pgpack_ctx</param>
/// <returns>行数</returns>
int32_t pgsql_affected_rows(pgpack_ctx *pgpack);

#endif//PGSQL_H_
