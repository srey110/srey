#ifndef TASK_RPC_H_
#define TASK_RPC_H_

#include "lib.h"

// 启动 RPC 服务端任务，支持两种请求类型：
//   type 1 —— 整数加法：读取两个 int32，返回和（均为网络字节序）
//   type 2 —— 字节串回显：原样返回请求数据，覆盖变长数据传输路径
// pt 非 0 时输出 task_call fire-and-forget 的计算结果日志
void task_rpc_start(loader_ctx *loader, const char *name, int32_t pt);

#endif//TASK_RPC_H_
