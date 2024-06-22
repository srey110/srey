#ifndef CORO_H_
#define CORO_H_

#include "srey/spub.h"

#if WITH_CORO

void coro_sleep(task_ctx *task, uint32_t ms);
void *coro_request(task_ctx *dst, task_ctx *src, uint8_t rtype, void *data, size_t size, int32_t copy, int32_t *erro, size_t *lens);
int32_t coro_ssl_exchange(task_ctx *task, SOCKET fd, uint64_t skid, int32_t client, struct evssl_ctx *evssl);
int32_t coro_handshaked(task_ctx *task, SOCKET fd, uint64_t skid, message_ctx *msg);
SOCKET coro_connect(task_ctx *task, pack_type pktype, struct evssl_ctx *evssl, const char *ip, uint16_t port, uint64_t *skid, int32_t netev);
void *coro_send(task_ctx *task, SOCKET fd, uint64_t skid, void *data, size_t len, size_t *size, int32_t copy);
void *coro_slice(task_ctx *task, SOCKET fd, uint64_t skid, size_t *size, int32_t *end);
void *coro_sendto(task_ctx *task, SOCKET fd, uint64_t skid, const char *ip, const uint16_t port, void *data, size_t len, size_t *size);

#endif
#endif//CORO_H_
