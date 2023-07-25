#ifndef SPUB_H_
#define SPUB_H_

#include "event/event.h"
#include "proto/protos.h"
#include "sarray.h"
#include "queue.h"
#include "rwlock.h"
#include "spinlock.h"
#include "mutex.h"
#include "cond.h"
#include "timer.h"
#include "tw.h"

#define INVALID_TNAME         0
#define RPC_NAME_LENS         64
#define SIGN_KEY_LENS         128
#define RECORD_WORKER_LOAD    0
struct coro_ctx;
typedef struct srey_ctx srey_ctx;
typedef struct task_ctx task_ctx;
typedef struct message_ctx message_ctx;
typedef void(*task_run)(task_ctx *task, message_ctx *msg);
typedef void(*ctask_timeout)(task_ctx *task, void *arg);
typedef struct cJSON *(*rpc_cb)(task_ctx *task, struct cJSON *args);

typedef enum msg_type {
    MSG_TYPE_NONE = 0x00,
    MSG_TYPE_STARTUP,//mtype
    MSG_TYPE_CLOSING,//mtype
    MSG_TYPE_TIMEOUT,//mtype sess
    MSG_TYPE_ACCEPT,//mtype pktype fd skid
    MSG_TYPE_CONNECT,//mtype pktype fd skid sess erro
    MSG_TYPE_HANDSHAKED,//mtype pktype fd skid client sess erro
    MSG_TYPE_RECV,//mtype pktype fd skid client sess slice data size
    MSG_TYPE_SEND,//mtype pktype fd skid client sess size
    MSG_TYPE_CLOSE,//mtype pktype fd skid sess
    MSG_TYPE_RECVFROM,//mtype fd skid sess data size lua: mtype fd skid sess ip port udata size
    MSG_TYPE_REQUEST,//mtype src sess data size
    MSG_TYPE_RESPONSE,//mtype sess data size

    MSG_TYPE_ALL
}msg_type;
typedef enum task_type{
    TTYPE_C = 0x00,
#if WITH_LUA
    TTYPE_LUA,
#endif
    TTYPE_CNT
}task_type;
typedef enum request_type {
    REQ_TYPE_DEF = 0x00,
    REQ_TYPE_RPC,

    REQ_TYPE_CNT
}request_type;

struct message_ctx {
    uint8_t mtype;//msg_type
    uint8_t pktype;//unpack_type
    uint8_t slice;//slice_type
    uint8_t client;
    int8_t erro;
    name_t src;
    SOCKET fd;
    void *data;
    size_t size;
    uint64_t skid;
    uint64_t sess;
};
QUEUE_DECL(message_ctx, qu_message);

#if WITH_SSL
typedef struct certs_ctx {
    name_t name;
    struct evssl_ctx *ssl;
}certs_ctx;
ARRAY_DECL(certs_ctx, arr_certs);
#endif

typedef struct worker_version {
    int8_t msgtype;
    name_t name;
    uint32_t ck_ver;
    atomic_t ver;
}worker_version;
typedef struct monitor_ctx {
    uint8_t stop;
    uint16_t interval;
    uint16_t threshold;
    worker_version *version;
    pthread_t thread;
}monitor_ctx;

QUEUE_DECL(name_t, qu_task);
ARRAY_DECL(name_t, arr_task);
typedef struct worker_ctx {
    uint8_t waiting;
    uint8_t adjusting;
    uint16_t index;
    uint16_t toindex;
    atomic_t cpu_cost;
#if RECORD_WORKER_LOAD
    atomic_t ntask;
#endif
    srey_ctx *srey;
    pthread_t thread;
    mutex_ctx mutex;
    cond_ctx cond;
    qu_task qutasks;
    timer_ctx timer;
}worker_ctx;

struct srey_ctx {
    uint8_t stop;
    uint16_t nworker;
    atomic_t index;
    worker_ctx *worker;
#if WITH_SSL
    arr_certs arrcerts;
    rwlock_ctx lckcerts;
#endif
    struct hashmap *maptasks;
    rwlock_ctx lcktasks;
    monitor_ctx monitor;
    tw_ctx tw;
    ev_ctx netev;
    char key[SIGN_KEY_LENS];
};

typedef struct rpc_ctx {
    rpc_cb rpc;
    char method[RPC_NAME_LENS];
}rpc_ctx;

struct task_ctx {
    uint8_t global;
    uint8_t ttype;
    uint16_t index;
    uint16_t maxcnt;
    uint16_t maxmsgqulens;
    name_t name;
    uint32_t cpu_cost;
    uint32_t warning;
    atomic_t closing;
    atomic_t ref;
    free_cb _arg_free;
    void *arg;
    srey_ctx *srey;
    struct coro_ctx *coro;
    struct hashmap *maprpc;
    spin_ctx spin_msg;
    qu_message qumsg;
    qu_message qutmo;
    qu_ptr qutmoarg;
    task_run _request[REQ_TYPE_CNT];
    task_run _run[MSG_TYPE_ALL];
};

typedef struct task_msg_arg {
    task_ctx *task;
    message_ctx msg;
}task_msg_arg;

void _coro_init_desc(size_t stack_size);
void _coro_new(task_ctx *task);
void _coro_free(task_ctx *task);
void _dispatch_coro(task_msg_arg *arg);

void _rpc_new(task_ctx *task);
void _rpc_free(task_ctx *task);
void _ctask_rpc(task_ctx *task, message_ctx *msg);

#endif//SPUB_H_
