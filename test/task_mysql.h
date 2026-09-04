#ifndef TASK_MYSQL_H_
#define TASK_MYSQL_H_

#include "lib.h"

// 启动 MySQL 连通性测试任务，覆盖：
// connect → selectdb → ping → 清表 + 插 3 行 → reader 逐字段回读 → prepare+execute →
// 语法错响应 → 多结果集 → session-track 跟踪当前库 → 4 协程并发查询 → quit
// *ok 三态含义见 task_pub.h 的 name_val_ctx::val；连上那一刻即置 -1。
// 期望 docker-compose 已启动并执行 docker/mysql-init.sql 创建 test_bind 表。
void task_mysql_start(loader_ctx *loader, const char *name,
                      const char *host, uint16_t port,
                      const char *user, const char *password, const char *database,
                      int32_t *ok);

#endif//TASK_MYSQL_H_
