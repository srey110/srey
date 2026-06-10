#ifndef LOADER_H_
#define LOADER_H_

#include "srey/spub.h"

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
rwlock_distr_ctx *loader_lckcache(loader_ctx *loader);
#endif
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
