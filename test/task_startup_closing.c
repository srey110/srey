#include "task_startup_closing.h"

static int32_t _prt = 0;
static void _startup(task_ctx *task) {
    if (_prt) {
        LOG_INFO("task %d run startup", task->name);
    }
}
static void _closing(task_ctx *task) {
    if (_prt) {
        LOG_INFO("task %d run closing", task->name);
    }
}
void task_startup_closing_start(loader_ctx *loader, name_t name, int32_t pt) {
    _prt = pt;
    task_ctx *task = task_new(loader, name, NULL, NULL, NULL);
    task_register(task, _startup, _closing);
}
