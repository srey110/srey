#ifndef TASK_MONGO_H_
#define TASK_MONGO_H_

#include "lib.h"

// 启动 MongoDB 连通性测试任务，覆盖：
// connect+auth → hello/ping → CRUD → E11000 → 断连重连 → MORETOCOME →
// 事务(组包失败复原 / 第二个 session / 绑定分叉即拒 / begin+commit / 重连后旧 session 可用 /
//      组包后排队期间重连的事务写不发)
// *ok 取值见 task_pub.h 的 name_val_ctx::val；发起连接前置 -2，连不上回 0，连上那一刻即置 -1。
void task_mongo_start(loader_ctx *loader, const char *name,
                      const char *host, uint16_t port,
                      const char *user, const char *password,
                      const char *db, const char *authdb,
                      int32_t *ok);

#endif//TASK_MONGO_H_
