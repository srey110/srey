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
    int32_t startup;
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
    uint8_t global;
    uint8_t maxcnt;
    uint8_t dmaxcnt;
    int32_t name;
    atomic_t startup;
    task_run _run;
    task_free _free;
    void *handle;
    srey_ctx *srey;
    uint64_t session;
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
    switch (msg->msgtype) {
    case MSG_TYPE_RECV:
    case MSG_TYPE_RECVFROM:
        protos_pkfree(msg->pktype, msg->data);
        break;
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
    mutex_lock(&task->mutask);
    size_t size = qu_message_size(&task->qumsg);
    mutex_unlock(&task->mutask);
    uint8_t nloop = (size > (size_t)task->dmaxcnt ? task->dmaxcnt : task->maxcnt);
    for (uint8_t i = 0; i < nloop; i++) {
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
    CALLOC(task, 1, sizeof(task_ctx));
    task->session = 1;
    task->name = name;
    task->maxcnt = maxcnt;
    task->dmaxcnt = maxcnt * 2;
    task->_run = _run;
    task->_free = _tfree;
    task->srey = ctx;
    mutex_init(&task->mutask);
    qu_message_init(&task->qumsg, QUMSG_INITLENS);
    if (NULL != _init) {
        task->handle = _init(task, arg);
    }
    int32_t started = 0;
    rwlock_wrlock(&ctx->lckmaptask);
    ASSERTAB(NULL == hashmap_set(ctx->maptask, &task), formatv("task %d repeat.", name));
    started = ctx->startup;
    rwlock_unlock(&ctx->lckmaptask);
    if (0 != started) {
        if (ATOMIC_CAS(&task->startup, 0, 1)) {
            message_ctx msg;
            msg.msgtype = MSG_TYPE_STARTED;
            _push_message(task, &msg);
        }
    }
    return task;
}
static void _free_task(void *item) {
    task_ctx *task = *(task_ctx **)item;
    message_ctx closing;
    closing.msgtype = MSG_TYPE_CLOSING;
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
ev_ctx *srey_netev(srey_ctx *ctx) {
    return &ctx->netev;
}
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
    return task->session++;
}
void task_user(task_ctx *dst, int32_t src, uint64_t session, void *data, size_t size, int32_t copy) {
    message_ctx msg;
    msg.msgtype = MSG_TYPE_USER;
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
    msg.msgtype = MSG_TYPE_TIMEOUT;
    msg.session = ud->session;
    _push_message((void *)ud->data, &msg);
}
void task_timeout(task_ctx *task, uint64_t session, uint32_t timeout) {
    ud_cxt ud;
    ud.data = task;
    ud.session = session;
    tw_add(&task->srey->tw, timeout, _srey_timeout, &ud);
}
static inline void _push_acptmsg(SOCKET fd, ud_cxt *ud) {
    message_ctx msg;
    msg.msgtype = MSG_TYPE_ACCEPT;
    msg.pktype = ud->pktype;
    msg.session = ud->session;
    msg.fd = fd;
    _push_message(ud->data, &msg);
}
static inline int32_t _task_net_accept(ev_ctx *ev, SOCKET fd, ud_cxt *ud) {
    if (ERR_OK != protos_handshake(ev, fd, ud, _push_acptmsg)) {
        return ERR_FAILED;
    }
    if (NULL == ud->hscb){
        _push_acptmsg(fd, ud);
    }
    return ERR_OK;
}
static inline void _task_net_recv(ev_ctx *ev, SOCKET fd, buffer_ctx *buf, size_t size, ud_cxt *ud) {
    message_ctx msg;
    msg.msgtype = MSG_TYPE_RECV;
    msg.pktype = ud->pktype;
    msg.session = ud->session;
    msg.fd = fd;
    void *data;
    size_t lens = 0;
    int32_t closefd = 0;
    do {
        data = protos_unpack(ev, fd, buf, &lens, ud, &closefd);
        if (NULL != data) {
            msg.data = data;
            msg.size = lens;
            _push_message(ud->data, &msg);
        }
    } while (NULL != data && 0 != buffer_size(buf));
    if (0 != closefd) {
        ev_close(ev, fd);
    }
}
static inline void _task_net_send(ev_ctx *ev, SOCKET fd, size_t size, ud_cxt *ud) {
    message_ctx msg;
    msg.msgtype = MSG_TYPE_SEND;
    msg.pktype = ud->pktype;
    msg.session = ud->session;
    msg.fd = fd;
    msg.size = size;
    _push_message(ud->data, &msg);
}
static inline void _push_connmsg(SOCKET fd, int32_t err, ud_cxt *ud) {
    message_ctx msg;
    msg.msgtype = MSG_TYPE_CONNECT;
    msg.pktype = ud->pktype;
    msg.session = ud->session;
    msg.fd = fd;
    msg.error = err;
    _push_message(ud->data, &msg);
}
static inline void _task_net_close(ev_ctx *ev, SOCKET fd, ud_cxt *ud) {
    if (NULL != ud->hscb
        && 0 == ud->status) {
        if (0 == ud->svside) {
            _push_connmsg(fd, ERR_FAILED, ud);
        }
        return;
    }
    message_ctx msg;
    msg.msgtype = MSG_TYPE_CLOSE;
    msg.pktype = ud->pktype;
    msg.session = ud->session;
    msg.fd = fd;
    _push_message(ud->data, &msg);
}
int32_t task_netlisten(task_ctx *task, pack_type pktype, struct evssl_ctx *evssl,
    const char *host, uint16_t port, int32_t sendev) {
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.pktype = pktype;
    ud.svside = 1;
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
    cbs.ud_free = protos_udfree;
    return ev_listen(&task->srey->netev, evssl, host, port, &cbs, &ud);
}
static inline int32_t _task_net_connect(ev_ctx *ev, SOCKET fd, int32_t err, ud_cxt *ud) {
    if (ERR_OK != err) {
        _push_connmsg(fd, err, ud);
        return ERR_OK;
    }
    if (ERR_OK != protos_handshake(ev, fd, ud, _push_connmsg)) {
        _push_connmsg(fd, ERR_FAILED, ud);
        return ERR_FAILED;
    }
    if (NULL == ud->hscb) {
        _push_connmsg(fd, err, ud);
    }
    return ERR_OK;
}
SOCKET task_netconnect(task_ctx *task, pack_type pktype, uint64_t session, struct evssl_ctx *evssl,
    const char *host, uint16_t port, int32_t sendev) {
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.pktype = pktype;
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
    cbs.ud_free = protos_udfree;
    return ev_connect(&task->srey->netev, evssl, host, port, &cbs, &ud);
}
static inline void _task_net_recvfrom(ev_ctx *ev, SOCKET fd, char *buf, size_t size, netaddr_ctx *addr, ud_cxt *ud) {
    message_ctx msg;
    msg.msgtype = MSG_TYPE_RECVFROM;
    msg.pktype = ud->pktype;
    msg.session = ud->session;
    msg.fd = fd;
    udp_msg_ctx *umsg;
    MALLOC(umsg, sizeof(udp_msg_ctx) + size);
    memcpy(&umsg->addr, addr, sizeof(netaddr_ctx));
    memcpy(umsg->data, buf, size);
    msg.data = umsg;
    msg.size = size;
    _push_message(ud->data, &msg);
}
SOCKET task_netudp(task_ctx *task, pack_type pktype, const char *host, uint16_t port) {
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.pktype = pktype;
    ud.data = task;
    ud.session = task_session(task);
    cbs_ctx cbs;
    ZERO(&cbs, sizeof(cbs));
    cbs.rf_cb = _task_net_recvfrom;
    cbs.ud_free = protos_udfree;
    return ev_udp(&task->srey->netev, host, port, &cbs, &ud);
}
void task_netsend(ev_ctx *ev, SOCKET fd, void *data, size_t len, pack_type pktype) {
    size_t size;
    void *pack = protos_pack(pktype, data, len, &size);
    ev_send(ev, fd, pack, size, 0);
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
    CALLOC(ctx, 1, sizeof(srey_ctx));
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
    ev_init(&ctx->netev, nnet);
    return ctx;
}
static inline bool _map_scan(const void *item, void *udata) {
    task_ctx *task = *(task_ctx **)item;
    if (!ATOMIC_CAS(&task->startup, 0, 1)){
        return true;
    }
    message_ctx *msg = udata;
    _push_message(task, msg);
    return true;
}
void srey_startup(srey_ctx *ctx) {
    message_ctx msg;
    msg.msgtype = MSG_TYPE_STARTED;
    rwlock_rdlock(&ctx->lckmaptask);
    ctx->startup = 1;
    hashmap_scan(ctx->maptask, _map_scan, &msg);
    rwlock_unlock(&ctx->lckmaptask);
}
void srey_free(srey_ctx *ctx) {
    ctx->stop = 1;
    mutex_lock(&ctx->muworker);
    cond_broadcast(&ctx->condworker);
    mutex_unlock(&ctx->muworker);
    for (uint32_t i = 0; i < ctx->nworker; i++) {
        thread_join(ctx->thworker[i]);
    }
    ev_free(&ctx->netev);
    tw_free(&ctx->tw);

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
