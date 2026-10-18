#ifndef TASK_PGSQL_H_
#define TASK_PGSQL_H_

#include "lib.h"

// 启动 PostgreSQL 连通性测试任务，按执行顺序覆盖：
// connect → ping → 会话参数(client_encoding / DateStyle) → 建表 → 插入 → reader 回读 → 零列查询 →
// prepare+execute(参数绑定，文本结果) → 二进制结果 → COPY IN → 回读计数 → COPY IN 出错路径 →
// COPY OUT → 语法错响应 → 多语句多结果集 → 4 协程并发查询 →
// 走 TLS 另连一次查一条（WITH_SSL 时；服务端没开 TLS 则跳过）→ quit
// *ok 取值见 task_pub.h 的 name_val_ctx::val；发起连接前置 -2，连不上回 0，连上那一刻即置 -1。
void task_pgsql_start(loader_ctx *loader, const char *name,
                      const char *host, uint16_t port,
                      const char *user, const char *password, const char *database,
                      int32_t *ok);

#endif//TASK_PGSQL_H_
