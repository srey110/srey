#ifndef SREY_H_
#define SREY_H_

#include "event/event.h"
#include "proto/protos.h"

typedef enum msg_type {
    MSG_TYPE_CLOSING = 0x01,
    MSG_TYPE_TIMEOUT,
    MSG_TYPE_ACCEPT,
    MSG_TYPE_CONNECT,
    MSG_TYPE_RECV,
    MSG_TYPE_SEND,
    MSG_TYPE_CLOSE,
    MSG_TYPE_RECVFROM,
    MSG_TYPE_USER,
}msg_type;
typedef struct srey_ctx srey_ctx;
typedef struct task_ctx task_ctx;
typedef struct message_ctx {
    msg_type type;
    int32_t error;
    SOCKET fd;
    task_ctx *src;
    void *data;
    size_t size;
    uint64_t session;
    netaddr_ctx addr;
}message_ctx;

#define QUMSG_INITLENS        512
typedef void *(*task_new)(task_ctx *task, void *arg);
typedef void(*task_run)(task_ctx *task, message_ctx *msg);
typedef void(*task_free)(task_ctx *task);

srey_ctx *srey_init(uint32_t nnet, uint32_t nworker);
void srey_free(srey_ctx *ctx);

task_ctx *srey_tasknew(srey_ctx *ctx, int32_t name, uint32_t maxcnt, 
    task_new _init, task_run _run, task_free _tfree, void *arg);
task_ctx *srey_taskqury(srey_ctx *ctx, int32_t name);

#if WITH_SSL
void certs_register(srey_ctx *ctx, const char *name, struct evssl_ctx *evssl);
struct evssl_ctx *certs_qury(srey_ctx *ctx, const char *name);
#endif

srey_ctx *task_srey(task_ctx *task);
ev_ctx *task_netev(task_ctx *task);
void *task_handle(task_ctx *task);
int32_t task_name(task_ctx *task);
uint64_t task_session(task_ctx *task);

void task_user(task_ctx *dst, task_ctx *src, uint64_t session, void *data, size_t size, int32_t copy);
void task_timeout(task_ctx *task, uint64_t session, uint32_t timeout);
int32_t task_netlisten(task_ctx *task, unpack_type type, struct evssl_ctx *evssl,
    const char *host, uint16_t port, int32_t sendev);
SOCKET task_netconnect(task_ctx *task, unpack_type type, uint64_t session, struct evssl_ctx *evssl,
    const char *host, uint16_t port, int32_t sendev);
SOCKET task_netudp(task_ctx *task, const char *host, uint16_t port);
void task_netsend(task_ctx *task, SOCKET fd, void *data, size_t len, pack_type type);

#endif //SREY_H_
