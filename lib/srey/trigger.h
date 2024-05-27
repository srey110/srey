#ifndef TRIGGER_H_
#define TRIGGER_H_

#include "srey/spub.h"

typedef enum trigger_netev {
    NETEV_NONE = 0x00,
    NETEV_ACCEPT = 0x01,
    NETEV_AUTHSSL = 0x02,
    NETEV_SEND = 0x04
}trigger_netev;

void trigger_timeout(task_ctx *task, uint64_t sess, uint32_t ms, _timeout_cb _timeout);
void trigger_request(task_ctx *dst, task_ctx *src, uint8_t reqtype, uint64_t sess, void *data, size_t size, int32_t copy);
void trigger_response(task_ctx *dst, uint64_t sess, int32_t erro, void *data, size_t size, int32_t copy);
void trigger_call(task_ctx *dst, uint8_t reqtype, void *data, size_t size, int32_t copy);
int32_t trigger_listen(task_ctx *task, pack_type pktype, struct evssl_ctx *evssl,
    const char *ip, uint16_t port, uint64_t *id, int32_t netev);
SOCKET trigger_connect(task_ctx *task, pack_type pktype, const char *ip, uint16_t port, uint64_t *skid, int32_t netev);
SOCKET trigger_udp(task_ctx *task, const char *ip, uint16_t port, uint64_t *skid);

#endif//TRIGGER_H_
