#ifndef PROTS_WRAP_H_
#define PROTS_WRAP_H_

#include "srey/task.h"
#include "protocol/mysql/mysql.h"
#include "protocol/pgsql/pgsql.h"
#include "protocol/mongo/mongo.h"
#include "protocol/smtp/smtp.h"
#include "protocol/mqtt/mqtt.h"

/// <summary>
/// 发起 MySQL 连接：绑定 task 并经 task_connect 注册网络事件回调
/// </summary>
/// <param name="task">所属 task_ctx</param>
/// <param name="mysql">mysql_ctx</param>
/// <returns>ERR_OK 成功，其他失败</returns>
int32_t mysql_try_connect(task_ctx *task, mysql_ctx *mysql);
/// <summary>
/// 发起 PostgreSQL 连接：绑定 task 并经 task_connect 注册网络事件回调
/// </summary>
/// <param name="task">所属 task_ctx</param>
/// <param name="pg">pgsql_ctx</param>
/// <returns>ERR_OK 成功，其他失败</returns>
int32_t pgsql_try_connect(task_ctx *task, pgsql_ctx *pg);
/// <summary>
/// 发起 MongoDB 连接：绑定 task 并经 task_connect 注册网络事件回调
/// </summary>
/// <param name="task">所属 task_ctx</param>
/// <param name="mongo">mongo_ctx</param>
/// <returns>ERR_OK 成功，其他失败</returns>
int32_t mongo_try_connect(task_ctx *task, mongo_ctx *mongo);
/// <summary>
/// 发起 SMTP 连接：绑定 task 并经 task_connect 注册网络事件回调
/// </summary>
/// <param name="task">所属 task_ctx</param>
/// <param name="smtp">smtp_ctx</param>
/// <returns>ERR_OK 成功，其他失败</returns>
int32_t smtp_try_connect(task_ctx *task, smtp_ctx *smtp);
/// <summary>
/// 发起 MQTT 连接：内部创建 mqtt_ctx，绑定 task 并经 task_connect 注册网络事件回调
/// </summary>
/// <param name="task">所属 task_ctx</param>
/// <param name="evssl">TLS 上下文，NULL 表示不加密</param>
/// <param name="ip">服务器 IP</param>
/// <param name="port">服务器端口</param>
/// <param name="netev">网络事件标志</param>
/// <param name="version">MQTT 协议版本</param>
/// <param name="fd">输出：socket 句柄</param>
/// <param name="skid">输出：socket ID</param>
/// <returns>ERR_OK 成功，其他失败</returns>
int32_t mqtt_try_connect(task_ctx *task, struct evssl_ctx *evssl,
                         const char *ip, uint16_t port, int32_t netev,
                         mqtt_protversion version, SOCKET *fd, uint64_t *skid);

#endif//PROTS_WRAP_H_
