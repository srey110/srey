#ifndef TRIGGER_H_
#define TRIGGER_H_

#include "srey/spub.h"

enum trigger_appendev {
    APPEND_ACCEPT = 0x01,
    APPEND_CONNECT = 0x02,
    APPEND_CLOSE = 0x04,
    APPEND_SEND = 0x08
};

void trigger_timeout(task_ctx *task, uint64_t sess, uint32_t ms, _timeout_cb _timeout);
void trigger_request(task_ctx *dst, task_ctx *src, uint8_t reqtype, uint64_t sess, void *data, size_t size, int32_t copy);
void trigger_response(task_ctx *dst, uint64_t sess, int32_t erro, void *data, size_t size, int32_t copy);
void trigger_call(task_ctx *dst, uint8_t reqtype, void *data, size_t size, int32_t copy);
//先注册需要的事件register_net_ 再启动
int32_t trigger_listen(task_ctx *task, pack_type pktype, struct evssl_ctx *evssl,
    const char *ip, uint16_t port, uint64_t *id, int32_t appendev);
SOCKET trigger_connect(task_ctx *task, uint64_t sess, pack_type pktype, struct evssl_ctx *evssl,
    const char *ip, uint16_t port, uint64_t *skid, int32_t appendev);
SOCKET trigger_udp(task_ctx *task, const char *ip, uint16_t port, uint64_t *skid);

#endif//TRIGGER_H_
