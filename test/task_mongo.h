#ifndef TASK_MONGO_H_
#define TASK_MONGO_H_

#include "task_pub.h"

// 启动 MongoDB 连通性测试任务，覆盖：
// connect+auth → hello/ping → CRUD → 游标 getMore/killCursors → E11000 → 断连重连 → MORETOCOME →
// 事务(组包失败复原 / 第二个 session / 绑定分叉即拒 / 空事务) → 错口令建链失败 →
// 事务(begin+commit / 重连后旧 session 可用 / 事务作废 / commit 挂起期间重新 begin /
//      组包后排队期间重连的事务写不发 / 提交前已断改发 abort / 提交在途断连后 ping 重连、重发提交成功) →
// quit 后 ping 重建并装回执行器
// *ok 取值见 task_pub.h 的 name_val_ctx::val；发起连接前置 -2，连不上回 0，连上那一刻即置 -1。
void task_mongo_start(loader_ctx *loader, const char *name,
                      const char *host, uint16_t port,
                      const char *user, const char *password,
                      const char *db, const char *authdb,
                      int32_t *ok);

#endif//TASK_MONGO_H_
