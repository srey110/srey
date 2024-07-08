#ifndef TASK_RORO_NET_H_
#define TASK_RORO_NET_H_

#include "lib.h"

#if WITH_CORO

void task_coro_net_start(loader_ctx *loader, name_t name, int32_t pt);

#endif
#endif//TASK_RORO_NET_H_
