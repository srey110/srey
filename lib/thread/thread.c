#include "thread/thread.h"

// 线程启动参数封装，传递用户回调和数据给线程入口函数
typedef struct th_ctx {
    void *udata; // 用户自定义参数
    th_cb thcb; // 用户线程回调函数
    thread_hooks hooks;
}th_ctx;

// 线程入口函数（内部使用）：调用用户回调后释放 th_ctx
#if defined(OS_WIN)
static uint32_t __stdcall _thread_cb(void *arg) {
#else
static void *_thread_cb(void *arg) {
#endif
    th_ctx *th = (th_ctx *)arg;
    if (NULL != th->hooks.init) {
        th->hooks.init(th->udata, th->hooks.assist);
    }
    if (NULL != th->thcb) {
        th->thcb(th->udata);
    }
    if (NULL != th->hooks.exit) {
        th->hooks.exit(th->udata, th->hooks.assist);
    }
    FREE(th);
#if defined(OS_WIN)
    return ERR_OK;
#else
    return NULL;
#endif
}
pthread_t thread_creat_hooks(th_cb _cb, hook_cb _init, hook_cb _exit,
                             void *udata, void *assist) {
    th_ctx *th;
    MALLOC(th, sizeof(th_ctx));
    th->udata = udata;
    th->thcb = _cb;
    th->hooks.init = _init;
    th->hooks.exit = _exit;
    th->hooks.assist = assist;
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
pthread_t thread_creat(th_cb _cb, void *udata) {
    return thread_creat_hooks(_cb, NULL, NULL, udata, NULL);
}
void thread_join(pthread_t th) {
#if defined(OS_WIN)
    ASSERTAB(WAIT_OBJECT_0 == WaitForSingleObject(th, INFINITE), ERRORSTR(ERRNO));
    CloseHandle(th);
#else
    ASSERTAB(ERR_OK == pthread_join(th, NULL), ERRORSTR(ERRNO));
#endif
}
