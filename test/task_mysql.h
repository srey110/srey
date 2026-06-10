#ifndef TASK_MYSQL_H_
#define TASK_MYSQL_H_

#include "lib.h"

// 启动 MySQL 连通性测试任务，覆盖：
// connect → selectdb → ping → query (无 bind / with bind) → reader 遍历 → prepare+execute → quit
// 全部成功后将 *ok 置 1；任何步骤失败立即 LOG_ERROR 并返回（不置位）。
// 期望 docker-compose 已启动并执行 docker/mysql-init.sql 创建 test_bind 表。
void task_mysql_start(loader_ctx *loader, const char *name,
                      const char *host, uint16_t port,
                      const char *user, const char *password, const char *database,
                      int32_t *ok);

#endif//TASK_MYSQL_H_
