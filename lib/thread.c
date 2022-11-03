#include "thread.h"

#define THREAD_WAITRUN  0
#define THREAD_RUNING   1
#define THREAD_STOP     2

#if defined(OS_WIN)
static uint32_t __stdcall _funccb(void *arg)
#else
static void *_funccb(void *arg)
#endif
{
    thread_ctx *ctx = (thread_ctx *)arg;
    ctx->state = THREAD_RUNING;
    ctx->th_cb(ctx->udata);
    ctx->state = THREAD_STOP;
#if defined(OS_WIN)
    return ERR_OK;
#else
    return NULL;
#endif
}
void thread_init(thread_ctx *ctx)
{
    ctx->state = THREAD_STOP;
}
void thread_creat(thread_ctx *ctx, void(*cb)(void*), void *udata)
{
    if (THREAD_STOP != ctx->state)
    {
        PRINT("%s", "thread not stop.");
        return;
    }
    ctx->state = THREAD_WAITRUN;
    ctx->th_cb = cb;
    ctx->udata = udata;
#if defined(OS_WIN)
    ctx->pthread = (HANDLE)_beginthreadex(NULL, 0, _funccb, (void*)ctx, 0, NULL);
    ASSERTAB(NULL != ctx->pthread, ERRORSTR(ERRNO));
#else
    ASSERTAB((ERR_OK == pthread_create(&ctx->pthread, NULL, _funccb, (void*)ctx)),
        ERRORSTR(ERRNO));
#endif
}
void thread_wait(thread_ctx *ctx)
{
    while (THREAD_WAITRUN == ctx->state);
}
void thread_join(thread_ctx *ctx)
{
    if (THREAD_STOP == ctx->state)
    {
        return;
    }
#if defined(OS_WIN)
    ASSERTAB(WAIT_OBJECT_0 == WaitForSingleObject(ctx->pthread, INFINITE), ERRORSTR(ERRNO));
#else
    ASSERTAB(ERR_OK == pthread_join(ctx->pthread, NULL), ERRORSTR(ERRNO));
#endif
}
