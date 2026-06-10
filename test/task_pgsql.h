#ifndef TASK_PGSQL_H_
#define TASK_PGSQL_H_

#include "lib.h"

// 启动 PostgreSQL 连通性测试任务，覆盖：
// connect → ping → query(DDL/DML) → prepare+execute(参数绑定) → COPY IN → COPY OUT → quit
// 全部成功后将 *ok 置 1；任何步骤失败立即 LOG_ERROR 并返回（不置位）。
void task_pgsql_start(loader_ctx *loader, const char *name,
                      const char *host, uint16_t port,
                      const char *user, const char *password, const char *database,
                      int32_t *ok);

#endif//TASK_PGSQL_H_
