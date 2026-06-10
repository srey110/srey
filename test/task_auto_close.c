#include "task_auto_close.h"

static int32_t _prt = 0;
// 累计关闭次数，程序退出后由 main.c 读取验证 auto_close 路径是否被覆盖
static uint32_t _autoclose = 0;

uint32_t get_close_count(void) {
    return _autoclose;
}
// closing 回调：任务关闭时计数加一
static void _closing(task_ctx *task) {
    if (_prt) {
        LOG_INFO("task auto close %"PRIu64" run closing", task->handle);
    }
    _autoclose++;
}
void task_auto_close_start(loader_ctx *loader, const char *name, int32_t pt) {
    _prt = pt;
    task_ctx *task = task_new(loader, name, 0, NULL, NULL, NULL);
    if (ERR_OK != task_register(task, NULL, _closing)) {
        task_free(task);
    }
}
