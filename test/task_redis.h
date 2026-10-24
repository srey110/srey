#ifndef TASK_REDIS_H_
#define TASK_REDIS_H_

#include "task_pub.h"

// 启动 Redis 连通性测试任务，覆盖：
// connect (可选 AUTH) → 预清 DEL → SET/GET/DEL → HSET/HGET → GET 撞 WRONGTYPE → INCR → DEL → 错口令 AUTH 建链失败 → 关闭
// *ok 取值见 task_pub.h 的 name_val_ctx::val；发起连接前置 -2，连不上回 0，连上那一刻即置 -1。
// key 为空字符串或 NULL 表示无密码 (docker-compose 默认配置)。
void task_redis_start(loader_ctx *loader, const char *name,
                      const char *host, uint16_t port,
                      const char *key, int32_t *ok);

#endif//TASK_REDIS_H_
