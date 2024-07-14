#ifndef THREAD_H_
#define THREAD_H_

#include "base/macro.h"

#if defined(OS_WIN)
typedef HANDLE pthread_t;
#endif
/// <summary>
/// 创建线程
/// </summary>
/// <param name="cb">回调函数</param>
/// <param name="udata">用户参数</param>
/// <returns>pthread_t</returns>
pthread_t thread_creat(void(*cb)(void*), void *udata);
/// <summary>
/// 等待线程结束
/// </summary>
/// <param name="th">pthread_t</param>
void thread_join(pthread_t th);

#endif//THREAD_H_
