#ifndef THREAD_H_
#define THREAD_H_

#include "macro.h"

#if defined(OS_WIN)
typedef HANDLE pthread_t;
#endif

pthread_t thread_creat(void(*cb)(void*), void *udata);
void thread_join(pthread_t th);

#endif//THREAD_H_
