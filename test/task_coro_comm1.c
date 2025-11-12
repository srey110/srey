#include "task_coro_comm1.h"

static int32_t _prt = 0;
static void _timeout(task_ctx *task, uint64_t sess) {
    task_ctx *comm2 = task_grab(task->loader, 10005);
    const char *reqmsg = "this is task_coro_comm1 request";
    int32_t error;
    size_t size;
    void *data = coro_request(comm2, task, 0, (void *)reqmsg, strlen(reqmsg), 1, &error, &size);
    task_ungrab(comm2);
    if (ERR_OK != error) {
        LOG_ERROR("%s", "syn_request error");
    } else {
        char buf[128] = { 0 };
        memcpy(buf, data, size);
        if (_prt) {
            LOG_INFO("syn_request return: %s", buf);
        }
    }
    task_timeout(task, 0, 3000, _timeout);
}
//超时后如果注册了 _response_cb 也会收到消息
static void _response(task_ctx *task, uint64_t sess, int32_t error, void *data, size_t size) {
    char buf[128] = { 0 };
    memcpy(buf, data, size);
    if (_prt) {
        LOG_INFO("_response: %s", buf);
    }
}
static void _startup(task_ctx *task) {
    on_responsed(task, _response);
    task_timeout(task, 0, 3000, _timeout);
}
void task_coro_comm1_start(loader_ctx *loader, name_t name, int32_t pt) {
    _prt = pt;
    task_ctx *task = task_new(loader, name, NULL, NULL, NULL);
    task_register(task, _startup, NULL);
}
