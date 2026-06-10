#ifndef THREAD_H_
#define THREAD_H_

#include "base/macro.h"

#if defined(OS_WIN)
typedef HANDLE pthread_t;
#endif

typedef void(*th_cb)(void *udata);
typedef void(*hook_cb)(void *udata, void *assist);
typedef struct thread_hooks {
    hook_cb init;
    hook_cb exit;
    void *assist;
} thread_hooks;

/// <summary>
/// 创建线程并附加 init / exit 钩子
/// </summary>
/// <param name="cb">业务线程回调函数</param>
/// <param name="init">线程进入业务回调前调用,接收 (udata, assist),可为 NULL</param>
/// <param name="exit">业务回调返回后调用,接收 (udata, assist),可为 NULL</param>
/// <param name="udata">透传给 cb / init / exit 的业务参数</param>
/// <param name="assist">透传给 init / exit 的钩子辅助参数,cb 不接收</param>
/// <returns>pthread_t</returns>
pthread_t thread_creat_hooks(th_cb _cb, hook_cb _init, hook_cb _exit,
                             void *udata, void *assist);
/// <summary>
/// 创建线程
/// </summary>
/// <param name="cb">回调函数</param>
/// <param name="udata">用户参数</param>
/// <returns>pthread_t</returns>
pthread_t thread_creat(th_cb _cb, void *udata);
/// <summary>
/// 等待线程退出
/// </summary>
/// <param name="th">pthread_t</param>
void thread_join(pthread_t th);

#endif//THREAD_H_
