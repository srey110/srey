#ifndef CORO_H_
#define CORO_H_

#include "srey/spub.h"

#if WITH_CORO

//ms 小于0 一直等待
void _coro_wait(task_ctx *task, int32_t ms, uint64_t sess, uint64_t assoc, message_ctx *msg);
void coro_sleep(task_ctx *task, uint32_t ms);
//超时后如果注册了 _response_cb 也可能会收到消息
void *coro_request(task_ctx *dst, task_ctx *src, int32_t ms, 
    uint8_t rtype, void *data, size_t size, int32_t copy,
    int32_t *erro, size_t *lens);
void *coro_slice(task_ctx *task, int32_t ms,
    SOCKET fd, uint64_t skid, uint64_t sess,
    size_t *size, int32_t *end);
SOCKET coro_connect(task_ctx *task, int32_t ms, pack_type pktype, struct evssl_ctx *evssl,
    const char *ip, uint16_t port, uint64_t *skid, int32_t appendev);
void *coro_send(task_ctx *task, int32_t ms, SOCKET fd, uint64_t skid, uint64_t sess,
    void *data, size_t len, size_t *size, int32_t copy);
void *coro_sendto(task_ctx *task, int32_t ms, SOCKET fd, uint64_t skid,
    const char *ip, const uint16_t port, void *data, size_t len, size_t *size);

#endif
#endif//CORO_H_
