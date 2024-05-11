#ifndef CORO_UTILS_H_
#define CORO_UTILS_H_

#include "srey/spub.h"

#if WITH_CORO

//dns_ip *ÐèÒªFREE
struct dns_ip *coro_dns_lookup(task_ctx *task, int32_t ms, const char *dns, const char *domain, int32_t ipv6, size_t *cnt);

#endif
#endif//CORO_UTILS_H_
