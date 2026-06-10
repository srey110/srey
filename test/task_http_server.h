#ifndef TASK_HTTP_SERVER_H_
#define TASK_HTTP_SERVER_H_

#include "task_pub.h"

// 启动 HTTP 服务任务，支持以下功能：
//   普通请求：GET 返回 200 "ok"，POST 回显请求体
//   chunked 请求：收齐完整请求后回复三帧 chunked 响应
// pt 非 0 时输出连接/关闭事件日志
void task_http_server_start(loader_ctx *loader, const char *name, uint16_t port, int32_t pt);

#endif//TASK_HTTP_SERVER_H_
