#include "thread/thread.h"

// 线程启动参数封装，传递用户回调和数据给线程入口函数
typedef struct th_ctx {
    void  *udata;         // 用户自定义参数
    void (*th_cb)(void*); // 用户线程回调函数
}th_ctx;

// 线程入口函数（内部使用）：调用用户回调后释放 th_ctx
#if defined(OS_WIN)
static uint32_t __stdcall _thread_cb(void *arg) {
#else
static void *_thread_cb(void *arg) {
#endif
    th_ctx *th = (th_ctx *)arg;
    th->th_cb(th->udata);
    FREE(th);
#if defined(OS_WIN)
    return ERR_OK;
#else
    return NULL;
#endif
}
pthread_t thread_creat(void(*cb)(void*), void *udata) {
    th_ctx *th;
    MALLOC(th, sizeof(th_ctx));
    th->th_cb = cb;
    th->udata = udata;
    pthread_t pthread;
#if defined(OS_WIN)
    pthread = (HANDLE)_beginthreadex(NULL, 0, _thread_cb, (void*)th, 0, NULL);
    ASSERTAB(NULL != pthread, ERRORSTR(ERRNO));
#else
    ASSERTAB((ERR_OK == pthread_create(&pthread, NULL, _thread_cb, (void*)th)),
        ERRORSTR(ERRNO));
#endif
    return pthread;
}
void thread_join(pthread_t th) {
#if defined(OS_WIN)
    ASSERTAB(WAIT_OBJECT_0 == WaitForSingleObject(th, INFINITE), ERRORSTR(ERRNO));
    CloseHandle(th);
#else
    ASSERTAB(ERR_OK == pthread_join(th, NULL), ERRORSTR(ERRNO));
#endif
}
