#ifndef SVPUB_H_
#define SVPUB_H_

#include "protocol/protos.h"
#include "event/event.h"
#include "containers/sarray.h"
#include "containers/queue.h"
#include "thread/rwlock.h"
#include "thread/spinlock.h"
#include "thread/mutex.h"
#include "thread/cond.h"
#include "utils/tw.h"

#define INVALID_TNAME         0
typedef struct scheduler_ctx scheduler_ctx;
typedef struct task_ctx task_ctx;
typedef struct message_ctx message_ctx;
typedef struct task_dispatch_arg task_dispatch_arg;

typedef void(*_task_dispatch_cb)(task_dispatch_arg *arg);
typedef void(*_task_startup_cb)(task_ctx *task);
typedef void(*_task_closing_cb)(task_ctx *task);
typedef void(*_timeout_cb)(task_ctx *task, uint64_t sess);
typedef void(*_request_cb)(task_ctx *task, uint8_t reqtype, uint64_t sess, name_t src, void *data, size_t size);
typedef void(*_response_cb)(task_ctx *task, uint64_t sess, int32_t error, void *data, size_t size);
typedef void(*_net_accept_cb)(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype);
typedef void(*_net_recv_cb)(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype, uint8_t client, uint8_t slice, void *data, size_t size);
typedef void(*_net_send_cb)(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype, uint8_t client, size_t size);
typedef void(*_net_connect_cb)(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype, int32_t erro);
typedef void(*_net_ssl_exchanged_cb)(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype, uint8_t client);
typedef void(*_net_handshake_cb)(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype, uint8_t client, int32_t erro);
typedef void(*_net_close_cb)(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype, uint8_t client);
typedef void(*_net_recvfrom_cb)(task_ctx *task, SOCKET fd, uint64_t skid, char ip[IP_LENS], uint16_t port, void *data, size_t size);

typedef enum msg_type {
    MSG_TYPE_NONE = 0x00,
    MSG_TYPE_STARTUP,
    MSG_TYPE_CLOSING,
    MSG_TYPE_TIMEOUT,
    MSG_TYPE_ACCEPT,
    MSG_TYPE_CONNECT,
    MSG_TYPE_SSLEXCHANGED,
    MSG_TYPE_HANDSHAKED,
    MSG_TYPE_RECV,
    MSG_TYPE_SEND,
    MSG_TYPE_CLOSE,
    MSG_TYPE_RECVFROM,
    MSG_TYPE_REQUEST,
    MSG_TYPE_RESPONSE,

    MSG_TYPE_ALL
}msg_type;
struct message_ctx {
    uint8_t mtype;//msg_type
    uint8_t pktype;//unpack_type
    uint8_t slice;//slice_type
    uint8_t client;
    int32_t erro;
    name_t src;
    SOCKET fd;
    void *data;
    size_t size;
    uint64_t skid;
    uint64_t sess;
};
QUEUE_DECL(message_ctx, qu_message);
typedef struct worker_version {
    uint8_t msgtype;
    name_t name;
    uint32_t ckver;
    uint32_t ver;
}worker_version;
typedef struct monitor_ctx {
    uint8_t stop;
    worker_version *version;
    pthread_t thread_monitor;
}monitor_ctx;
QUEUE_DECL(name_t, qu_task);
typedef struct worker_ctx {
    uint16_t index;
    scheduler_ctx *scheduler;
    pthread_t thread_worker;
#if !SCHEDULER_GLOBAL
    int32_t waiting;
    spin_ctx lcktasks;
    qu_task_ctx qutasks;
    mutex_ctx mutex;
    cond_ctx cond;
#endif
}worker_ctx;
#if WITH_SSL
typedef struct certs_ctx {
    name_t name;
    struct evssl_ctx *ssl;
}certs_ctx;
ARRAY_DECL(certs_ctx, arr_certs);
#endif
struct scheduler_ctx {
    uint8_t stop;
    uint16_t nworker;
    int32_t waiting;
    atomic64_t index;
    worker_ctx *worker;
    struct hashmap *maptasks;
#if WITH_SSL
    arr_certs_ctx arrcerts;
    rwlock_ctx lckcerts;
#endif
    rwlock_ctx lckmaptasks;
#if SCHEDULER_GLOBAL
    spin_ctx lckglobal;
    qu_task_ctx quglobal;
    mutex_ctx mutex;
    cond_ctx cond;
#endif
    monitor_ctx monitor;
    tw_ctx tw;
    ev_ctx netev;
};
struct task_ctx {
    uint8_t global;
    name_t name;
    atomic_t closing;
    atomic_t ref;
    void *arg;
    free_cb _arg_free;
    scheduler_ctx *scheduler;
#if WITH_CORO
    struct coro_ctx *coro;
#endif
    _task_dispatch_cb _task_dispatch;
    _task_startup_cb _task_startup;
    _task_closing_cb _task_closing;
    _net_accept_cb _net_accept;
    _net_recv_cb _net_recv;
    _net_send_cb _net_send;
    _net_connect_cb _net_connect;
    _net_handshake_cb _net_handshaked;
    _net_close_cb _net_close;
    _net_recvfrom_cb _net_recvfrom;
    _request_cb _request;
    _response_cb _response;
    _net_ssl_exchanged_cb _auth_ssl;
    spin_ctx lckmsg;
    qu_message_ctx qumsg;
};
struct task_dispatch_arg {
    task_ctx *task;
    message_ctx msg;
};

void _message_dispatch(task_dispatch_arg *arg);
void _message_run(task_ctx *task, message_ctx *msg);
int32_t _message_handshaked_push(SOCKET fd, uint64_t skid, int32_t client, ud_cxt *ud, int32_t erro, void *data, size_t lens);
void _task_message_push(task_ctx *task, message_ctx *msg);
int32_t _message_should_clean(message_ctx *msg);
void _message_clean(msg_type mtype, pack_type pktype, void *data);
#if WITH_CORO
void _mcoro_init(size_t stack_size);
void _mcoro_new(task_ctx *task);
void _mcoro_free(task_ctx *task);
#endif

#endif//SVPUB_H_
