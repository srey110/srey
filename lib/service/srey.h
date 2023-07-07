#ifndef SREY_H_
#define SREY_H_

#include "event/event.h"
#include "proto/protos.h"

typedef enum msg_type {
    MSG_TYPE_NONE = 0x00,
    MSG_TYPE_STARTED,
    MSG_TYPE_CLOSING,
    MSG_TYPE_TIMEOUT,
    MSG_TYPE_ACCEPT,
    MSG_TYPE_CONNECT,
    MSG_TYPE_HANDSHAKED,
    MSG_TYPE_RECV,
    MSG_TYPE_SEND,
    MSG_TYPE_CLOSE,
    MSG_TYPE_RECVFROM,
    MSG_TYPE_REQUEST,
    MSG_TYPE_RESPONSE
}msg_type;
typedef struct message_ctx {
    uint8_t msgtype;//msg_type
    uint8_t pktype;//unpack_type
    uint8_t slice;//slice_type
    int8_t erro;
    uint32_t src;
    SOCKET fd;
    void *data;
    size_t size;
    uint64_t skid;
    uint64_t sess;
}message_ctx;
typedef struct udp_msg_ctx {
    netaddr_ctx addr;
    char data[0];
}udp_msg_ctx;

typedef struct srey_ctx srey_ctx;
typedef struct task_ctx task_ctx;
typedef void *(*task_new)(task_ctx *task, void *arg);
typedef void(*task_run)(task_ctx *task, message_ctx *msg);
typedef void(*task_free)(task_ctx *task);

srey_ctx *srey_init(uint16_t nnet, uint16_t nworker, uint16_t adjinterval, uint16_t adjthreshold);
void srey_startup(srey_ctx *ctx);
void srey_free(srey_ctx *ctx);

task_ctx *srey_tasknew(srey_ctx *ctx, uint32_t name, uint16_t maxcnt, uint16_t maxmsgqulens,
    task_new _init, task_run _run, task_free _tfree, void *arg);
task_ctx *srey_taskqury(srey_ctx *ctx, uint32_t name);
ev_ctx *srey_netev(srey_ctx *ctx);
srey_ctx *task_srey(task_ctx *task);
ev_ctx *task_netev(task_ctx *task);
void *task_handle(task_ctx *task);
uint32_t task_name(task_ctx *task);
#if WITH_SSL
int32_t certs_register(srey_ctx *ctx, uint32_t name, struct evssl_ctx *evssl);
struct evssl_ctx *certs_qury(srey_ctx *ctx, uint32_t name);
#endif
void task_sleep(task_ctx *task, uint32_t ms);
void task_timeout(task_ctx *task, uint64_t sess, uint32_t ms);
void task_call(task_ctx *dst, void *data, size_t size, int32_t copy);
void *task_request(task_ctx *dst, task_ctx *src, void *data, size_t size, int32_t copy, size_t *lens);
void task_response(task_ctx *dst, uint64_t sess, void *data, size_t size, int32_t copy);
void *task_slice(task_ctx *task, uint64_t sess, size_t *size, int32_t *end);
int32_t task_netlisten(task_ctx *task, pack_type pktype, struct evssl_ctx *evssl,
    const char *ip, uint16_t port, int32_t sendev, uint64_t *id);
SOCKET task_netconnect(task_ctx *task, pack_type pktype, struct evssl_ctx *evssl,
    const char *ip, uint16_t port, int32_t sendev, uint64_t *skid);
SOCKET task_netudp(task_ctx *task, const char *ip, uint16_t port, uint64_t *skid);

void task_netsend(task_ctx *task, SOCKET fd, uint64_t skid,
    void *data, size_t len, pack_type pktype);
void *task_synsend(task_ctx *task, SOCKET fd, uint64_t skid,
    void *data, size_t len, size_t *size, pack_type pktype, uint64_t *sess);
void *task_synsendto(task_ctx *task, SOCKET fd, uint64_t skid,
    const char *ip, const uint16_t port, void *data, size_t len, size_t *size);

void _push_handshaked(SOCKET fd, uint64_t skid, ud_cxt *ud);

#endif //SREY_H_
