#ifndef MYSQL_H_
#define MYSQL_H_

#include "protocol/mysql/mysql_struct.h"
#include "srey/spub.h"

// 内部函数：初始化握手回调函数指针
void _mysql_init(void *hspush);
// 内部函数：释放 mpack_ctx 数据包
void _mysql_pkfree(void *pack);
// 内部函数：释放用户数据上下文中的 MySQL 资源
void _mysql_udfree(ud_cxt *ud);
// 内部函数：连接关闭时清理 MySQL 资源
void _mysql_closed(ud_cxt *ud);
// 内部函数：SSL 握手完成后发送认证响应
int32_t _mysql_ssl_exchanged(ev_ctx *ev, ud_cxt *ud);
// 内部函数：MySQL 数据解包（验证阶段和命令阶段统一入口）
void *mysql_unpack(ev_ctx *ev, buffer_ctx *buf, ud_cxt *ud, int32_t *status);

/// <summary>
/// MySQL 参数初始化
/// </summary>
/// <param name="mysql">mysql_ctx</param>
/// <param name="ip">服务器 IP 地址</param>
/// <param name="port">端口号，0 表示使用默认端口 3306</param>
/// <param name="evssl">SSL 上下文，非 NULL 时启用 SSL 连接</param>
/// <param name="user">登录用户名</param>
/// <param name="password">登录密码</param>
/// <param name="database">默认数据库名，NULL 或空字符串表示不设置</param>
/// <param name="charset">字符集名称（如 "utf8mb4"）</param>
/// <param name="maxpk">最大数据包大小，0 表示使用默认值 ONEK * ONEK</param>
/// <returns>ERR_OK 成功，ERR_FAILED 失败</returns>
int32_t mysql_init(mysql_ctx *mysql, const char *ip, uint16_t port, struct evssl_ctx *evssl,
    const char *user, const char *password, const char *database, const char *charset, uint32_t maxpk);

/// <summary>
/// 发起数据库连接请求
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="mysql">mysql_ctx</param>
/// <returns>ERR_OK 请求发起成功</returns>
int32_t mysql_try_connect(task_ctx *task, mysql_ctx *mysql);

/// <summary>
/// 获取服务器版本信息
/// </summary>
/// <param name="mysql">mysql_ctx</param>
/// <returns>服务器版本字符串</returns>
const char *mysql_version(mysql_ctx *mysql);

/// <summary>
/// 获取最近一次操作的错误信息
/// </summary>
/// <param name="mysql">mysql_ctx</param>
/// <param name="code">输出错误码，NULL 表示不获取</param>
/// <returns>错误信息字符串</returns>
const char *mysql_erro(mysql_ctx *mysql, int32_t *code);

/// <summary>
/// 清除错误信息
/// </summary>
/// <param name="mysql">mysql_ctx</param>
void mysql_erro_clear(mysql_ctx *mysql);

/// <summary>
/// 获取最近一次 INSERT 操作的自增 ID
/// </summary>
/// <param name="mysql">mysql_ctx</param>
/// <returns>自增 ID 值</returns>
int64_t mysql_last_id(mysql_ctx *mysql);

/// <summary>
/// 获取最近一次操作影响的行数
/// </summary>
/// <param name="mysql">mysql_ctx</param>
/// <returns>影响的行数</returns>
int64_t mysql_affected_rows(mysql_ctx *mysql);

/// <summary>
/// 关闭预处理语句并释放相关资源
/// </summary>
/// <param name="stmt">mysql_stmt_ctx</param>
void mysql_stmt_close(mysql_stmt_ctx *stmt);

#endif//MYSQL_H_
