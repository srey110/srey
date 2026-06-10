#ifndef HUG_H_
#define HUG_H_

#include "base/macro.h"
#ifdef OS_WIN
#include "thread/mutex.h"
#include "thread/cond.h"
#endif

typedef struct hug_ctx {
    atomic_t exitflag;
#ifdef OS_WIN
    mutex_ctx muexit;
    cond_ctx condexit;
#else
    // POSIX: signal handler 可在任意线程执行, 持锁 signal 违反 async-signal-safe;
    // 改用 self-pipe trick: handler 走 hug_wakeup 写 1 字节, 主线程 read 阻塞等待
    int32_t exit_pipe[2];
#endif
}hug_ctx;

/// <summary>
/// 初始化 hug 模块: 创建同步对象 (POSIX self-pipe / Windows mutex+cond)。
/// 注意 hug 不注册系统信号 handler, 业务需自行调 sighandle(cb, &ctx) 在回调内 hug_wakeup(ctx);
/// 业务回调内可附加日志等其他动作, hug 仅提供唤醒/等待原语
/// </summary>
/// <param name="ctx">业务持有的 hug_ctx 实例 (栈或堆分配)</param>
/// <returns>ERR_OK 成功; ERR_FAILED 失败 (POSIX pipe 创建失败等)</returns>
int32_t hug_init(hug_ctx *ctx);
/// <summary>
/// 阻塞主线程, 直到 hug_wakeup 被调用 (业务通常在 sighandle 回调内调)
/// </summary>
/// <param name="ctx">hug_init 成功的 hug_ctx 实例</param>
void hug_wait(hug_ctx *ctx);
/// <summary>
/// 唤醒 hug_wait。可从任意线程调用 (POSIX 实现走 write, 安全于 signal handler 上下文)
/// </summary>
/// <param name="ctx">hug_init 成功的 hug_ctx 实例</param>
void hug_wakeup(hug_ctx *ctx);
/// <summary>
/// 释放 hug 模块资源 (pipe / mutex / cond)。
/// 仅在 hug_init 返回 ERR_OK 后调用; init 失败时 ctx 内字段未初始化, 不应调本函数
/// </summary>
/// <param name="ctx">hug_init 成功的 hug_ctx 实例</param>
void hug_free(hug_ctx *ctx);

#endif//HUG_H_
