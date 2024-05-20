#ifndef CORO_UTILS_H_
#define CORO_UTILS_H_

#include "srey/spub.h"

#if WITH_CORO

//dns_ip *ÐèÒªFREE
struct dns_ip *coro_dns_lookup(task_ctx *task, const char *domain, int32_t ipv6, size_t *cnt);
//ws://host:port
SOCKET coro_wbsock_connect(task_ctx *task, struct evssl_ctx *evssl, const char *ws, uint64_t *skid, int32_t appendev);
SOCKET coro_redis_connect(task_ctx *task, struct evssl_ctx *evssl, const char *ip, uint16_t port, const char *key, 
    uint64_t *skid, int32_t appendev);

#endif
#endif//CORO_UTILS_H_
