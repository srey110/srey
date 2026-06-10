#ifndef TASK_TCP_SERVER_H_
#define TASK_TCP_SERVER_H_

#include "task_pub.h"

// 启动 TCP 回显服务端任务，初始协议 PACK_CUSTZ_FIXED，支持四种测试指令：
//   TEST_ECHO          —— 原样回显收到的数据包
//   TEST_SSL_CHANGE    —— 先回显再将连接升级为 SSL
//   TEST_PKTYPE_CHANGE —— 先回显再按数据体第 2 字节切换 pack_type
//   TEST_RPC_ECHO      —— 将数据转发给 rpcname 任务（type 2 回显），收到结果后返回给客户端
// 运行中可切换为 PACK_CUSTZ_FLAG / PACK_CUSTZ_VAR
// pt 非 0 时输出连接/收发/关闭事件日志
void task_tcp_erver_start(loader_ctx *loader, const char *name, uint16_t port,
    void *evssl, const char *rpcname, int32_t pt);

#endif//TASK_TCP_SERVER_H_
