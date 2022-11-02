#include "thread.h"

#define THREAD_WAITRUN  0
#define THREAD_RUNING   1
#define THREAD_STOP     2

#if defined(OS_WIN)
static uint32_t __stdcall _funccb(void *parg)
#else
static void *_funccb(void *parg)
#endif
{
    struct thread_ctx *pctx = (struct thread_ctx *)parg;
    pctx->state = THREAD_RUNING;
    pctx->th_cb(pctx->data);

    pctx->state = THREAD_STOP;
#if defined(OS_WIN)
    return ERR_OK;
#else
    return NULL;
#endif
}
void thread_init(struct thread_ctx *pctx)
{
    pctx->state = THREAD_STOP;
}
void thread_creat(struct thread_ctx *pctx, void(*cb)(void*), void *pdata)
{
    if (THREAD_STOP != pctx->state)
    {
        PRINT("%s", "thread not stop.");
        return;
    }
    pctx->state = THREAD_WAITRUN;
    pctx->th_cb = cb;
    pctx->data = pdata;
#if defined(OS_WIN)
    pctx->pthread = (HANDLE)_beginthreadex(NULL, 0, _funccb, (void*)pctx, 0, NULL);
    ASSERTAB(NULL != pctx->pthread, ERRORSTR(ERRNO));
#else
    ASSERTAB((ERR_OK == pthread_create(&pctx->pthread, NULL, _funccb, (void*)pctx)),
        ERRORSTR(ERRNO));
#endif
}
void thread_wait(struct thread_ctx *pctx)
{
    while (THREAD_WAITRUN == pctx->state);
}
void thread_join(struct thread_ctx *pctx)
{
    if (THREAD_STOP == pctx->state)
    {
        return;
    }
#if defined(OS_WIN)
    ASSERTAB(WAIT_OBJECT_0 == WaitForSingleObject(pctx->pthread, INFINITE), ERRORSTR(ERRNO));
#else
    ASSERTAB(ERR_OK == pthread_join(pctx->pthread, NULL), ERRORSTR(ERRNO));
#endif
}
