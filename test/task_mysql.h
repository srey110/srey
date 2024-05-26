#ifndef TASK_MYSQL_H_
#define TASK_MYSQL_H_

#include "lib.h"

#if WITH_CORO

void task_mysql_start(scheduler_ctx *scheduler, name_t name, int32_t pt);

#endif
#endif//TASK_MYSQL_H_