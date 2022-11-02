#ifndef THREAD_H_
#define THREAD_H_

#include "macro.h"

#if defined(OS_WIN)
typedef HANDLE pthread_t;
#endif
struct thread_ctx
{
    volatile int32_t state;
    pthread_t pthread;
    void *data;
    void(*th_cb)(void*);
};

/*
* \brief             初始化
*/
void thread_init(struct thread_ctx *pctx);
/*
* \brief             创建一线程,别多次调用
* \param thread_cb   回调函数
* \param pparam      回调函数参数,方便多个参数的时候
*/
void thread_creat(struct thread_ctx *pctx, void(*cb)(void*), void *pdata);
/*
* \brief          等待线程启动
*/
void thread_wait(struct thread_ctx *pctx);
/*
* \brief          等待线程结束
*/
void thread_join(struct thread_ctx *pctx);

#endif//THREAD_H_
