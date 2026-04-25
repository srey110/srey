#ifndef PGSQL_H_
#define PGSQL_H_

#include "protocol/pgsql/pgsql_pack.h"

// 初始化握手完成推送回调（在协议注册时调用）
void _pgsql_init(void *hspush);
// 释放 pgpack_ctx 数据包
void _pgsql_pkfree(void *pack);
// 释放 ud_cxt 中绑定的 pgsql 上下文资源
void _pgsql_udfree(ud_cxt *ud);
// 连接关闭时的清理回调
void _pgsql_closed(ud_cxt *ud);
// 连接建立后发送 SSL 协商请求
int32_t _pgsql_on_connected(ev_ctx *ev, SOCKET fd, uint64_t skid, ud_cxt *ud, int32_t err);
// SSL 握手完成后发送 Startup 消息
int32_t _pgsql_ssl_exchanged(ev_ctx *ev, ud_cxt *ud);
// 判断当前数据包是否允许 task 恢复（通知包不允许立即恢复）
int32_t _pgsql_may_resume(void *data);
// 协议解包入口，根据连接状态分派 SSL/认证/命令响应处理
void *pgsql_unpack(ev_ctx *ev, buffer_ctx *buf, ud_cxt *ud, int32_t *status);
/// <summary>
/// 初始化 pgsql 连接参数
/// </summary>
/// <param name="pg">pgsql_ctx 指针</param>
/// <param name="ip">服务端 IP 地址</param>
/// <param name="port">服务端端口，0 表示使用默认端口 5432</param>
/// <param name="evssl">SSL 上下文，不使用 SSL 时为 NULL</param>
/// <param name="user">登录用户名</param>
/// <param name="password">登录密码</param>
/// <param name="database">目标数据库名</param>
/// <returns>ERR_OK 成功，ERR_FAILED 参数过长</returns>
int32_t pgsql_init(pgsql_ctx *pg, const char *ip, uint16_t port, struct evssl_ctx *evssl,
    const char *user, const char *password, const char *database);
/// <summary>
/// 发起到 pgsql 服务端的异步连接请求
/// </summary>
/// <param name="task">task_ctx 指针</param>
/// <param name="pg">pgsql_ctx 指针</param>
/// <returns>ERR_OK 请求成功</returns>
int32_t pgsql_try_connect(task_ctx *task, pgsql_ctx *pg);
/// <summary>
/// 更新 pgsql 连接的用户名和密码
/// </summary>
/// <param name="pg">pgsql_ctx 指针</param>
/// <param name="user">新用户名</param>
/// <param name="password">新密码</param>
void pgsql_set_userpwd(pgsql_ctx *pg, const char *user, const char *password);
/// <summary>
/// 更新目标数据库名
/// </summary>
/// <param name="pg">pgsql_ctx 指针</param>
/// <param name="database">新数据库名</param>
void pgsql_set_db(pgsql_ctx *pg, const char *database);
/// <summary>
/// 获取当前配置的数据库名
/// </summary>
/// <param name="pg">pgsql_ctx 指针</param>
/// <returns>数据库名字符串</returns>
const char *pgsql_get_db(pgsql_ctx *pg);
/// <summary>
/// 从命令完成标签中解析受影响的行数
/// </summary>
/// <param name="pgpack">pgpack_ctx 指针，complete 字段须已填充</param>
/// <returns>受影响的行数，解析失败时返回 0</returns>
int32_t pgsql_affected_rows(pgpack_ctx *pgpack);

#endif//PGSQL_H_
