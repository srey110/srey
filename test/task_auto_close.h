#ifndef TASK_AUTO_CLOSE_H_
#define TASK_AUTO_CLOSE_H_

#include "lib.h"

// 启动仅用于计数关闭次数的辅助任务，closing 回调执行时将全局计数器加一
// 用于验证 _timeout_auto_close 路径：程序退出后通过 get_close_count() 确认路径被覆盖
void task_auto_close_start(loader_ctx *loader, const char *name, int32_t pt);

// 返回自进程启动以来 task_auto_close 任务被关闭的累计次数
uint32_t get_close_count(void);

#endif//TASK_AUTO_CLOSE_H_
