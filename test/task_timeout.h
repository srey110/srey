#ifndef TASK_TIMEOUT_H_
#define TASK_TIMEOUT_H_

#include "task_pub.h"

// 启动周期性综合测试任务，首次触发延迟 100ms，此后每轮间隔 1000ms。
// 每轮依次执行六个子测试，任意失败则将 *ok 置 0，全部通过置 1：
//   1. coro_sleep 精度：50 / 100 / 200 ms 三个时长的唤醒误差校验
//   2. auto_close：注册或关闭 TASK_NAME_AUTOCLOSE 任务，验证任务生命周期
//   3. RPC：task_call fire-and-forget + coro_request 整数加法(type1) + 字节串回显(type2)
//   4. UDP：1 字节、4095 字节固定边界 + 随机大小三次收发校验
//   5. TCP：PACK_CUSTZ_FIXED / FLAG / VAR 各 ECHO_ROUNDS 轮，含 SSL 升级和协议切换
//   6. HTTP：GET 请求验证 200 响应；chunked POST 请求收发验证（三帧往返）
//   7. WS：纯 WebSocket 服务端文本帧、二进制帧回显，ping/pong 往返，三帧分片消息收发验证
// rpcname 为 INVALID_TNAME 时跳过 RPC 子测试；autoclose 非 0 时本任务负责驱动 auto_close 子测试
void task_timeout_start(loader_ctx *loader, const char *name,
    const char *rpcname, name_val_ctx *ports, void *evssl,
    int32_t autoclose, const char *haborkey, int32_t pt, int32_t *ok);

#endif//TASK_TIMEOUT_H_
