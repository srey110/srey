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
/// <param name="setsess">是否设置sess</param>
/// <returns>ERR_OK 成功，其他失败</returns>
int32_t mysql_try_connect(task_ctx *task, mysql_ctx *mysql, int32_t setsess);
/// <summary>
/// 发起 PostgreSQL 连接：绑定 task 并经 task_connect 注册网络事件回调
/// </summary>
/// <param name="task">所属 task_ctx</param>
/// <param name="pg">pgsql_ctx</param>
/// <param name="setsess">是否设置sess</param>
/// <returns>ERR_OK 成功，其他失败</returns>
int32_t pgsql_try_connect(task_ctx *task, pgsql_ctx *pg, int32_t setsess);
/// <summary>
/// 发起 MongoDB 连接：绑定 task 并经 task_connect 注册网络事件回调
/// </summary>
/// <param name="task">所属 task_ctx</param>
/// <param name="mongo">mongo_ctx</param>
/// <param name="setsess">是否设置sess</param>
/// <returns>ERR_OK 成功，其他失败</returns>
int32_t mongo_try_connect(task_ctx *task, mongo_ctx *mongo, int32_t setsess);
/// <summary>
/// 发起 SMTP 连接：绑定 task 并经 task_connect 注册网络事件回调
/// </summary>
/// <param name="task">所属 task_ctx</param>
/// <param name="smtp">smtp_ctx</param>
/// <param name="setsess">是否设置sess</param>
/// <returns>ERR_OK 成功，其他失败</returns>
int32_t smtp_try_connect(task_ctx *task, smtp_ctx *smtp, int32_t setsess);
/// <summary>
/// 发起 MQTT 连接：挂上按协议版本的全局 mqtt_ctx，绑定 task 并经 task_connect 注册网络事件回调
/// </summary>
/// <param name="task">所属 task_ctx</param>
/// <param name="evssl">TLS 上下文，NULL 表示不加密</param>
/// <param name="ip">服务器 IP</param>
/// <param name="port">服务器端口</param>
/// <param name="netev">网络事件标志</param>
/// <param name="version">MQTT 协议版本，须是 MQTT_311 或 MQTT_50，否则连接不发起</param>
/// <param name="setsess">是否设置sess</param>
/// <param name="sk">连接标识</param>
/// <returns>ERR_OK 成功，其他失败</returns>
int32_t mqtt_try_connect(task_ctx *task, struct evssl_ctx *evssl,
                         const char *ip, uint16_t port, int32_t netev,
                         mqtt_protversion version, int32_t setsess, sock_ctx *sk);
/// <summary>
/// 给一条已完成 WebSocket 握手、且协商到 "mqtt" 子协议的连接绑定 MQTT 上下文。
/// 客户端方向没有别的建立点：服务端方向由协议层解 CONNECT 时自建，而客户端收到的第一条是
/// CONNACK，那里只认已存在的上下文，缺了就判协议错并断连。
/// 须在发出 MQTT CONNECT 之前调用——它与随后的发送同走一条命令队列、同 fd 落同一个 watcher，
/// FIFO 保证先投的先生效
/// </summary>
/// <param name="task">所属 task_ctx</param>
/// <param name="sk">连接标识</param>
/// <param name="version">MQTT 协议版本，须是 MQTT_311 或 MQTT_50</param>
/// <returns>ERR_OK 命令已投递，只表示投递成功、不代表真的绑上了：非 WebSocket 连接、握手
/// 未完成、子协议无内建解析器、已绑过一次——这几种都判误用，协议层拒收并就地断连。
/// version 非法或 fd 非法返回 ERR_FAILED。
/// 无论哪种失败，上下文都已在内部回收，调用方不持有任何东西</returns>
int32_t mqtt_ws_bind(task_ctx *task, sock_ctx *sk, mqtt_protversion version);

#endif//PROTS_WRAP_H_
