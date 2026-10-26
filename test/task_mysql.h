#ifndef TASK_MYSQL_H_
#define TASK_MYSQL_H_

#include "task_pub.h"

// 启动 MySQL 连通性测试任务，覆盖：
// 走 TLS 另连一次查一条(WITH_SSL 时，先于明文连接，给 caching_sha2 账号暖服务端缓存) →
// connect → selectdb → ping → 清表 + 插 3 行 → reader 逐字段回读 → prepare+execute →
// 语法错响应 → 多结果集 → 多语句逐条计数 → session-track 跟踪当前库 → 4 协程并发查询 →
// 2 条真实连接池跑 4 协程查询 → 强制断连后 ping 重连(断连前的 stmt 本地拒掉) → quit 后 ping 重建并装回执行器、再并发查询 →
// 错口令建链失败(走 TLS 时错误码 1045) → quit。全程不该收到 SSLEXCHANGED(TLS 由协议自己升级)
// *ok 取值见 task_pub.h 的 name_val_ctx::val；发起连接前置 -2，连不上回 0，连上那一刻即置 -1。
// 期望 docker-compose 已启动并执行 docker/mysql-init.sql 创建 test_bind 表。
void task_mysql_start(loader_ctx *loader, const char *name,
                      const char *host, uint16_t port,
                      const char *user, const char *password, const char *database,
                      int32_t *ok);

#endif//TASK_MYSQL_H_
