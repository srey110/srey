#include "task_thread_comm2.h"

static int32_t _prt = 0;
static void _request(task_ctx *task, uint8_t reqtype, uint64_t sess, name_t src, void *data, size_t size) {
    char buf[128] = { 0 };
    memcpy(buf, data, size);
    if (_prt) {
        LOG_INFO("%s", buf);
    }
    if (INVALID_TNAME != src) {
        task_ctx *comm1 = task_grab(task->loader, src);
        if (NULL != comm1) {
            const char *respmsg = "this is task_thread_comm2 response";
            trigger_response(comm1, sess, ERR_OK, (void*)respmsg, strlen(respmsg), 1);
            task_ungrab(comm1);
        }
    }
}
static void _startup(task_ctx *task) {
    on_requested(task, _request);
}
void task_threadcomm2_start(loader_ctx *loader, name_t name, int32_t pt) {
    _prt = pt;
    task_ctx *task = task_new(loader, name, NULL, NULL, NULL);
    task_register(task, _startup, NULL);
}
