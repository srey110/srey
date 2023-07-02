#include "service/srey.h"
#include "service/maps.h"
#include "service/minicoro.h"
#include "sarray.h"
#include "hashmap.h"
#include "tw.h"
#include "cond.h"
#include "rwlock.h"
#include "queue.h"

typedef enum timeout_type {
    TMO_TYPE_SLEEP = 0x01,
    TMO_TYPE_NORMAL,
    TMO_TYPE_NET
}timeout_type;
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
    mutex_ctx mutask;
    qu_message qumsg;
    qu_copool qucopool;
};
typedef struct co_arg_ctx {
    task_ctx *task;
    message_ctx msg;
}co_arg_ctx;

#define CONNECT_TIMEOUT       3000
#define NETRD_TIMEOUT         3000
#define MSG_INIT_CAP          512
#define COPOOL_INIT_CAP       128
#define CO_CREATE(arg)\
    arg->task->curco = _co_create(arg->task);\
    ASSERTAB(MCO_SUCCESS == mco_push(arg->task->curco, arg, sizeof(co_arg_ctx)), "mco_push failed!");\
    ASSERTAB(MCO_SUCCESS == mco_resume(arg->task->curco), "resume coroutine failed!")
#define YIELD(task) ASSERTAB(MCO_SUCCESS == mco_yield(task->curco), "yield coroutine failed!")
#define RESUME(arg, co) \
    arg->task->curco = co;\
    ASSERTAB(MCO_SUCCESS == mco_push(co, &arg->msg, sizeof(message_ctx)), "mco_push failed!");\
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
    if (ERR_OK != _map_cotmo_get(&arg->task->mapco, arg->msg.sess, &cotmo)) {
        return;
    }
    switch (cotmo.type) {
    case TMO_TYPE_SLEEP:
        RESUME(arg, cotmo.co);
        break;
    case TMO_TYPE_NORMAL:
        CO_CREATE(arg);
        break;
    case TMO_TYPE_NET: {
        co_sess_ctx cosess;
        if (ERR_OK == _map_cosess_get(&arg->task->mapco, arg->msg.sess, &cosess, 1)) {
            RESUME(arg, cosess.co);
        }
        break;
    }
    default:
        break;
    }
}
static inline void _dispatch_connect(co_arg_ctx *arg) {
    if (0 != arg->msg.sess) {
        co_sess_ctx cosess;
        if (ERR_OK == _map_cosess_get(&arg->task->mapco, arg->msg.sess, &cosess, 1)) {
            _map_cotmo_del(&arg->task->mapco, arg->msg.sess);
            RESUME(arg, cosess.co);
        } else {
            if (ERR_OK == arg->msg.erro) {
                ev_close(task_netev(arg->task), arg->msg.fd, arg->msg.skid);
            }
        }
    } else {
        CO_CREATE(arg);
    }
}
static inline void _dispatch_netrd(co_arg_ctx *arg) {
    if (0 != arg->msg.sess) {
        int32_t del;
        if (SLICE == arg->msg.slice
            || SLICE_START == arg->msg.slice) {
            del = 0;
        } else {
            del = 1;
        }
        co_sess_ctx cosess;
        if (ERR_OK == _map_cosess_get(&arg->task->mapco, arg->msg.sess, &cosess, del)) {
            if (SLICE_NONE == arg->msg.slice
                || SLICE_START == arg->msg.slice) {
                _map_cotmo_del(&arg->task->mapco, arg->msg.sess);
            }
            RESUME(arg, cosess.co);
        }
        _message_clean(&arg->msg);
    } else {
        CO_CREATE(arg);
    }
}
static inline void _dispatch_close(co_arg_ctx *arg) {
    if (0 != arg->msg.sess) {
        co_sess_ctx cosess;
        if (ERR_OK == _map_cosess_get(&arg->task->mapco, arg->msg.sess, &cosess, 1)) {
            _map_cotmo_del(&arg->task->mapco, arg->msg.sess);
            RESUME(arg, cosess.co);
        }
    }
    CO_CREATE(arg);
}
static inline void _dispatch_response(co_arg_ctx *arg) {
    co_sess_ctx cosess;
    if (ERR_OK == _map_cosess_get(&arg->task->mapco, arg->msg.sess, &cosess, 1)) {
        RESUME(arg, cosess.co);
    } else {
        LOG_ERROR("task: %d, can't find session:%"PRIu64, arg->task->name, arg->msg.sess);
    }
    _message_clean(&arg->msg);
}
static inline void _task_on_message(co_arg_ctx *arg) {
    if (0 != arg->task->closed) {
        _message_clean(&arg->msg);
        return;
    }
    switch (arg->msg.msgtype) {
    case MSG_TYPE_STARTED:
        CO_CREATE(arg);
        break;
    case MSG_TYPE_CLOSING:
        CO_CREATE(arg);
        break;
    case MSG_TYPE_TIMEOUT:
        _dispatch_timeout(arg);
        break;
    case MSG_TYPE_ACCEPT:
        CO_CREATE(arg);
        break;
    case MSG_TYPE_CONNECT:
        _dispatch_connect(arg);
        break;
    case MSG_TYPE_HANDSHAKED:
        CO_CREATE(arg);
        break;
    case MSG_TYPE_RECV:
        _dispatch_netrd(arg);
        break;
    case MSG_TYPE_SEND:
        CO_CREATE(arg);
        break;
    case MSG_TYPE_CLOSE:
        _dispatch_close(arg);
        break;
    case MSG_TYPE_RECVFROM:
        _dispatch_netrd(arg);
        break;
    case MSG_TYPE_REQUEST:
        CO_CREATE(arg);
        break;
    case MSG_TYPE_RESPONSE:
        _dispatch_response(arg);
        break;
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
        _task_on_message(&arg);
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
    if (NULL == _run) {
        LOG_WARN("task %d, %s", name, ERRSTR_INVPARAM);
        return NULL;
    }
    task_ctx *task;
    CALLOC(task, 1, sizeof(task_ctx));
    task->name = name;
    task->maxcnt = maxcnt;
    task->dmaxcnt = maxcnt * 2;
    task->_run = _run;
    task->_free = _tfree;
    task->srey = ctx;
    mutex_init(&task->mutask);
    _map_co_init(&task->mapco);
    qu_message_init(&task->qumsg, MSG_INIT_CAP);
    qu_copool_init(&task->qucopool, COPOOL_INIT_CAP);
    if (NULL != _init) {
        task->handle = _init(task, arg);
        if (NULL == task->handle) {
            _task_free(task);
            return NULL;
        }
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
int32_t certs_register(srey_ctx *ctx, const char *name, struct evssl_ctx *evssl) {
    certs_ctx cert;
    ASSERTAB(strlen(name) < sizeof(cert.name), "cert name too long.");
    ZERO(&cert, sizeof(certs_ctx));
    strcpy(cert.name, name);
    cert.ssl = evssl;
    int32_t rtn;
    rwlock_wrlock(&ctx->lckarrcert);
    if (NULL != _certs_get(ctx, name)) {
        LOG_ERROR("cert name %s repeat.", name);
        rtn = ERR_FAILED;
    } else {
        arr_certs_push_back(&ctx->arrcert, &cert);
        rtn = ERR_OK;
    }
    rwlock_unlock(&ctx->lckarrcert);
    return rtn;
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
void _push_handshaked(SOCKET fd, uint64_t skid, ud_cxt *ud) {
    message_ctx msg;
    msg.msgtype = MSG_TYPE_HANDSHAKED;
    msg.pktype = ud->pktype;
    msg.fd = fd;
    msg.skid = skid;
    _push_message(ud->data, &msg);
}
static inline void _srey_timeout(ud_cxt *ud) {
    message_ctx msg;
    msg.msgtype = MSG_TYPE_TIMEOUT;
    msg.sess = ud->sess;
    _push_message(ud->data, &msg);
}
static inline void _task_timeout(task_ctx *task, uint64_t sess, uint32_t ms, uint32_t type) {
    ud_cxt ud;
    ud.data = task;
    ud.sess = sess;
    _map_cotmo_add(&task->mapco, type, task->curco, sess);
    tw_add(&task->srey->tw, ms, _srey_timeout, &ud);
    if (TMO_TYPE_SLEEP == type) {
        YIELD(task);
        message_ctx msg;
        ASSERTAB(MCO_SUCCESS == mco_pop(task->curco, &msg, sizeof(msg)), "mco_pop failed!");
        if (sess != msg.sess) {
            LOG_ERROR("task: %d, request session: %"PRIu64", response session: %"PRIu64" not the same.", task->name, sess, msg.sess);
            return;
        }
        if (MSG_TYPE_TIMEOUT != msg.msgtype) {
            LOG_ERROR("task: %d, message type: %d error, session: %"PRIu64, task->name, msg.msgtype, sess);
        }
    }
}
void task_sleep(task_ctx *task, uint32_t ms) {
    _task_timeout(task, createid(), ms, TMO_TYPE_SLEEP);
}
void task_timeout(task_ctx *task, uint64_t sess, uint32_t ms) {
    _task_timeout(task, sess, ms, TMO_TYPE_NORMAL);
}
static inline void *_task_request(task_ctx *dst, task_ctx *src, void *data, size_t size, int32_t copy, size_t *lens) {
    message_ctx msg;
    msg.msgtype = MSG_TYPE_REQUEST;
    if (NULL == src) {
        msg.sess = 0;
        msg.src = -1;
    } else {
        msg.sess = createid();
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
        _map_cosess_add(&src->mapco, src->curco, msg.sess);
    }
    _push_message(dst, &msg);
    if (NULL != src) {
        YIELD(src);
        message_ctx resp;
        ASSERTAB(MCO_SUCCESS == mco_pop(src->curco, &resp, sizeof(resp)), "mco_pop failed!");
        if (msg.sess != resp.sess) {
            LOG_ERROR("task: %d, request session: %"PRIu64", response session: %"PRIu64" not the same.", src->name, msg.sess, resp.sess);
            return NULL;
        }
        if (MSG_TYPE_RESPONSE != resp.msgtype) {
            LOG_ERROR("task: %d, message type: %d error, session: %"PRIu64, src->name, resp.msgtype, msg.sess);
            return NULL;
        }
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
    msg.sess = sess;
    if (0 != copy) {
        MALLOC(msg.data, size);
        memcpy(msg.data, data, size);
    } else {
        msg.data = data;
    }
    msg.size = size;
    _push_message(dst, &msg);
}
void *task_slice(task_ctx *task, uint64_t sess, size_t *size, int32_t *end) {
    YIELD(task);
    message_ctx msg;
    ASSERTAB(MCO_SUCCESS == mco_pop(task->curco, &msg, sizeof(msg)), "mco_pop failed!");
    if (sess != msg.sess) {
        LOG_ERROR("task: %d, request session: %"PRIu64", response session: %"PRIu64" not the same.", task->name, sess, msg.sess);
        return NULL;
    }
    if (MSG_TYPE_CLOSE == msg.msgtype) {
        LOG_WARN("task: %d, connection closed. session:%"PRIu64, task->name, sess);
        return NULL;
    }
    if (MSG_TYPE_RECV != msg.msgtype) {
        LOG_ERROR("task: %d, message type: %d error, session: %"PRIu64, task->name, msg.msgtype, sess);
        return NULL;
    }
    if (SLICE_END == msg.slice) {
        *end = 1;
    }
    *size = msg.size;
    return msg.data;
}
static inline int32_t _task_net_accept(ev_ctx *ev, SOCKET fd, uint64_t skid, ud_cxt *ud) {
    message_ctx msg;
    msg.msgtype = MSG_TYPE_ACCEPT;
    msg.pktype = ud->pktype;
    msg.fd = fd;
    msg.skid = skid;
    _push_message(ud->data, &msg);
    return ERR_OK;
}
static inline void _set_sess_slice(message_ctx *msg, ud_cxt *ud, int32_t slice) {
    if (0 != ud->sess
        && SLICE_NONE == ud->slice) {
        msg->sess = ud->sess;
        if (SLICE_START == slice) {
            ud->slice = SLICE;
            msg->slice = SLICE_START;
        } else {
            ud->sess = 0;
            msg->slice = SLICE_NONE;
        }
    } else if (SLICE == ud->slice) {
        if (SLICE_NONE == slice) {
            msg->slice = SLICE_NONE;
            msg->sess = 0;
        } else if (SLICE_END == slice) {
            msg->slice = SLICE_END;
            msg->sess = ud->sess;
            ud->sess = 0;
            ud->slice = SLICE_NONE;
        } else {
            msg->slice = SLICE;
            msg->sess = ud->sess;
        }
    } else {
        msg->slice = SLICE_NONE;
        msg->sess = 0;
    }
}
static inline void _task_net_recv(ev_ctx *ev, SOCKET fd, uint64_t skid, buffer_ctx *buf, size_t size, ud_cxt *ud) {
    message_ctx msg;
    msg.msgtype = MSG_TYPE_RECV;
    msg.pktype = ud->pktype;
    msg.fd = fd;
    msg.skid = skid;
    void *data;
    size_t lens;
    int32_t closefd = 0;
    int32_t slice;
    do {
        data = protos_unpack(ev, fd, skid, buf, &lens, ud, &closefd, &slice);
        if (NULL != data) {
            msg.data = data;
            msg.size = lens;
            _set_sess_slice(&msg, ud, slice);
            _push_message(ud->data, &msg);
        }
    } while (NULL != data && 0 != buffer_size(buf));
    if (0 != closefd) {
        ev_close(ev, fd, skid);
    }
}
static inline void _task_net_send(ev_ctx *ev, SOCKET fd, uint64_t skid, size_t size, ud_cxt *ud) {
    message_ctx msg;
    msg.msgtype = MSG_TYPE_SEND;
    msg.pktype = ud->pktype;
    msg.fd = fd;
    msg.skid = skid;
    msg.size = size;
    _push_message(ud->data, &msg);
}
static inline void _task_net_close(ev_ctx *ev, SOCKET fd, uint64_t skid, ud_cxt *ud) {
    message_ctx msg;
    msg.msgtype = MSG_TYPE_CLOSE;
    msg.pktype = ud->pktype;
    msg.fd = fd;
    msg.skid = skid;
    msg.sess = ud->sess;
    _push_message(ud->data, &msg);
}
int32_t task_netlisten(task_ctx *task, pack_type pktype, struct evssl_ctx *evssl,
    const char *ip, uint16_t port, int32_t sendev) {
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
    return ev_listen(&task->srey->netev, evssl, ip, port, &cbs, &ud);
}
static inline int32_t _task_net_connect(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t err, ud_cxt *ud) {
    message_ctx msg;
    msg.msgtype = MSG_TYPE_CONNECT;
    msg.pktype = ud->pktype;
    msg.skid = skid;
    msg.fd = fd;
    msg.erro = (int8_t)err;
    msg.sess = skid;
    _push_message(ud->data, &msg);
    return ERR_OK;
}
SOCKET task_netconnect(task_ctx *task, pack_type pktype, struct evssl_ctx *evssl,
    const char *ip, uint16_t port, int32_t sendev, uint64_t *skid) {
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.pktype = pktype;
    ud.data = task;
    cbs_ctx cbs;
    ZERO(&cbs, sizeof(cbs));
    cbs.conn_cb = _task_net_connect;
    cbs.r_cb = _task_net_recv;
    cbs.c_cb = _task_net_close;
    if (0 != sendev) {
        cbs.s_cb = _task_net_send;
    }
    cbs.ud_free = protos_udfree;
    SOCKET fd = ev_connect(&task->srey->netev, evssl, ip, port, &cbs, &ud, skid);
    if (INVALID_SOCK == fd) {
        return INVALID_SOCK;
    }
    _map_cosess_add(&task->mapco, task->curco, *skid);
    _task_timeout(task, *skid, CONNECT_TIMEOUT, TMO_TYPE_NET);
    YIELD(task);
    message_ctx msg;
    ASSERTAB(MCO_SUCCESS == mco_pop(task->curco, &msg, sizeof(msg)), "mco_pop failed!");
    if (*skid != msg.sess) {
        LOG_ERROR("task: %d, request session: %"PRIu64", response session: %"PRIu64" not the same.", task->name, *skid, msg.sess);
        return INVALID_SOCK;
    }
    if (MSG_TYPE_TIMEOUT == msg.msgtype) {
        LOG_WARN("task: %d, connect %s:%d timeout.session:%"PRIu64, task->name, ip, port, *skid);
        return INVALID_SOCK;
    }
    if (MSG_TYPE_CONNECT != msg.msgtype) {
        LOG_ERROR("task: %d, message type: %d error, session: %"PRIu64, task->name, msg.msgtype, *skid);
        return INVALID_SOCK;
    }
    if (ERR_OK != msg.erro) {
        return INVALID_SOCK;
    }
    return fd;
}
static inline void _task_net_recvfrom(ev_ctx *ev, SOCKET fd, uint64_t skid, char *buf, size_t size, netaddr_ctx *addr, ud_cxt *ud) {
    message_ctx msg;
    msg.msgtype = MSG_TYPE_RECVFROM;
    msg.fd = fd;
    msg.skid = skid;
    udp_msg_ctx *umsg;
    MALLOC(umsg, sizeof(udp_msg_ctx) + size);
    memcpy(&umsg->addr, addr, sizeof(netaddr_ctx));
    memcpy(umsg->data, buf, size);
    msg.data = umsg;
    msg.size = size;
    msg.sess = ud->sess;
    msg.slice = SLICE_NONE;
    ud->sess = 0;
    _push_message(ud->data, &msg);
}
SOCKET task_netudp(task_ctx *task, const char *ip, uint16_t port, uint64_t *skid) {
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.data = task;
    cbs_ctx cbs;
    ZERO(&cbs, sizeof(cbs));
    cbs.rf_cb = _task_net_recvfrom;
    cbs.ud_free = protos_udfree;
    return ev_udp(&task->srey->netev, ip, port, &cbs, &ud, skid);
}
void task_netsend(task_ctx *task, SOCKET fd, uint64_t skid,
    void *data, size_t len, pack_type pktype) {
    size_t size;
    void *pack = protos_pack(pktype, data, len, &size);
    ev_send(&task->srey->netev, fd, skid, pack, size, 0);
}
void *task_synsend(task_ctx *task, SOCKET fd, uint64_t skid,
    void *data, size_t len, size_t *size, pack_type pktype, uint64_t *sess) {
    *sess = createid();
    _map_cosess_add(&task->mapco, task->curco, *sess);
    _task_timeout(task, *sess, NETRD_TIMEOUT, TMO_TYPE_NET);
    ev_ud_sess(&task->srey->netev, fd, skid, *sess);
    task_netsend(task, fd, skid, data, len, pktype);
    YIELD(task);
    message_ctx msg;
    ASSERTAB(MCO_SUCCESS == mco_pop(task->curco, &msg, sizeof(msg)), "mco_pop failed!");
    if (*sess != msg.sess) {
        LOG_ERROR("task: %d, request session: %"PRIu64", response session: %"PRIu64" not the same.", task->name, *sess, msg.sess);
        return NULL;
    }
    if (MSG_TYPE_TIMEOUT == msg.msgtype) {
        LOG_WARN("task: %d, synsend timeout.session:%"PRIu64, task->name, *sess);
        return NULL;
    }
    if (MSG_TYPE_CLOSE == msg.msgtype) {
        LOG_WARN("task: %d, connection closed. session:%"PRIu64, task->name, *sess);
        return NULL;
    }
    if (MSG_TYPE_RECV != msg.msgtype) {
        LOG_ERROR("task: %d, message type: %d error, session: %"PRIu64, task->name, msg.msgtype, *sess);
        return NULL;
    }
    *size = msg.size;
    return msg.data;
}
void *task_synsendto(task_ctx *task, SOCKET fd, uint64_t skid,
    const char *ip, const uint16_t port, void *data, size_t len, size_t *size) {
    uint64_t sess = createid();
    _map_cosess_add(&task->mapco, task->curco, sess);
    _task_timeout(task, sess, NETRD_TIMEOUT, TMO_TYPE_NET);
    ev_ud_sess(&task->srey->netev, fd, skid, sess);
    if (ERR_OK != ev_sendto(&task->srey->netev, fd, skid, ip, port, data, len)) {
        ev_ud_sess(&task->srey->netev, fd, skid, 0);
        _map_cosess_del(&task->mapco, sess);
        _map_cotmo_del(&task->mapco, sess);
        return NULL;
    }
    YIELD(task);
    message_ctx msg;
    ASSERTAB(MCO_SUCCESS == mco_pop(task->curco, &msg, sizeof(msg)), "mco_pop failed!");
    if (sess != msg.sess) {
        LOG_ERROR("task: %d, request session: %"PRIu64", response session: %"PRIu64" not the same.", task->name, sess, msg.sess);
        return NULL;
    }
    if (MSG_TYPE_TIMEOUT == msg.msgtype) {
        LOG_WARN("task: %d, synsend timeout.session:%"PRIu64, task->name, sess);
        return NULL;
    }
    if (MSG_TYPE_RECVFROM != msg.msgtype) {
        LOG_ERROR("task: %d, message type: %d error, session: %"PRIu64, task->name, msg.msgtype, sess);
        return NULL;
    }
    udp_msg_ctx *umsg = msg.data;
    *size = msg.size;
    return umsg->data;
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
