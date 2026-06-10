#ifndef TASK_MONGO_H_
#define TASK_MONGO_H_

#include "lib.h"

// 启动 MongoDB 连通性测试任务，覆盖：
// connect + SCRAM-SHA-256 auth → hello → ping → drop(srey_test 集合) → insert(3 文档) →
// find → count → update → startsession + begin + commit → 关闭
// 全部成功后将 *ok 置 1；任何步骤失败立即 LOG_ERROR 并返回（不置位）。
void task_mongo_start(loader_ctx *loader, const char *name,
                      const char *host, uint16_t port,
                      const char *user, const char *password,
                      const char *db, const char *authdb,
                      int32_t *ok);

#endif//TASK_MONGO_H_
