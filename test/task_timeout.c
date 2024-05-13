#include "task_timeout.h"
#include "task_auto_close.h"

static int32_t _prt = 1;
static name_t _autoclose = 10007;
static void _timeout1(task_ctx *task, uint64_t sess) {
    if (_prt) {
        LOG_INFO("task %d run _timeout1 session %"PRIu64, task->name, sess);
    }
    task_ctx *autoclose = task_grab(task->scheduler, _autoclose);
    if (NULL == autoclose) {
        task_auto_close_start(task->scheduler, _autoclose, _prt);
    } else {
        task_ungrab(autoclose);
    }
    trigger_timeout(task, createid(), 3000, _timeout1);
}
static void _timeout2(task_ctx *task, uint64_t sess) {
    if (_prt) {
        LOG_INFO("task %d run _timeout2 session %"PRIu64, task->name, sess);
    }
    task_ctx *autoclose = task_grab(task->scheduler, _autoclose);
    if (NULL != autoclose) {
        task_close(autoclose);
        task_ungrab(autoclose);
    }
    trigger_timeout(task, createid(), 5000, _timeout2);
}
static void _startup(task_ctx *task) {
    trigger_timeout(task, createid(), 3000, _timeout1);
    trigger_timeout(task, createid(), 5000, _timeout2);
}
void task_timeout_start(scheduler_ctx *scheduler, name_t name, int32_t pt) {
    _prt = pt;
    task_ctx *task = task_new(scheduler, name, NULL, NULL, NULL);
    task_register(task, _startup, NULL);
}
