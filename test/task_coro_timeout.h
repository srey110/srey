#ifndef TASK_CORO_TIMEOUT_H_
#define TASK_CORO_TIMEOUT_H_

#include "lib.h"

#if WITH_CORO

void task_coro_timeout_start(loader_ctx *loader, name_t name, int32_t pt);

#endif
#endif//TASK_CORO_TIMEOUT_H_
