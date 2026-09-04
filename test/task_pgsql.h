#ifndef TASK_PGSQL_H_
#define TASK_PGSQL_H_

#include "lib.h"

// 启动 PostgreSQL 连通性测试任务，覆盖：
// connect → ping → 建表 + 插入 → reader 逐字段回读 → 语法错响应 → 多语句多结果集 →
// prepare+execute(参数绑定) → COPY IN → COPY OUT → 4 协程并发查询 → quit
// *ok 三态含义见 task_pub.h 的 name_val_ctx::val；连上那一刻即置 -1。
void task_pgsql_start(loader_ctx *loader, const char *name,
                      const char *host, uint16_t port,
                      const char *user, const char *password, const char *database,
                      int32_t *ok);

#endif//TASK_PGSQL_H_
