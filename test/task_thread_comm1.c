#include "task_thread_comm1.h"

static int32_t _prt = 0;
static void _response(task_ctx *task, uint64_t sess, int32_t error, void *data, size_t size) {
    char buf[128] = { 0 };
    memcpy(buf, data, size);
    if (_prt) {
        LOG_INFO("%s", buf);
    }
}
static void _timeout(task_ctx *task, uint64_t sess) {
    task_ctx *comm2 = task_grab(task->scheduler, 10005);
    const char *reqmsg = "this is task_thread_comm1 request";
    trigger_request(comm2, task, 0, createid(), (void*)reqmsg, strlen(reqmsg), 1);
    const char *callmsg = "this is task_thread_comm1 call";
    trigger_call(comm2, 1, (void*)callmsg, strlen(callmsg), 1);
    task_ungrab(comm2);
    trigger_timeout(task, createid(), 3000, _timeout);
}
void task_threadcomm1_start(scheduler_ctx *scheduler, name_t name, int32_t pt) {
    _prt = pt;
    task_ctx *task = task_new(name, NULL, NULL, NULL);
    task_register(scheduler, task, NULL, NULL);
    register_response(task, _response);
    trigger_timeout(task, createid(), 3000, _timeout);
}
