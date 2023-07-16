#ifndef SYNSL_H_
#define SYNSL_H_

#include "service/spub.h"

#if WITH_CORO

int32_t syn_task_new(task_ctx *task, task_type ttype, name_t name, uint16_t maxcnt, uint16_t maxmsgqulens,
    task_new _init, task_run _run, task_free _tfree, void(*_arg_free)(void *arg), void *arg);
void syn_sleep(task_ctx *task, uint32_t ms);
void syn_timeout(task_ctx *task, uint64_t sess, uint32_t ms);
void *syn_request(task_ctx *dst, task_ctx *src, void *data, size_t size, int32_t copy, int32_t *erro, size_t *lens);
void *syn_slice(task_ctx *task, SOCKET fd, uint64_t skid, uint64_t sess, size_t *size, int32_t *end);
SOCKET syn_connect(task_ctx *task, pack_type pktype, struct evssl_ctx *evssl,
    const char *ip, uint16_t port, int32_t sendev, uint64_t *skid);
void *syn_synsend(task_ctx *task, SOCKET fd, uint64_t skid, uint64_t sess,
    void *data, size_t len, size_t *size, int32_t copy);
void *syn_synsendto(task_ctx *task, SOCKET fd, uint64_t skid,
    const char *ip, const uint16_t port, void *data, size_t len, size_t *size);
//ÐèÒªFREE
struct dns_ip *syn_dns_lookup(task_ctx *task, const char *dns, const char *domain, int32_t ipv6, size_t *cnt);
SOCKET syn_websock_connect(task_ctx *task, const char *host, uint16_t port, struct evssl_ctx *evssl, uint64_t *skid);

#endif
#endif//SYNSL_H_
