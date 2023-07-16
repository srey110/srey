#ifndef SREY_H_
#define SREY_H_

#include "service/spub.h"

srey_ctx *srey_init(uint16_t nnet, uint16_t nworker, size_t stack_size, uint16_t interval, uint16_t threshold);
void srey_free(srey_ctx *ctx);
uint16_t srey_nworker(srey_ctx *ctx);
void srey_worker_load(srey_ctx *ctx, uint16_t index, uint32_t *ntask, uint32_t *cpu_cost);

#if WITH_SSL
int32_t srey_ssl_register(srey_ctx *ctx, name_t name, struct evssl_ctx *evssl);
struct evssl_ctx *srey_ssl_qury(srey_ctx *ctx, name_t name);
#endif

int32_t srey_task_new(srey_ctx *ctx, task_type ttype, name_t name, uint16_t maxcnt, uint16_t maxmsgqulens,
    name_t src, uint64_t sess, task_new _init, task_run _run, task_free _tfree, void(*_arg_free)(void *arg), void *arg);
task_ctx *srey_task_grab(srey_ctx *ctx, name_t name);
void srey_task_addref(task_ctx *task);
void srey_task_release(task_ctx *task);
size_t srey_task_qusize(task_ctx *task);

void srey_timeout(task_ctx *task, uint64_t sess, uint32_t ms);
void srey_request(task_ctx *dst, task_ctx *src, uint64_t sess, void *data, size_t size, int32_t copy);
void srey_response(task_ctx *dst, uint64_t sess, int32_t erro, void *data, size_t size, int32_t copy);
void srey_call(task_ctx *dst, void *data, size_t size, int32_t copy);

int32_t srey_listen(task_ctx *task, pack_type pktype, struct evssl_ctx *evssl,
    const char *ip, uint16_t port, int32_t sendev, uint64_t *id);
SOCKET srey_connect(task_ctx *task, uint64_t sess, pack_type pktype, struct evssl_ctx *evssl,
    const char *ip, uint16_t port, int32_t sendev, uint64_t *skid);
SOCKET srey_udp(task_ctx *task, const char *ip, uint16_t port, uint64_t *skid);

void push_handshaked(SOCKET fd, uint64_t skid, ud_cxt *ud, int32_t *closefd, int32_t erro);
void message_clean(msg_type mtype, pack_type pktype, void *data);

#endif //SREY_H_
