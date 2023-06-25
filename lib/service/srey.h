#ifndef SREY_H_
#define SREY_H_

#include "event/event.h"
#include "proto/protos.h"

typedef enum msg_type {
    MSG_TYPE_STARTED = 0x01,
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
    MSG_TYPE_RESPONSE,
    MSG_TYPE_ENDRTN
}msg_type;
typedef struct message_ctx {
    int8_t msgtype;//msg_type
    int8_t pktype;//unpack_type
    int8_t erro;
    uint8_t synflag;
    int32_t src;
    SOCKET fd;
    void *data;
    size_t size;
    uint64_t session;
    uint64_t skid;
}message_ctx;
typedef struct udp_msg_ctx {
    netaddr_ctx addr;
    char data[0];
}udp_msg_ctx;

#define CONNECT_TIMEOUT       3000
#define NETRD_TIMEOUT         3000
#define QUMSG_INITLENS        128
typedef struct srey_ctx srey_ctx;
typedef struct task_ctx task_ctx;
typedef void *(*task_new)(task_ctx *task, void *arg);
typedef void(*task_run)(task_ctx *task, message_ctx *msg);
typedef void(*task_free)(task_ctx *task);
extern srey_ctx *srey;

srey_ctx *srey_init(uint32_t nnet, uint32_t nworker);
void srey_startup(srey_ctx *ctx);
void srey_free(srey_ctx *ctx);

task_ctx *srey_tasknew(srey_ctx *ctx, int32_t name, uint32_t maxcnt, 
    task_new _init, task_run _run, task_free _tfree, void *arg);
task_ctx *srey_taskqury(srey_ctx *ctx, int32_t name);
ev_ctx *srey_netev(srey_ctx *ctx);

#if WITH_SSL
void certs_register(srey_ctx *ctx, const char *name, struct evssl_ctx *evssl);
struct evssl_ctx *certs_qury(srey_ctx *ctx, const char *name);
#endif

srey_ctx *task_srey(task_ctx *task);
ev_ctx *task_netev(task_ctx *task);
void *task_handle(task_ctx *task);
int32_t task_name(task_ctx *task);
uint64_t task_session(task_ctx *task);

void task_sleep(task_ctx *task, uint32_t timeout);
void task_timeout(task_ctx *task, uint64_t session, uint32_t timeout);
void task_call(task_ctx *dst, void *data, size_t size, int32_t copy);
void *task_request(task_ctx *dst, task_ctx *src, void *data, size_t size, int32_t copy, size_t *lens);
void task_response(task_ctx *dst, uint64_t sess, void *data, size_t size, int32_t copy);
int32_t task_netlisten(task_ctx *task, pack_type pktype, struct evssl_ctx *evssl,
    const char *ip, uint16_t port, int32_t sendev);
SOCKET task_netconnect(task_ctx *task, pack_type pktype, struct evssl_ctx *evssl,
    const char *ip, uint16_t port, int32_t sendev, uint64_t *skid);
SOCKET task_netudp(task_ctx *task, const char *ip, uint16_t port, uint64_t *skid);

int32_t task_netsend(task_ctx *task, SOCKET fd, uint64_t skid,
    void *data, size_t len, uint8_t synflag, pack_type pktype);
void *task_synsend(task_ctx *task, SOCKET fd, uint64_t skid,
    void *data, size_t len, size_t *size, pack_type pktype);
int32_t task_sendto(task_ctx *task, SOCKET fd, uint64_t skid,
    const char *ip, const uint16_t port, void *data, size_t len, uint8_t synflag);
void *task_synsendto(task_ctx *task, SOCKET fd, uint64_t skid,
    const char *ip, const uint16_t port, void *data, size_t len, size_t *size);

void _push_handshaked(SOCKET fd, ud_cxt *ud);

#endif //SREY_H_
