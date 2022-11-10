#include "thread.h"

typedef struct th_ctx
{
    void *udata;
    void(*th_cb)(void*);
}th_ctx;

#if defined(OS_WIN)
static uint32_t __stdcall _funccb(void *arg)
#else
static void *_funccb(void *arg)
#endif
{
    th_ctx *th = (th_ctx *)arg;
    th->th_cb(th->udata);
    FREE(th);
#if defined(OS_WIN)
    return ERR_OK;
#else
    return NULL;
#endif
}
pthread_t thread_creat(void(*cb)(void*), void *udata)
{
    th_ctx *th;
    MALLOC(th, sizeof(th_ctx));
    th->th_cb = cb;
    th->udata = udata;
    pthread_t pthread;
#if defined(OS_WIN)
    pthread = (HANDLE)_beginthreadex(NULL, 0, _funccb, (void*)th, 0, NULL);
    ASSERTAB(NULL != pthread, ERRORSTR(ERRNO));
#else
    ASSERTAB((ERR_OK == pthread_create(&pthread, NULL, _funccb, (void*)th)),
        ERRORSTR(ERRNO));
#endif
    return pthread;
}
void thread_join(pthread_t th)
{
#if defined(OS_WIN)
    ASSERTAB(WAIT_OBJECT_0 == WaitForSingleObject(th, INFINITE), ERRORSTR(ERRNO));
#else
    ASSERTAB(ERR_OK == pthread_join(th, NULL), ERRORSTR(ERRNO));
#endif
}
