#include "service/srey.h"
#include "sarray.h"
#include "tw.h"
#include "cond.h"
#include "rwlock.h"
#include "queue.h"
#include "hashmap.h"

QUEUE_DECL(message_ctx, qu_message);
#if WITH_SSL
typedef struct certs_ctx {
    struct evssl_ctx *ssl;
    char name[64];
}certs_ctx;
ARRAY_DECL(certs_ctx, arr_certs);
#endif
struct srey_ctx {
    int32_t stop;
    int32_t waiting;
    uint32_t nworker;
    pthread_t *thworker;
#if WITH_SSL
    arr_certs arrcert;
    rwlock_ctx lckarrcert;
#endif
    struct hashmap *maptask;
    rwlock_ctx lckmaptask;
    mutex_ctx muworker;
    cond_ctx condworker;
    qu_void quglobal;
    tw_ctx tw;
    ev_ctx netev;
};
struct task_ctx {
    int32_t name;
    int32_t global;
    uint32_t maxcnt;
    task_run _run;
    task_free _free;
    void *handle;
    srey_ctx *srey;
    atomic64_t session;
    mutex_ctx mutask;
    qu_message qumsg;
};

static inline uint64_t _maptask_hash(const void *item, uint64_t seed0, uint64_t seed1) {
    return hash((const char *)&((*(task_ctx **)item)->name), sizeof((*(task_ctx **)item)->name));
}
static inline int _maptask_compare(const void *a, const void *b, void *ud) {
    return (*(task_ctx **)a)->name - (*(task_ctx **)b)->name;
}
static inline void _push_message(task_ctx *task, message_ctx *msg) {
    mutex_lock(&task->mutask);
    qu_message_push(&task->qumsg, msg);
    if (0 == task->global) {
        task->global = 1;
        mutex_lock(&task->srey->muworker);
        qu_void_push(&task->srey->quglobal, (void **)&task);
        if (task->srey->waiting > 0) {
            cond_signal(&task->srey->condworker);
        }
        mutex_unlock(&task->srey->muworker);
    }
    mutex_unlock(&task->mutask);
}
static inline void _message_clean(message_ctx *msg) {
    switch (msg->type) {
    case MSG_TYPE_RECV:
    case MSG_TYPE_RECVFROM:
    case MSG_TYPE_USER:
        FREE(msg->data);
        break;
    default:
        break;
    }
}
static inline void _task_run(srey_ctx *ctx, task_ctx *task) {
    message_ctx *tmp;
    message_ctx msg;
    for (uint32_t i = 0; i < task->maxcnt; i++) {
        mutex_lock(&task->mutask);
        tmp = qu_message_pop(&task->qumsg);
        if (NULL == tmp) {
            mutex_unlock(&task->mutask);
            break;
        }
        msg = *tmp;
        mutex_unlock(&task->mutask);
        task->_run(task, &msg);
        _message_clean(&msg);
    }
}
task_ctx *srey_tasknew(srey_ctx *ctx, int32_t name, uint32_t maxcnt, 
    task_new _init, task_run _run, task_free _tfree, void *arg) {
    ASSERTAB(NULL != _run, ERRSTR_INVPARAM);
    task_ctx *task;
    MALLOC(task, sizeof(task_ctx));
    ZERO(task, sizeof(task_ctx));
    task->session = 1;
    task->name = name;
    task->maxcnt = maxcnt;
    task->_run = _run;
    task->_free = _tfree;
    task->srey = ctx;
    mutex_init(&task->mutask);
    qu_message_init(&task->qumsg, QUMSG_INITLENS);
    if (NULL != _init) {
        task->handle = _init(task, arg);
    }
    rwlock_wrlock(&ctx->lckmaptask);
    ASSERTAB(NULL == hashmap_set(ctx->maptask, &task), formatv("task %d repeat.", name));
    rwlock_unlock(&ctx->lckmaptask);
    message_ctx msg;
    msg.type = MSG_TYPE_STARTED;
    _push_message(task, &msg);
    return task;
}
static void _free_task(void *item) {
    task_ctx *task = *(task_ctx **)item;
    message_ctx closing;
    closing.type = MSG_TYPE_CLOSING;
    task->_run(task, &closing);
    if (NULL != task->_free) {
        task->_free(task);
    }
    message_ctx *msg;
    while (NULL != (msg = qu_message_pop(&task->qumsg))) {
        _message_clean(msg);
    }
    qu_message_free(&task->qumsg);
    mutex_free(&task->mutask);
    FREE(task);
}
task_ctx *srey_taskqury(srey_ctx *ctx, int32_t name) {
    task_ctx key;
    key.name = name;
    task_ctx *pkey = &key;
    rwlock_rdlock(&ctx->lckmaptask);
    void **tmp = (void **)hashmap_get(ctx->maptask, &pkey);
    task_ctx *task = (NULL == tmp ? NULL : *tmp);
    rwlock_unlock(&ctx->lckmaptask);
    return task;
}
#if WITH_SSL
static inline certs_ctx *_certs_get(srey_ctx *ctx, const char *name) {
    certs_ctx *cert;
    for (size_t i = 0; i < arr_certs_size(&ctx->arrcert); i++) {
        cert = arr_certs_at(&ctx->arrcert, i);
        if (0 == strcmp(name, cert->name)) {
            return cert;
        }
    }
    return NULL;
}
void certs_register(srey_ctx *ctx, const char *name, struct evssl_ctx *evssl) {
    certs_ctx cert;
    ASSERTAB(strlen(name) < sizeof(cert.name), "cert name too long.");
    ZERO(&cert, sizeof(certs_ctx));
    strcpy(cert.name, name);
    cert.ssl = evssl;
    rwlock_wrlock(&ctx->lckarrcert);
    if (NULL != _certs_get(ctx, name)) {
        ASSERTAB(0, "cert name repeat.")
    }
    arr_certs_push_back(&ctx->arrcert, &cert);
    rwlock_unlock(&ctx->lckarrcert);
}
struct evssl_ctx *certs_qury(srey_ctx *ctx, const char *name) {
    certs_ctx *cert;
    rwlock_rdlock(&ctx->lckarrcert);
    cert = _certs_get(ctx, name);
    rwlock_unlock(&ctx->lckarrcert);
    return NULL == cert ? NULL : cert->ssl;
}
#endif
srey_ctx *task_srey(task_ctx *task) {
    return task->srey;
}
ev_ctx *task_netev(task_ctx *task) {
    return &task->srey->netev;
}
void *task_handle(task_ctx *task) {
    return task->handle;
}
int32_t task_name(task_ctx *task) {
    return task->name;
}
uint64_t task_session(task_ctx *task) {
    return (uint64_t)ATOMIC64_ADD(&task->session, 1);
}
void task_user(task_ctx *dst, task_ctx *src, uint64_t session, void *data, size_t size, int32_t copy) {
    message_ctx msg;
    msg.type = MSG_TYPE_USER;
    msg.session = session;
    msg.src = src;
    if (0 != copy) {
        MALLOC(msg.data, size);
        memcpy(msg.data, data, size);
    } else {
        msg.data = data;
    }
    msg.size = size;
    _push_message(dst, &msg);
}
static inline void _srey_timeout(ud_cxt *ud) {
    message_ctx msg;
    msg.type = MSG_TYPE_TIMEOUT;
    msg.session = ud->session;
    _push_message((void *)ud->data, &msg);
}
void task_timeout(task_ctx *task, uint64_t session, uint32_t timeout) {
    ud_cxt ud;
    ud.data = task;
    ud.session = session;
    tw_add(&task->srey->tw, timeout, _srey_timeout, &ud);
}
static inline int32_t _task_net_accept(ev_ctx *ev, SOCKET fd, ud_cxt *ud) {
    message_ctx msg;
    msg.type = MSG_TYPE_ACCEPT;
    msg.ptype = ud->type;
    msg.session = ud->session;
    msg.fd = fd;
    _push_message(ud->data, &msg);
    return ERR_OK;
}
static inline void _task_net_recv(ev_ctx *ev, SOCKET fd, buffer_ctx *buf, size_t size, ud_cxt *ud) {
    size_t lens;
    void *data;
    message_ctx msg;
    msg.type = MSG_TYPE_RECV;
    msg.ptype = ud->type;
    msg.session = ud->session;
    msg.fd = fd;
    while (NULL != (data = protos_unpack(ud->type, buf, &lens, ud))) {
        msg.data = data;
        msg.size = lens;
        _push_message(ud->data, &msg);
    }    
}
static inline void _task_net_send(ev_ctx *ev, SOCKET fd, size_t size, ud_cxt *ud) {
    message_ctx msg;
    msg.type = MSG_TYPE_SEND;
    msg.ptype = ud->type;
    msg.session = ud->session;
    msg.fd = fd;
    msg.size = size;
    _push_message(ud->data, &msg);
}
static inline void _task_net_close(ev_ctx *ev, SOCKET fd, ud_cxt *ud) {
    message_ctx msg;
    msg.type = MSG_TYPE_CLOSE;
    msg.ptype = ud->type;
    msg.session = ud->session;
    msg.fd = fd;
    _push_message(ud->data, &msg);
}
int32_t task_netlisten(task_ctx *task, unpack_type type, struct evssl_ctx *evssl,
    const char *host, uint16_t port, int32_t sendev) {
    ud_cxt ud;
    ud.index = 0;
    ud.type = type;
    ud.status = 0;
    ud.data = task;
    ud.session = task_session(task);
    cbs_ctx cbs;
    ZERO(&cbs, sizeof(cbs));
    cbs.acp_cb = _task_net_accept;
    cbs.r_cb = _task_net_recv;
    cbs.c_cb = _task_net_close;
    if (0 != sendev) {
        cbs.s_cb = _task_net_send;
    }
    return ev_listen(&task->srey->netev, evssl, host, port, &cbs, &ud);
}
static inline int32_t _task_net_connect(ev_ctx *ev, SOCKET fd, int32_t err, ud_cxt *ud) {
    message_ctx msg;
    msg.type = MSG_TYPE_CONNECT;
    msg.ptype = ud->type;
    msg.session = ud->session;
    msg.fd = fd;
    msg.error = err;
    _push_message(ud->data, &msg);
    return ERR_OK;
}
SOCKET task_netconnect(task_ctx *task, unpack_type type, uint64_t session, struct evssl_ctx *evssl,
    const char *host, uint16_t port, int32_t sendev) {
    ud_cxt ud;
    ud.type = type;
    ud.status = 0;
    ud.data = task;
    ud.session = session;
    cbs_ctx cbs;
    ZERO(&cbs, sizeof(cbs));
    cbs.conn_cb = _task_net_connect;
    cbs.r_cb = _task_net_recv;
    cbs.c_cb = _task_net_close;
    if (0 != sendev) {
        cbs.s_cb = _task_net_send;
    }
    return ev_connect(&task->srey->netev, evssl, host, port, &cbs, &ud);
}
static inline void _task_net_recvfrom(ev_ctx *ev, SOCKET fd, char *buf, size_t size, netaddr_ctx *addr, ud_cxt *ud) {
    message_ctx msg;
    msg.type = MSG_TYPE_RECVFROM;
    msg.ptype = ud->type;
    msg.session = ud->session;
    msg.fd = fd;
    MALLOC(msg.data, size);
    memcpy(msg.data, buf, size);
    msg.size = size;
    memcpy(&msg.addr, addr, sizeof(netaddr_ctx));
    _push_message(ud->data, &msg);
}
SOCKET task_netudp(task_ctx *task, unpack_type type, const char *host, uint16_t port) {
    ud_cxt ud;
    ud.status = 0;
    ud.type = type;
    ud.data = task;
    ud.session = task_session(task);
    return ev_udp(&task->srey->netev, host, port, _task_net_recvfrom, &ud);
}
void task_netsend(task_ctx *task, SOCKET fd, void *data, size_t len, pack_type type) {
    size_t size;
    void *pack = protos_pack(type, data, len, &size);
    ev_send(task_netev(task), fd, pack, size, 0);
}
static void _loop_worker(void *arg) {
    void **tmp;
    task_ctx *task;
    srey_ctx *ctx = (srey_ctx *)arg;
    while (0 == ctx->stop) {
        //从全局队列取一任务
        mutex_lock(&ctx->muworker);
        tmp = qu_void_pop(&ctx->quglobal);
        if (NULL == tmp) {
            task = NULL;
            ctx->waiting++;
            cond_wait(&ctx->condworker, &ctx->muworker);
            ctx->waiting--;
        } else {
            task = *tmp;
        }
        mutex_unlock(&ctx->muworker);
        if (NULL == task) {
            continue;
        }
        //执行
        _task_run(ctx, task);
        //是否加回全局队列
        mutex_lock(&task->mutask);
        if (0 == qu_message_size(&task->qumsg)) {
            task->global = 0;
        } else {
            mutex_lock(&ctx->muworker);
            qu_void_push(&ctx->quglobal, (void **)&task);
            mutex_unlock(&ctx->muworker);
        }
        mutex_unlock(&task->mutask);
    }
}
srey_ctx *srey_init(uint32_t nnet, uint32_t nworker) {
    srey_ctx *ctx;
    MALLOC(ctx, sizeof(srey_ctx));
    ZERO(ctx, sizeof(srey_ctx));
    ctx->nworker = nworker;
    MALLOC(ctx->thworker, sizeof(pthread_t) * ctx->nworker);
    mutex_init(&ctx->muworker);
    cond_init(&ctx->condworker);
    qu_void_init(&ctx->quglobal, ONEK);
    rwlock_init(&ctx->lckmaptask);
    ctx->maptask = hashmap_new_with_allocator(_malloc, _realloc, _free,
                                              sizeof(task_ctx *), ONEK, 0, 0,
                                              _maptask_hash, _maptask_compare, _free_task, NULL);
#if WITH_SSL
    rwlock_init(&ctx->lckarrcert);
    arr_certs_init(&ctx->arrcert, 16);
#endif
    for (uint32_t i = 0; i < ctx->nworker; i++) {
        ctx->thworker[i] = thread_creat(_loop_worker, ctx);
    }

    tw_init(&ctx->tw);
    protos_init();
    ev_init(&ctx->netev, nnet);
    return ctx;
}
void srey_free(srey_ctx *ctx) {
    ev_free(&ctx->netev);
    tw_free(&ctx->tw);

    ctx->stop = 1;
    mutex_lock(&ctx->muworker);
    cond_broadcast(&ctx->condworker);
    mutex_unlock(&ctx->muworker);
    for (uint32_t i = 0; i < ctx->nworker; i++) {
        thread_join(ctx->thworker[i]);
    }
    hashmap_free(ctx->maptask);
    rwlock_free(&ctx->lckmaptask);
#if WITH_SSL
    certs_ctx *cert;
    for (size_t i = 0; i < arr_certs_size(&ctx->arrcert); i++) {
        cert = arr_certs_at(&ctx->arrcert, i);
        evssl_free(cert->ssl);
    }
    arr_certs_free(&ctx->arrcert);
    rwlock_free(&ctx->lckarrcert);
#endif
    mutex_free(&ctx->muworker);
    cond_free(&ctx->condworker);
    qu_void_free(&ctx->quglobal);
    FREE(ctx->thworker);
    FREE(ctx);
}
