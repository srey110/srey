#ifndef TASK_ROUTER_H_
#define TASK_ROUTER_H_

#include "task_pub.h"

// 启动 lib/utils/router 路由 server task
// 路由表 / 中间件覆盖: 字面量、{param}、{?}、*、query、嵌套 group、命名中间件、
// 中间件截断、next 后置统计、漏写响应兜底 500、方法位掩码
void task_router_server_start(loader_ctx *loader, const char *name, uint16_t port);

// 启动客户端 task: 顺序发请求, 全部断言通过后将 result_slot 写为 1
void task_router_client_start(loader_ctx *loader, const char *name, uint16_t port, int32_t *result_slot);

#endif // TASK_ROUTER_H_
