#include "task_coro_timeout.h"

#if WITH_CORO

static int32_t _prt = 0;
static void _timeout(task_ctx *task, uint64_t sess) {
    if (_prt) {
        LOG_INFO("task %d run _timeout", task->name);
    }
    uint64_t bgts = nowsec();
    coro_sleep(task, 1000);
    if (_prt) {
        LOG_INFO("sleep(1000) start: %"PRIu64" end:%"PRIu64, bgts, nowsec());
    }
    task_timeout(task, 0, 3000, _timeout);
}
static void _startup(task_ctx *task) {
    task_timeout(task, 0, 3000, _timeout);
}
void task_coro_timeout_start(loader_ctx *loader, name_t name, int32_t pt) {
    _prt = pt;
    task_ctx *task = task_new(loader, name, NULL, NULL, NULL);
    task_register(task, _startup, NULL);
}

#endif
