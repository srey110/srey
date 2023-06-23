#include "service/srey.h"
#include "service/maps.h"
#include "sarray.h"
#include "hashmap.h"
#include "tw.h"
#include "cond.h"
#include "rwlock.h"
#include "queue.h"
#include "loger.h"
#include "minicoro.h"

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
    mco_desc codesc;
};
QUEUE_DECL(mco_coro *, qu_copool);
struct task_ctx {
    uint8_t global;
    uint8_t maxcnt;
    uint8_t dmaxcnt;
    uint8_t closed;
    int32_t name;
    atomic_t startup;
    task_run _run;
    task_free _free;
    void *handle;
    srey_ctx *srey;
    mco_coro *curco;
    mapco_ctx mapco;
    uint64_t session;
    mutex_ctx mutask;
    qu_message qumsg;
    qu_copool qucopool;
};
typedef struct co_arg_ctx {
    task_ctx *task;
    message_ctx msg;
}co_arg_ctx;

#define RESUME_NORMAL(arg)\
    arg->task->curco = _co_create(arg->task);\
    ASSERTAB(MCO_SUCCESS == mco_push(arg->task->curco, arg, sizeof(co_arg_ctx)), "mco_push failed!");\
    ASSERTAB(MCO_SUCCESS == mco_resume(arg->task->curco), "resume coroutine failed!")
#define YIELD(task) ASSERTAB(MCO_SUCCESS == mco_yield(task->curco), "yield coroutine failed!")
#define RESUME(task, co) \
    task->curco = co;\
    ASSERTAB(MCO_SUCCESS == mco_resume(co), "resume coroutine failed!")

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
    case MSG_TYPE_REQUEST:
    case MSG_TYPE_RESPONSE:
        FREE(msg->data);
        break;
    default:
        break;
    }
}
static inline mco_coro *_co_create(task_ctx *task) {
    mco_coro **co = qu_copool_pop(&task->qucopool);
    if (NULL != co) {
        ASSERTAB(MCO_SUCCESS == mco_init(*co, &task->srey->codesc), "init coroutine failed!");
        return *co;
    }
    mco_coro *conew;
    ASSERTAB(MCO_SUCCESS == mco_create(&conew, &task->srey->codesc), "create coroutine failed!");
    return conew;
}
static inline void _co_cb(mco_coro *co) {
    co_arg_ctx arg;
    ASSERTAB(MCO_SUCCESS == mco_pop(co, &arg, sizeof(arg)), "mco_pop failed!");
    arg.task->_run(arg.task, &arg.msg);
    if (MSG_TYPE_CLOSING == arg.msg.msgtype) {
        arg.task->closed = 1;
    }
    qu_copool_push(&arg.task->qucopool, &co);
    _message_clean(&arg.msg);
}
static inline void _dispatch_timeout(co_arg_ctx *arg) {
    co_tmo_ctx cotmo;
    if (ERR_OK != _map_cotmo_get(&arg->task->mapco, arg->msg.session, &cotmo)) {
        return;
    }
    switch (cotmo.type) {
    case TMO_TYPE_SLEEP:
        RESUME(arg->task, cotmo.co.co);
        break;
    case TMO_TYPE_NORMAL:
        RESUME_NORMAL(arg);
        break;
    case TMO_TYPE_CONNECT:
        _map_cosess_del(&arg->task->mapco, arg->msg.session);
        arg->msg.erro = ERR_FAILED;
        ASSERTAB(MCO_SUCCESS == mco_push(cotmo.co.co, &arg->msg, sizeof(arg->msg)), "mco_push failed!");
        RESUME(arg->task, cotmo.co.co);
        LOG_WARN("connect timeout. session:%"PRIu64, arg->msg.session);
        break;
    case TMO_TYPE_SYNSEND:
        _map_cosk_del(&arg->task->mapco, cotmo.fd);
        arg->msg.erro = ERR_FAILED;
        ASSERTAB(MCO_SUCCESS == mco_push(cotmo.co.co, &arg->msg, sizeof(arg->msg)), "mco_push failed!");
        RESUME(arg->task, cotmo.co.co);
        LOG_WARN("synsend(to) timeout. sock:%d", (int32_t)cotmo.fd);
        break;
    default:
        break;
    }
}
static inline void _dispatch_netrd(co_arg_ctx *arg) {
    if (1 == arg->msg.synflag) {
        co_sock_ctx cosk;
        if (ERR_OK == _map_cosk_get(&arg->task->mapco, arg->msg.fd, &cosk)) {
            _map_cotmo_del(&arg->task->mapco, cosk.co.session);
            arg->msg.erro = ERR_OK;
            ASSERTAB(MCO_SUCCESS == mco_push(cosk.co.co, &arg->msg, sizeof(arg->msg)), "mco_push failed!");
            RESUME(arg->task, cosk.co.co);
        } else {
            LOG_WARN("can't find sock %d, maybe timeout already.", (int32_t)arg->msg.fd);
        }
        _message_clean(&arg->msg);
    } else {
        RESUME_NORMAL(arg);
    }
}
static inline void _task_onmsg(co_arg_ctx *arg) {
    if (0 != arg->task->closed) {
        _message_clean(&arg->msg);
        return;
    }
    switch (arg->msg.msgtype) {
    case MSG_TYPE_STARTED:
        RESUME_NORMAL(arg);
        break;
    case MSG_TYPE_CLOSING:
        RESUME_NORMAL(arg);
        break;
    case MSG_TYPE_TIMEOUT:
        _dispatch_timeout(arg);
        break;
    case MSG_TYPE_ACCEPT:
        RESUME_NORMAL(arg);
        break;
    case MSG_TYPE_CONNECT:
        if (1 == arg->msg.synflag) {
            co_sess_ctx cosess;
            if (ERR_OK == _map_cosess_get(&arg->task->mapco, arg->msg.session, &cosess)) {
                _map_cotmo_del(&arg->task->mapco, arg->msg.session);
                ASSERTAB(MCO_SUCCESS == mco_push(cosess.co, &arg->msg, sizeof(arg->msg)), "mco_push failed!");
                RESUME(arg->task, cosess.co);
            } else {
                if (ERR_OK == arg->msg.erro) {
                    ev_close(task_netev(arg->task), arg->msg.fd, arg->msg.skid);
                }
                LOG_WARN("can't find connect session %"PRIu64", maybe timeout already.", arg->msg.session);
            }
        } else {
            RESUME_NORMAL(arg);
        }
        break;
    case MSG_TYPE_HANDSHAKED:
        RESUME_NORMAL(arg);
        break;
    case MSG_TYPE_RECV:
        _dispatch_netrd(arg);
        break;
    case MSG_TYPE_SEND:
        RESUME_NORMAL(arg);
        break;
    case MSG_TYPE_CLOSE:
        if (1 == arg->msg.synflag) {
            co_sock_ctx cosk;
            if (ERR_OK == _map_cosk_get(&arg->task->mapco, arg->msg.fd, &cosk)) {
                _map_cotmo_del(&arg->task->mapco, cosk.co.session);
                arg->msg.erro = ERR_FAILED;
                ASSERTAB(MCO_SUCCESS == mco_push(cosk.co.co, &arg->msg, sizeof(arg->msg)), "mco_push failed!");
                RESUME(arg->task, cosk.co.co);
            } else {
                LOG_WARN("can't find synsend sock %d, maybe timeout already.", (int32_t)arg->msg.fd);
            }
        }
        RESUME_NORMAL(arg);
        break;
    case MSG_TYPE_RECVFROM:
        _dispatch_netrd(arg);
        break;
    case MSG_TYPE_REQUEST:
        RESUME_NORMAL(arg);
        break;
    case MSG_TYPE_RESPONSE: {
        co_sess_ctx cosess;
        if (ERR_OK == _map_cosess_get(&arg->task->mapco, arg->msg.session, &cosess)) {
            ASSERTAB(MCO_SUCCESS == mco_push(cosess.co, &arg->msg, sizeof(arg->msg)), "mco_push failed!");
            RESUME(arg->task, cosess.co);
        } else {
            LOG_WARN("can't find session %"PRIu64, arg->msg.session);
        }
        _message_clean(&arg->msg);
        break;
    }
    default:
        break;
    }
}
static inline void _task_run(task_ctx *task) {
    message_ctx *tmp;
    co_arg_ctx arg;
    arg.task = task;
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
        arg.msg = *tmp;
        mutex_unlock(&task->mutask);
        _task_onmsg(&arg);
    }
}
static inline void _task_free(task_ctx *task) {
    if (NULL != task->_free) {
        task->_free(task);
    }
    message_ctx *msg;
    while (NULL != (msg = qu_message_pop(&task->qumsg))) {
        _message_clean(msg);
    }
    qu_message_free(&task->qumsg);
    mco_coro **co;
    while (NULL != (co = qu_copool_pop(&task->qucopool))) {
        mco_destroy(*co);
    }
    qu_copool_free(&task->qucopool);
    _map_co_free(&task->mapco);
    mutex_free(&task->mutask);
    FREE(task);
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
    _map_co_init(&task->mapco);
    qu_message_init(&task->qumsg, QUMSG_INITLENS);
    qu_copool_init(&task->qucopool, 0);
    if (NULL != _init) {
        task->handle = _init(task, arg);
    }
    int32_t started = 0;
    rwlock_wrlock(&ctx->lckmaptask);
    if (NULL != hashmap_get(ctx->maptask, &task)) {
        rwlock_unlock(&ctx->lckmaptask);
        _task_free(task);
        LOG_ERROR("task %d repeat.", name);
        return NULL;
    }
    hashmap_set(ctx->maptask, &task);
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
    return ++(task->session);
}
static inline void _srey_timeout(ud_cxt *ud) {
    message_ctx msg;
    msg.msgtype = MSG_TYPE_TIMEOUT;
    msg.session = ud->session;
    _push_message(ud->data, &msg);
}
static inline void _task_timeout(task_ctx *task, uint64_t session, uint32_t timeout, uint32_t type, SOCKET fd) {
    ud_cxt ud;
    ud.data = task;
    ud.session = session;
    co_tmo_ctx cotmo;
    cotmo.type = type;
    cotmo.fd = fd;
    cotmo.co.co = task->curco;
    cotmo.co.session = session;
    _map_cotmo_add(&task->mapco, &cotmo);
    tw_add(&task->srey->tw, timeout, _srey_timeout, &ud);
    if (TMO_TYPE_SLEEP == type) {
        YIELD(task);
    }
}
void task_sleep(task_ctx *task, uint32_t timeout) {
    _task_timeout(task, task_session(task), timeout, TMO_TYPE_SLEEP, INVALID_SOCK);
}
void task_timeout(task_ctx *task, uint64_t session, uint32_t timeout) {
    _task_timeout(task, session, timeout, TMO_TYPE_NORMAL, INVALID_SOCK);
}
static inline void *_task_request(task_ctx *dst, task_ctx *src, void *data, size_t size, int32_t copy, size_t *lens) {
    message_ctx msg;
    msg.msgtype = MSG_TYPE_REQUEST;
    if (NULL == src) {
        msg.session = 0;
        msg.src = -1;
    } else {
        msg.session = task_session(src);
        msg.src = src->name;
    }
    if (0 != copy) {
        MALLOC(msg.data, size);
        memcpy(msg.data, data, size);
    } else {
        msg.data = data;
    }
    msg.size = size;
    if (NULL != src) {
        _map_cosess_add(&src->mapco, src->curco, msg.session);
    }
    _push_message(dst, &msg);
    if (NULL != src) {
        YIELD(src);
        message_ctx resp;
        ASSERTAB(MCO_SUCCESS == mco_pop(src->curco, &resp, sizeof(resp)), "mco_pop failed!");
        *lens = resp.size;
        return resp.data;
    }
    return NULL;
}
void task_call(task_ctx *dst, void *data, size_t size, int32_t copy) {
    _task_request(dst, NULL, data, size, copy, NULL);
}
void *task_request(task_ctx *dst, task_ctx *src, void *data, size_t size, int32_t copy, size_t *lens) {
    return _task_request(dst, src, data, size, copy, lens);
}
void task_response(task_ctx *dst, uint64_t sess, void *data, size_t size, int32_t copy) {
    message_ctx msg;
    msg.msgtype = MSG_TYPE_RESPONSE;
    msg.session = sess;
    if (0 != copy) {
        MALLOC(msg.data, size);
        memcpy(msg.data, data, size);
    } else {
        msg.data = data;
    }
    msg.size = size;
    _push_message(dst, &msg);
}
static inline void _push_handshaked(SOCKET fd, ud_cxt *ud) {
    message_ctx msg;
    msg.msgtype = MSG_TYPE_HANDSHAKED;
    msg.pktype = ud->pktype;
    msg.fd = fd;
    msg.skid = ud->skid;
    _push_message(ud->data, &msg);
}
static inline int32_t _task_net_accept(ev_ctx *ev, SOCKET fd, ud_cxt *ud) {
    message_ctx msg;
    msg.msgtype = MSG_TYPE_ACCEPT;
    msg.pktype = ud->pktype;
    msg.fd = fd;
    msg.skid = ud->skid;
    protos_handshaked(ud, _push_handshaked);
    _push_message(ud->data, &msg);
    return ERR_OK;
}
static inline void _task_net_recv(ev_ctx *ev, SOCKET fd, buffer_ctx *buf, size_t size, ud_cxt *ud) {
    message_ctx msg;
    msg.msgtype = MSG_TYPE_RECV;
    msg.pktype = ud->pktype;
    msg.fd = fd;
    msg.skid = ud->skid;
    void *data;
    size_t lens = 0;
    int32_t closefd = 0;
    do {
        data = protos_unpack(ev, fd, buf, &lens, ud, &closefd);
        if (NULL != data) {
            msg.data = data;
            msg.size = lens;
            if (0 != ud->synflag) {
                ud->synflag = 0;
                msg.synflag = 1;
            } else {
                msg.synflag = 0;
            }
            _push_message(ud->data, &msg);
        }
    } while (NULL != data && 0 != buffer_size(buf));
    if (0 != closefd) {
        ev_close(ev, fd, ud->skid);
    }
}
static inline void _task_net_send(ev_ctx *ev, SOCKET fd, size_t size, ud_cxt *ud) {
    message_ctx msg;
    msg.msgtype = MSG_TYPE_SEND;
    msg.pktype = ud->pktype;
    msg.fd = fd;
    msg.skid = ud->skid;
    msg.size = size;
    _push_message(ud->data, &msg);
}
static inline void _task_net_close(ev_ctx *ev, SOCKET fd, ud_cxt *ud) {
    message_ctx msg;
    msg.msgtype = MSG_TYPE_CLOSE;
    msg.pktype = ud->pktype;
    msg.fd = fd;
    msg.skid = ud->skid;
    if (0 != ud->synflag) {
        ud->synflag = 0;
        msg.synflag = 1;
    } else {
        msg.synflag = 0;
    }
    _push_message(ud->data, &msg);
}
int32_t task_netlisten(task_ctx *task, pack_type pktype, struct evssl_ctx *evssl,
    const char *host, uint16_t port, int32_t sendev) {
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.pktype = pktype;
    ud.data = task;
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
    message_ctx msg;
    msg.msgtype = MSG_TYPE_CONNECT;
    msg.pktype = ud->pktype;
    msg.session = ud->session;
    msg.fd = fd;
    msg.skid = ud->skid;
    msg.erro = (int8_t)err;
    if (0 != ud->synflag) {
        ud->synflag = 0;
        msg.synflag = 1;
    } else {
        msg.synflag = 0;
    }
    protos_handshaked(ud, _push_handshaked);
    _push_message(ud->data, &msg);
    return ERR_OK;
}
SOCKET task_netconnect(task_ctx *task, pack_type pktype, struct evssl_ctx *evssl,
    const char *host, uint16_t port, int32_t sendev, uint64_t *skid) {
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.synflag = 1;
    ud.pktype = pktype;
    ud.data = task;
    ud.session = task_session(task);
    cbs_ctx cbs;
    ZERO(&cbs, sizeof(cbs));
    cbs.conn_cb = _task_net_connect;
    cbs.r_cb = _task_net_recv;
    cbs.c_cb = _task_net_close;
    if (0 != sendev) {
        cbs.s_cb = _task_net_send;
    }
    cbs.ud_free = protos_udfree;
    SOCKET fd = ev_connect(&task->srey->netev, evssl, host, port, &cbs, &ud, skid);
    if (INVALID_SOCK == fd) {
        return INVALID_SOCK;
    }
    _task_timeout(task, ud.session, CONNECT_TIMEOUT, TMO_TYPE_CONNECT, fd);
    _map_cosess_add(&task->mapco, task->curco, ud.session);
    YIELD(task);
    message_ctx msg;
    ASSERTAB(MCO_SUCCESS == mco_pop(task->curco, &msg, sizeof(msg)), "mco_pop failed!");
    if (ERR_OK != msg.erro) {
        return INVALID_SOCK;
    }
    ASSERTAB(*skid == msg.skid, "different socket id.");
    return fd;
}
static inline void _task_net_recvfrom(ev_ctx *ev, SOCKET fd, char *buf, size_t size, netaddr_ctx *addr, ud_cxt *ud) {
    message_ctx msg;
    msg.msgtype = MSG_TYPE_RECVFROM;
    msg.fd = fd;
    msg.skid = ud->skid;
    udp_msg_ctx *umsg;
    MALLOC(umsg, sizeof(udp_msg_ctx) + size);
    memcpy(&umsg->addr, addr, sizeof(netaddr_ctx));
    memcpy(umsg->data, buf, size);
    msg.data = umsg;
    msg.size = size;
    if (0 != ud->synflag) {
        ud->synflag = 0;
        msg.synflag = 1;
    } else {
        msg.synflag = 0;
    }
    _push_message(ud->data, &msg);
}
SOCKET task_netudp(task_ctx *task, const char *host, uint16_t port, uint64_t *skid) {
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.data = task;
    cbs_ctx cbs;
    ZERO(&cbs, sizeof(cbs));
    cbs.rf_cb = _task_net_recvfrom;
    cbs.ud_free = protos_udfree;
    return ev_udp(&task->srey->netev, host, port, &cbs, &ud, skid);
}
void *task_synsendto(task_ctx *task, SOCKET fd, uint64_t skid,
    const char *host, const uint16_t port, void *data, size_t len, size_t *size) {
    uint64_t sess = task_session(task);
    _task_timeout(task, sess, NETRD_TIMEOUT, TMO_TYPE_SYNSEND, fd);
    _map_cosk_add(&task->mapco, sess, task->curco, fd);
    ev_sendto(&task->srey->netev, fd, skid, host, port, data, len, 1);
    YIELD(task);
    message_ctx msg;
    ASSERTAB(MCO_SUCCESS == mco_pop(task->curco, &msg, sizeof(msg)), "mco_pop failed!");
    if (ERR_OK != msg.erro) {
        return NULL;
    }
    ASSERTAB(skid == msg.skid, "different socket id.");
    *size = msg.size;
    return msg.data;
}
void *task_synsend(task_ctx *task, SOCKET fd, uint64_t skid,
    void *data, size_t len, size_t *size, pack_type pktype) {
    uint64_t sess = task_session(task);
    _task_timeout(task, sess, NETRD_TIMEOUT, TMO_TYPE_SYNSEND, fd);
    _map_cosk_add(&task->mapco, sess, task->curco, fd);
    task_netsend(&task->srey->netev, fd, skid, data, len, 1, pktype);
    YIELD(task);
    message_ctx msg;
    ASSERTAB(MCO_SUCCESS == mco_pop(task->curco, &msg, sizeof(msg)), "mco_pop failed!");
    if (ERR_OK != msg.erro) {
        return NULL;
    }
    ASSERTAB(skid == msg.skid, "different socket id.");
    *size = msg.size;
    return msg.data;
}
void task_netsend(ev_ctx *ev, SOCKET fd, uint64_t skid,
    void *data, size_t len, uint8_t synflag, pack_type pktype) {
    size_t size;
    void *pack = protos_pack(pktype, data, len, &size);
    ev_send(ev, fd, skid, pack, size, synflag, 0);
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
        _task_run(task);
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
static inline uint64_t _maptask_hash(const void *item, uint64_t seed0, uint64_t seed1) {
    return hash((const char *)&((*(task_ctx **)item)->name), sizeof((*(task_ctx **)item)->name));
}
static inline int _maptask_compare(const void *a, const void *b, void *ud) {
    return (*(task_ctx **)a)->name - (*(task_ctx **)b)->name;
}
static void _maptask_free(void *item) {
    _task_free(*(task_ctx **)item);
}
srey_ctx *srey_init(uint32_t nnet, uint32_t nworker) {
    srey_ctx *ctx;
    CALLOC(ctx, 1, sizeof(srey_ctx));
    ctx->nworker = nworker;
    MALLOC(ctx->thworker, sizeof(pthread_t) * ctx->nworker);
    ctx->codesc = mco_desc_init(_co_cb, 0);
    mutex_init(&ctx->muworker);
    cond_init(&ctx->condworker);
    qu_void_init(&ctx->quglobal, ONEK);
    rwlock_init(&ctx->lckmaptask);
    ctx->maptask = hashmap_new_with_allocator(_malloc, _realloc, _free,
                                              sizeof(task_ctx *), ONEK, 0, 0,
                                              _maptask_hash, _maptask_compare, _maptask_free, NULL);
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
static inline bool _push_closing(const void *item, void *udata) {
    _push_message(*(task_ctx **)item, udata);
    return true;
}
static inline bool _check_closing(const void *item, void *udata) {
    if (0 == (*(task_ctx **)item)->closed) {
        *((int32_t *)udata) = 1;
        return false;
    }
    return true;
}
static void _task_closing(srey_ctx *ctx) {
    message_ctx closing;
    closing.msgtype = MSG_TYPE_CLOSING;
    rwlock_rdlock(&ctx->lckmaptask);
    hashmap_scan(ctx->maptask, _push_closing, &closing);
    rwlock_unlock(&ctx->lckmaptask);
    int32_t closed;
    do {
        closed = 0;
        rwlock_rdlock(&ctx->lckmaptask);
        hashmap_scan(ctx->maptask, _check_closing, &closed);
        rwlock_unlock(&ctx->lckmaptask);
        if (0 != closed) {
            MSLEEP(50);
        }
    } while (0 != closed);
}
void srey_free(srey_ctx *ctx) {
    _task_closing(ctx);
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
