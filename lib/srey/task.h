#ifndef TASK_H_
#define TASK_H_

#include "srey/spub.h"

task_ctx *task_new(name_t name, _task_dispatch_cb _dispatch, free_cb _argfree, void *arg);
void task_free(task_ctx *task);
int32_t task_register(scheduler_ctx *scheduler, task_ctx *task, _task_startup_cb _startup, _task_closing_cb _closing);
void task_close(task_ctx *task);//任务关闭
task_ctx *task_grab(scheduler_ctx *scheduler, name_t name);
void task_incref(task_ctx *task);
void task_ungrab(task_ctx *task);//与 task_grab task_incref 配对

#endif//TASK_H_
