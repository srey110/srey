#ifndef TASK_REDIS_H_
#define TASK_REDIS_H_

#include "lib.h"

#if WITH_CORO

void task_redis_start(loader_ctx *loader, name_t name, int32_t pt);

#endif
#endif//TASK_REDIS_H_
