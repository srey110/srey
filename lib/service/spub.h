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
#define RECORD_WORKER_LOAD    0
#if WITH_CORO
struct coro_ctx;
#endif
typedef struct srey_ctx srey_ctx;
typedef struct task_ctx task_ctx;
typedef struct message_ctx message_ctx;
typedef void *(*task_new)(task_ctx *task, void *arg);
typedef void(*task_run)(task_ctx *task, message_ctx *msg);

typedef enum msg_type {
    MSG_TYPE_NONE = 0x00,
    MSG_TYPE_WAKEUP,//mtype sess erro
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

    MSG_TYPE_CNT
}msg_type;
typedef enum task_type{
    TTYPE_C = 0x00,
#if WITH_LUA
    TTYPE_LUA,
#endif
    TTYPE_CNT
}task_type;

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

typedef struct initer_msg {
    name_t src;
    task_ctx *task;
    uint64_t sess;
}initer_msg;
QUEUE_DECL(initer_msg, qu_initer);
typedef struct initer_ctx {
    uint8_t stop;
    uint8_t waiting;
    pthread_t thread;
    mutex_ctx mutex;
    cond_ctx cond;
    qu_initer qutask;
}initer_ctx;

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
    initer_ctx initer;
    tw_ctx tw;
    ev_ctx netev;
};

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
    task_new _init;
    task_run _run;
    free_cb _free;
    free_cb _arg_free;
    void *arg;
    void *handle;
    srey_ctx *srey;
#if WITH_CORO
    struct coro_ctx *coro;
#endif
    spin_ctx spin_msg;
    qu_message qumsg;
    qu_message qutmo;
};

typedef struct task_msg_arg {
    task_ctx *task;
    message_ctx msg;
}task_msg_arg;

#if WITH_CORO
void _dispatch_coro(task_msg_arg *arg);
void _coro_init_desc(size_t stack_size);
struct coro_ctx *_coro_new(void);
void _coro_free(struct coro_ctx *coctx);
void _coro_shrink(struct coro_ctx *coctx);
#endif

#endif//SPUB_H_
