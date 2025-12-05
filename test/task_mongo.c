#include "task_mongo.h"

static int32_t _prt = 1;

static void _startup(task_ctx *task) {

}
static void _closing_cb(task_ctx *task) {

}
void task_mongo_start(loader_ctx *loader, name_t name, int32_t pt) {
    _prt = pt;
    task_ctx *task = task_new(loader, name, NULL, NULL, NULL);
    task_register(task, _startup, _closing_cb);
}
