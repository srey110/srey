#ifndef THREAD_H_
#define THREAD_H_

#include "macro.h"

#if defined(OS_WIN)
typedef HANDLE pthread_t;
#endif
typedef struct thread_ctx
{
    volatile int32_t state;
    pthread_t pthread;
    void *udata;
    void(*th_cb)(void*);
}thread_ctx;

void thread_init(thread_ctx *ctx);
void thread_creat(thread_ctx *ctx, void(*cb)(void*), void *udata);
void thread_wait(thread_ctx *ctx);
void thread_join(thread_ctx *ctx);

#endif//THREAD_H_
