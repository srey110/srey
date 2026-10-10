#ifndef LOADER_H_
#define LOADER_H_

#include "srey/spub.h"

// 任务调度：把 task 的消息交给线程去跑。
// 线程：nnet 个 net 线程(事件循环，收发网络数据)、nworker 个 worker、一个时间轮线程(到期投超时消息)、
// 一个 monitor(每 5 秒查一次有没有 task 卡在一条消息上，worker 与 net 线程都查)。
//
// 一条消息的流程：
//   1. 入队：net 线程解出的包、到期的超时、别的 task 的请求与响应，推进目标 task 的消息队列。
//   2. 抢调度权：把 task 的 global 从 0 改成 1，抢到的那一个负责调度，其余只入队。
//   3. 交出去(_loader_task_schedule)：
//      没绑 net 线程的，挑一个 worker(优先正在空转的，其次睡着的，都忙就轮流)放进它的任务队列，睡着就叫醒；
//      绑了的(task_bind_net)，经 ev_defer_exec 交给那条 net 线程，等它本轮事件派发完再跑。
//   4. 跑一轮(_loader_task_run)：成批取消息逐条分发。worker 上取多少由它的 weight 和 task 优先级定，
//      net 线程上一次跑完当时积压的全部。worker 自己队列空了，会去积压最多的那个 worker 那里拿一个。
//   5. 交还(_loader_task_release)：global 改回 0 后再看一眼队列，还有消息就重新抢，回到第 3 步。
// 捷径：绑在当前 net 线程上、正闲着的 task，收包时当场分发这一条、不入队(_loader_task_try_run)。
// 同一个 task 任一时刻只在一个线程上跑，靠的就是 global。
//
// 退出(loader_free)：先给所有 task 发 CLOSING 并等它们全部注销，再停 worker 与 monitor，最后停 net 线程和时间轮。

/// <summary>
/// 任务调度初始化
/// </summary>
/// <param name="nnet">网络线程数, 0 cpu核心数</param>
/// <param name="nworker">工作线程数, 0 cpu核心数</param>
/// <param name="twcap">时间轮队列大小, 0 4096</param>
/// <returns>loader_ctx</returns>
loader_ctx *loader_init(uint16_t nnet, uint16_t nworker, uint32_t twcap);
/// <summary>
/// 任务调度释放
/// </summary>
/// <param name="loader">loader_ctx</param>
void loader_free(loader_ctx *loader);
#if WITH_LUA && ENABLE_LUA_BYTECACHE
/// <summary>
/// 取 loader 内的 Lua 字节码缓存读写锁（供 lbc_init 使用）。
/// 锁随 loader 创建与释放，调用方只借用；slot 仅 worker 线程注册，其余线程走内部 fallback 锁。
/// </summary>
/// <param name="loader">loader_ctx</param>
/// <returns>rwlock_distr_ctx</returns>
rwlock_distr_ctx *loader_lckcache(loader_ctx *loader);
#endif
/// <summary>
/// net 线程数
/// </summary>
/// <param name="loader">loader_ctx</param>
/// <returns>net 线程数，即 task_bind_net 的下标上界(不含)</returns>
uint32_t loader_nnet(loader_ctx *loader);
/// <summary>
/// task 枚举回调函数类型
/// </summary>
/// <param name="name">已注册 task 的字符串名（匿名 task 为 NULL）</param>
/// <param name="handle">已注册 task 的句柄</param>
/// <param name="arg">透传给回调的用户参数</param>
typedef void(*task_each_cb)(const char *name, name_t handle, void *arg);
/// <summary>
/// 遍历所有已注册 task，为每个 task 调用 cb。
/// 内部持 lckmaptasks 读锁；cb 内禁止做 task_register 等会拿写锁的操作，否则死锁。
/// </summary>
/// <param name="loader">loader_ctx</param>
/// <param name="cb">每个 task 触发一次的回调</param>
/// <param name="arg">透传给 cb 的用户参数</param>
void loader_task_each(loader_ctx *loader, task_each_cb cb, void *arg);

#endif//LOADER_H_
