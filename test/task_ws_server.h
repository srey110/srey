#ifndef TASK_WS_SERVER_H_
#define TASK_WS_SERVER_H_

#include "task_pub.h"

// 启动 WebSocket 服务任务，框架自动处理握手；握手完成后：
//   非分片帧 - 回显 text/binary，ping 回 pong，close 关闭连接
//   分片帧   - 收齐完整消息（PROT_SLICE_END）后回复三帧分片消息
// pt 非 0 时输出连接/关闭事件日志
void task_ws_server_start(loader_ctx *loader, const char *name, uint16_t port, int32_t pt);

#endif//TASK_WS_SERVER_H_
