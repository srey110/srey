#include "task_auto_close.h"

static int32_t _prt = 0;
// 累计关闭次数，程序退出后由 main.c 读取验证 auto_close 路径是否被覆盖。
// _closing 跑在 worker 线程，多轮 close 可落在不同 worker 上，故计数用 atomic
static atomic_t _autoclose;
// 累计"一轮调度结束"回调次数，验证 task_round_ended 挂上的回调确实被 loader 调到
static atomic_t _roundend;

uint32_t get_close_count(void) {
    return (uint32_t)ATOMIC_GET(&_autoclose);
}
uint32_t get_round_end_count(void) {
    return (uint32_t)ATOMIC_GET(&_roundend);
}
// 一轮调度结束回调：计数加一
static void _round_end(task_ctx *task) {
    (void)task;
    ATOMIC_ADD(&_roundend, 1);
}
// closing 回调：任务关闭时计数加一
static void _closing(task_ctx *task) {
    if (_prt) {
        LOG_INFO("task auto close %"PRIu64" run closing", task->handle);
    }
    ATOMIC_ADD(&_autoclose, 1);
}
void task_auto_close_start(loader_ctx *loader, const char *name, int32_t pt) {
    _prt = pt;
    task_ctx *task = task_new(loader, name, 0, NULL, NULL, NULL);
    task_round_ended(task, _round_end);
    if (ERR_OK != task_register(task, NULL, _closing)) {
        task_free(task);
    }
}
