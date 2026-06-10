#ifndef TASK_REDIS_H_
#define TASK_REDIS_H_

#include "lib.h"

// 启动 Redis 连通性测试任务，覆盖：
// connect (可选 AUTH) → SET/GET/DEL → HSET/HGETALL → INCR → 关闭
// 全部成功后将 *ok 置 1；任何步骤失败立即 LOG_ERROR 并返回（不置位）。
// key 为空字符串或 NULL 表示无密码 (docker-compose 默认配置)。
void task_redis_start(loader_ctx *loader, const char *name,
                      const char *host, uint16_t port,
                      const char *key, int32_t *ok);

#endif//TASK_REDIS_H_
