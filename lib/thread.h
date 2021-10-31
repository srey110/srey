#ifndef THREAD_H_
#define THREAD_H_

#include "macro.h"

#define THREAD_WAITRUN  0
#define THREAD_RUNING   1
#define THREAD_STOP     2

#if defined(OS_WIN)
typedef HANDLE pthread_t;
#endif
typedef struct thread_ctx
{
    volatile atomic_t threadid;
    volatile atomic_t state;
    pthread_t pthread;
    void *param1;
    void *param2;
    void *param3;
    void(*cb)(void*, void*, void*);
}thread_ctx;
/*
* \brief          获取当前线程Id
* \return         线程Id
*/
static inline uint32_t threadid()
{
#if defined(OS_WIN)
    return (uint32_t)GetCurrentThreadId();
#else
    return (uint32_t)pthread_self();
#endif
};
#if defined(OS_WIN)
static uint32_t __stdcall _funccb(void *parg)
#else
static void *_funccb(void *parg)
#endif
{
    struct thread_ctx *pctx = (struct thread_ctx *)parg;
    ATOMIC_SET(&pctx->threadid, (atomic_t)threadid());
    ATOMIC_SET(&pctx->state, THREAD_RUNING);

    pctx->cb(pctx->param1, pctx->param2, pctx->param3);

    ATOMIC_SET(&pctx->state, THREAD_STOP);
#if defined(OS_WIN)
    return ERR_OK;
#else
    return NULL;
#endif
}
/*
* \brief             初始化
*/
static inline void thread_init(struct thread_ctx *pctx)
{
    pctx->threadid = 0;
    pctx->state = THREAD_STOP;
};
/*
* \brief             创建一线程,别多次调用
* \param thread_cb   回调函数
* \param pparam      回调函数参数,方便多个参数的时候
*/
static inline void thread_creat(struct thread_ctx *pctx, void(*cb)(void*, void*, void*),
    void *pparam1, void *pparam2, void *pparam3)
{
    if (!ATOMIC_CAS(&pctx->state, THREAD_STOP, THREAD_WAITRUN))
    {
        PRINTF("%s", "thread not stop.");
        return;
    }

    pctx->cb = cb;
    pctx->param1 = pparam1;
    pctx->param2 = pparam2;
    pctx->param3 = pparam3;

#if defined(OS_WIN)
    pctx->pthread = (HANDLE)_beginthreadex(NULL, 0, _funccb, (void*)pctx, 0, NULL);
    ASSERTAB(NULL != pctx->pthread, ERRORSTR(ERRNO));
#else
    ASSERTAB((ERR_OK == pthread_create(&pctx->pthread, NULL, _funccb, (void*)pctx)),
        ERRORSTR(ERRNO));
#endif
};
/*
* \brief          等待线程启动
*/
static inline void thread_waitstart(struct thread_ctx *pctx)
{
    while (THREAD_WAITRUN == ATOMIC_GET(&pctx->state));
};
/*
* \brief          等待线程结束
*/
static inline void thread_join(struct thread_ctx *pctx)
{
    if (THREAD_STOP == ATOMIC_GET(&pctx->state))
    {
        return;
    }
#if defined(OS_WIN)
    ASSERTAB(WAIT_OBJECT_0 == WaitForSingleObject(pctx->pthread, INFINITE), ERRORSTR(ERRNO));
#else
    ASSERTAB(ERR_OK == pthread_join(pctx->pthread, NULL), ERRORSTR(ERRNO));
#endif
};
/*
* \brief          获取启动的线程id
*/
static inline uint32_t thread_id(struct thread_ctx *pctx)
{
    return (uint32_t)ATOMIC_GET(&pctx->threadid);
};

#endif//THREAD_H_
