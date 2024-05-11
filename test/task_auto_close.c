#include "task_auto_close.h"

static int32_t _prt = 0;
static void _closing(task_ctx *task) {
    if (_prt) {
        LOG_INFO("task auto close %d run closing", task->name);
    }
}
void task_auto_close_start(scheduler_ctx *scheduler, name_t name, int32_t pt) {
    _prt = pt;
    task_ctx *task = task_new(name, NULL, NULL, NULL);
    task_register(scheduler, task, NULL, _closing);
}
