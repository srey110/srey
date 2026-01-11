#include "srey/coro.h"
#include "containers/hashmap.h"
#include "utils/timer.h"
#define MINICORO_IMPL
#include "srey/minicoro.h"

typedef struct coro_info {
    msg_type mtype;
    mco_coro *co;
    uint64_t timeout;
}coro_info;
QUEUE_DECL(coro_info, qu_coinfo)
typedef struct coro_sess {
    int32_t disposable;//一次性
    qu_coinfo_ctx *qucoinfo;
    coro_info coinfo;//disposable true时用
    uint64_t sess;
}coro_sess;
typedef struct coro_ctx {//task->arg
    int32_t nyield;
    mco_coro *curco;
    struct hashmap *mapco;
    qu_ptr_ctx qucopool;
    timer_ctx timer;
}coro_ctx;

static mco_desc _coro_desc;

static uint64_t _map_cosess_hash(const void *item, uint64_t seed0, uint64_t seed1) {
    return hash((const char *)&(((coro_sess *)item)->sess), sizeof(((coro_sess *)item)->sess));
}
static int _map_cosess_compare(const void *a, const void *b, void *ud) {
    return (int)(((coro_sess *)a)->sess - ((coro_sess *)b)->sess);
}
static void _map_cosess_set(task_ctx *task, int32_t disposable, mco_coro *coro, uint64_t sess, msg_type mtype, uint32_t ms) {
    coro_ctx *coctx = task->arg;
    if (disposable) {
        coro_sess cosess;
        cosess.disposable = disposable;
        cosess.sess = sess;
        if (ms > 0) {
            cosess.coinfo.timeout = timer_cur_ms(&coctx->timer) + ms;
        } else {
            cosess.coinfo.timeout = 0;
        }
        cosess.coinfo.co = coro;
        cosess.coinfo.mtype = mtype;
        cosess.qucoinfo = NULL;
        ASSERTAB(NULL == hashmap_set(coctx->mapco, &cosess), "repeat session");
        return;
    }
    coro_info coinfo;
    if (ms > 0) {
        coinfo.timeout = timer_cur_ms(&coctx->timer) + ms;
    } else {
        coinfo.timeout = 0;
    }
    coinfo.co = coro;
    coinfo.mtype = mtype;
    coro_sess key;
    key.sess = sess;
    const coro_sess *cofind = hashmap_get(coctx->mapco, &key);
    if (NULL != cofind) {
        qu_coinfo_push(cofind->qucoinfo, &coinfo);
    } else {
        coro_sess cosess;
        cosess.disposable = disposable;
        cosess.sess = sess;
        MALLOC(cosess.qucoinfo, sizeof(qu_coinfo_ctx));
        qu_coinfo_init(cosess.qucoinfo, 0);
        qu_coinfo_push(cosess.qucoinfo, &coinfo);
        hashmap_set(coctx->mapco, &cosess);
    }
}
static int32_t _map_cosess_get(task_ctx *task, uint64_t sess, msg_type mtype, coro_sess *cosess) {
    coro_ctx *coctx = task->arg;
    coro_sess key;
    key.sess = sess;
    coro_sess *cofind = (coro_sess *)hashmap_get(coctx->mapco, &key);
    if (NULL == cofind) {
        return ERR_FAILED;
    }
    if (cofind->disposable) {
        if (mtype != cofind->coinfo.mtype 
            && MSG_TYPE_CLOSE != mtype) {
            return ERR_FAILED;
        }
        *cosess = *cofind;
        hashmap_delete(coctx->mapco, &key);
        return ERR_OK;
    }
    coro_info *coinfo = qu_coinfo_peek(cofind->qucoinfo);
    if (NULL == coinfo) {
        return ERR_FAILED;
    }
    if (mtype != coinfo->mtype
        && MSG_TYPE_CLOSE != mtype) {
        return ERR_FAILED;
    }
    *cosess = *cofind;
    if (MSG_TYPE_CLOSE == mtype) {
        hashmap_delete(coctx->mapco, &key);
    }
    return ERR_OK;
}
static inline mco_coro *_get_mco(coro_sess *cosess) {
    if (cosess->disposable) {
        return cosess->coinfo.co;
    }
    coro_info *coinfo = qu_coinfo_pop(cosess->qucoinfo);
    if (NULL == coinfo) {
        return NULL;
    }
    return coinfo->co;
}
static void _mco_cb(mco_coro *coro) {
    mco_result rtn;
    task_dispatch_arg arg;
    for (;;) {
        rtn = mco_yield(coro);
        ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
        rtn = mco_pop(coro, &arg, sizeof(arg));
        ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
        task_incref(arg.task);//保证_message_run里yield后不会被释放
        _message_run(arg.task, &arg.msg);
        qu_ptr_push(&((coro_ctx *)arg.task->arg)->qucopool, (void **)&coro);
        task_ungrab(arg.task);
    }
}
void coro_desc_init(size_t stack_size) {
    _coro_desc = mco_desc_init(_mco_cb, stack_size);
}
static coro_ctx *_coro_ctx_init(void) {
    coro_ctx *coctx;
    CALLOC(coctx, 1, sizeof(coro_ctx));
    qu_ptr_init(&coctx->qucopool, 0);
    timer_init(&coctx->timer);
    coctx->mapco = hashmap_new_with_allocator(_malloc, _realloc, _free,
                                              sizeof(coro_sess), ONEK, 0, 0,
                                              _map_cosess_hash, _map_cosess_compare, NULL, NULL);
    return coctx;
}
static void _coro_ctx_free(void *arg) {
    coro_ctx *coctx = arg;
    mco_result rtn;
    mco_coro **coro;
    while (NULL != (coro = (mco_coro **)qu_ptr_pop(&coctx->qucopool))) {
        rtn = mco_destroy(*coro);
        if (MCO_SUCCESS != rtn) {
            LOG_WARN("%s", mco_result_description(rtn));
        }
    }
    qu_ptr_free(&coctx->qucopool);
    size_t iter = 0;
    coro_sess *corosess;
    while (hashmap_iter(coctx->mapco, &iter, (void **)&corosess)) {
        if (NULL != corosess->qucoinfo) {
            qu_coinfo_free(corosess->qucoinfo);
            FREE(corosess->qucoinfo);
        }
    }
    hashmap_free(coctx->mapco);
    FREE(coctx);
}
static mco_coro *_coro_pool_get(task_ctx *task) {
    coro_ctx *coctx = task->arg;
    mco_coro **coro = (mco_coro **)qu_ptr_pop(&coctx->qucopool);
    if (NULL != coro) {
        return *coro;
    }
    mco_coro *coronew;
    mco_result rtn = mco_create(&coronew, &_coro_desc);
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
    rtn = mco_resume(coronew);
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
    return coronew;
}
static void _co_create(task_dispatch_arg *arg) {
    coro_ctx *coctx = arg->task->arg;
    coctx->curco = _coro_pool_get(arg->task);
    mco_result rtn = mco_push(coctx->curco, arg, sizeof(task_dispatch_arg));
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
    rtn = mco_resume(coctx->curco);
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
}
static void _co_resume(mco_coro *coro, task_dispatch_arg *arg) {
    coro_ctx *coctx = arg->task->arg;
    coctx->curco = coro;
    mco_result rtn = mco_push(coro, &arg->msg, sizeof(message_ctx));
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
    rtn = mco_resume(coro);
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
    _message_clean(arg->msg.mtype, arg->msg.pktype, arg->msg.data);
}
static void _timeout_dispatch(task_dispatch_arg *arg) {
    if (0 == arg->msg.sess) {
        _co_create(arg);
        return;
    }
    coro_sess cosess;
    if (ERR_OK != _map_cosess_get(arg->task, arg->msg.sess, (msg_type)arg->msg.mtype, &cosess)) {
        LOG_WARN("task %d message type %d, can't find session %"PRIu64, arg->task->name, arg->msg.mtype, arg->msg.sess);
        return;
    }
    _co_resume(_get_mco(&cosess), arg);
}
static void _connected_dispatch(task_dispatch_arg *arg) {
    coro_sess cosess;
    if (ERR_OK != _map_cosess_get(arg->task, arg->msg.skid, (msg_type)arg->msg.mtype, &cosess)) {
        _co_create(arg);
        return;
    }
    mco_coro *coro = _get_mco(&cosess);
    if (NULL == coro) {
        _co_create(arg);
    } else {
        _co_resume(coro, arg);
    }
}
static void _ssl_exchanged_dispatch(task_dispatch_arg *arg) {
    coro_sess cosess;
    if (ERR_OK != _map_cosess_get(arg->task, arg->msg.skid, (msg_type)arg->msg.mtype, &cosess)) {
        _co_create(arg);
        return;
    }
    mco_coro *coro = _get_mco(&cosess);
    if (NULL == coro) {
        _co_create(arg);
    } else {
        _co_resume(coro, arg);
    }
}
static void _handshaked_dispatch(task_dispatch_arg *arg) {
    coro_sess cosess;
    if (ERR_OK != _map_cosess_get(arg->task, arg->msg.skid, (msg_type)arg->msg.mtype, &cosess)) {
        _co_create(arg);
        return;
    }
    mco_coro *coro = _get_mco(&cosess);
    if (NULL == coro) {
        _co_create(arg);
    } else {
        _co_resume(coro, arg);
    }
}
static void _recved_dispatch(task_dispatch_arg *arg) {
    if (0 == arg->msg.sess
        || ERR_OK != prots_may_resume(arg->msg.pktype, arg->msg.data)) {
        _co_create(arg);
        return;
    }
    coro_sess cosess;
    if (ERR_OK != _map_cosess_get(arg->task, arg->msg.sess, (msg_type)arg->msg.mtype, &cosess)) {
        _co_create(arg);
        return;
    }
    mco_coro *coro = _get_mco(&cosess);
    if (NULL == coro) {
        _co_create(arg);
    } else {
        _co_resume(coro, arg);
    }
}
static void _closed_dispatch(task_dispatch_arg *arg) {
    coro_sess cosess;
    if (ERR_OK == _map_cosess_get(arg->task, arg->msg.skid, (msg_type)arg->msg.mtype, &cosess)) {
        mco_coro *coro;
        while (NULL != (coro = _get_mco(&cosess))) {
            _co_resume(coro, arg);
        }
        qu_coinfo_free(cosess.qucoinfo);
        FREE(cosess.qucoinfo);
    }
    _co_create(arg);
}
static void _recvfrom_dispatch(task_dispatch_arg *arg) {
    if (0 == arg->msg.sess) {
        _co_create(arg);
        return;
    }
    coro_sess cosess;
    if (ERR_OK != _map_cosess_get(arg->task, arg->msg.sess, (msg_type)arg->msg.mtype, &cosess)) {
        _co_create(arg);
    } else {
        _co_resume(_get_mco(&cosess), arg);
    }
}
static void _response_dispatch(task_dispatch_arg *arg) {
    coro_sess cosess;
    if (ERR_OK != _map_cosess_get(arg->task, arg->msg.sess, (msg_type)arg->msg.mtype, &cosess)) {
        _co_create(arg); 
    } else {
        _co_resume(_get_mco(&cosess), arg);
    }
}
static void _timeout_monitor(task_ctx *task, uint64_t sess) {
    coro_ctx *coctx = task->arg;
    if (coctx->nyield > 0) {
        size_t iter = 0;
        mco_coro *coro;
        coro_sess *cosess;
        uint64_t now = timer_cur_ms(&coctx->timer);
        coro_info *coinfo;
        task_dispatch_arg arg;
        arg.task = task;
        arg.msg.mtype = MSG_TYPE_TIMEOUT;
        while (hashmap_iter(coctx->mapco, &iter, (void **)&cosess)) {
            if (cosess->disposable) {
                coinfo = &cosess->coinfo;
            } else {
                coinfo = qu_coinfo_peek(cosess->qucoinfo);
                if (NULL == coinfo) {
                    continue;
                }
            }
            if (coinfo->timeout > 0
                && now >= coinfo->timeout) {
                arg.msg.sess = cosess->sess;
                coro = coinfo->co;
                LOG_INFO("task %d message type %d session %"PRIu64" timeout.", task->name, coinfo->mtype, cosess->sess);
                if (cosess->disposable) {
                    hashmap_delete(coctx->mapco, cosess);
                } else {
                    qu_coinfo_pop(cosess->qucoinfo);
                }
                _co_resume(coro, &arg);
            }
        }
    }
    task_timeout(task, 0, 3 * 1000, _timeout_monitor);
}
void _message_dispatch(task_dispatch_arg *arg) {
    switch (arg->msg.mtype) {
    case MSG_TYPE_STARTUP:
        task_timeout(arg->task, 0, 3 * 1000, _timeout_monitor);
        _co_create(arg);
        break;
    case MSG_TYPE_CLOSING:
        _co_create(arg);
        if (((coro_ctx *)arg->task->arg)->nyield > 0) {
            LOG_WARN("task %d yield %d.", arg->task->name, ((coro_ctx *)arg->task->arg)->nyield);
        }
        break;
    case MSG_TYPE_TIMEOUT:
        _timeout_dispatch(arg);
        break;
    case MSG_TYPE_ACCEPT:
        _co_create(arg);
        break;
    case MSG_TYPE_CONNECT:
        _connected_dispatch(arg);
        break;
    case MSG_TYPE_SSLEXCHANGED:
        _ssl_exchanged_dispatch(arg);
        break;
    case MSG_TYPE_HANDSHAKED:
        _handshaked_dispatch(arg);
        break;
    case MSG_TYPE_RECV:
        _recved_dispatch(arg);
        break;
    case MSG_TYPE_SEND:
        _co_create(arg);
        break;
    case MSG_TYPE_CLOSE:
        _closed_dispatch(arg);
        break;
    case MSG_TYPE_RECVFROM:
        _recvfrom_dispatch(arg);
        break;
    case MSG_TYPE_REQUEST:
        _co_create(arg);
        break;
    case MSG_TYPE_RESPONSE:
        _response_dispatch(arg);
        break;
    default:
        break;
    }
}
task_ctx *coro_task_register(loader_ctx *loader, name_t name, _task_startup_cb _startup, _task_closing_cb _closing) {
    coro_ctx *coctx = _coro_ctx_init();
    task_ctx *task = task_new(loader, name, _message_dispatch, _coro_ctx_free, coctx);
    if (ERR_OK != task_register(task, _startup, _closing)) {
        task_free(task);
        return NULL;
    }
    return task;
}
void coro_sync(task_ctx *task, SOCKET fd, uint64_t skid) {
    ev_ud_sess(&task->loader->netev, fd, skid, skid);
}
static inline void _coro_wait(task_ctx *task, int32_t disposable, uint64_t sess, msg_type mtype, uint32_t ms, message_ctx *msg) {
    coro_ctx *coctx = task->arg;
    _map_cosess_set(task, disposable, coctx->curco, sess, mtype, ms);
    ++coctx->nyield;
    mco_result rtn = mco_yield(coctx->curco);
    --coctx->nyield;
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
    rtn = mco_pop(coctx->curco, msg, sizeof(message_ctx));
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
    ASSERTAB(sess == msg->sess, "different session");
}
void coro_sleep(task_ctx *task, uint32_t ms) {
    message_ctx msg;
    uint64_t sess = createid();
    task_timeout(task, sess, ms, NULL);
    _coro_wait(task, 1, sess, MSG_TYPE_TIMEOUT, 0, &msg);
}
void *coro_request(task_ctx *dst, task_ctx *src, uint8_t rtype, void *data, size_t size, int32_t copy, int32_t *erro, size_t *lens) {
    uint64_t sess = createid();
    task_request(dst, src, rtype, sess, data, size, copy);
    message_ctx msg;
    _coro_wait(src, 1, sess, MSG_TYPE_RESPONSE, task_get_request_timeout(src), &msg);
    if (MSG_TYPE_TIMEOUT == msg.mtype) {
        *erro = ERR_FAILED;
        LOG_WARN("dst %d src %d request type %d timeout, session %"PRIu64".", dst->name, src->name, rtype, sess);
        return NULL;
    }
    *erro = msg.erro;
    if (NULL != lens) {
        *lens = msg.size;
    }
    return msg.data;
}
static int32_t _wait_ssl_exchanged(task_ctx *task, SOCKET fd, uint64_t skid) {
    message_ctx msg;
    _coro_wait(task, 0, skid, MSG_TYPE_SSLEXCHANGED, task_get_netread_timeout(task), &msg);
    if (MSG_TYPE_TIMEOUT == msg.mtype) {
        ev_close(&task->loader->netev, fd, skid);
        LOG_WARN("task %d, ssl exchange timeout, skid %"PRIu64".", task->name, skid);
        return ERR_FAILED;
    }
    if (MSG_TYPE_CLOSE == msg.mtype) {
        return ERR_FAILED;
    }
    return ERR_OK;
}
int32_t coro_ssl_exchange(task_ctx *task, SOCKET fd, uint64_t skid, int32_t client, struct evssl_ctx *evssl) {
    if (ERR_OK != ev_ssl(&task->loader->netev, fd, skid, client, evssl)) {
        return ERR_FAILED;
    }
    return _wait_ssl_exchanged(task, fd, skid);
}
void *coro_handshaked(task_ctx *task, SOCKET fd, uint64_t skid, int32_t *err, size_t *size) {
    message_ctx msg;
    _coro_wait(task, 0, skid, MSG_TYPE_HANDSHAKED, task_get_netread_timeout(task), &msg);
    if (MSG_TYPE_TIMEOUT == msg.mtype) {
        *err = ERR_FAILED;
        ev_close(&task->loader->netev, fd, skid);
        LOG_WARN("task: %d, handshake timeout, skid %"PRIu64".", task->name, skid);
        return NULL;
    }
    if (MSG_TYPE_CLOSE == msg.mtype) {
        *err = ERR_FAILED;
        return NULL;
    }
    *err = msg.erro;
    if (NULL != size) {
        *size = msg.size;
    }
    return msg.data;
}
int32_t coro_connect(task_ctx *task, pack_type pktype, struct evssl_ctx *evssl, const char *ip, uint16_t port, int32_t netev, void *extra,
    SOCKET *fd, uint64_t *skid) {
    if (ERR_OK != task_connect(task, pktype, evssl, ip, port, netev, extra, fd, skid)) {
        LOG_WARN("task: %d, connect %s:%d error.", task->name, ip, port);
        return ERR_FAILED;
    }
    message_ctx msg;
    _coro_wait(task, 0, *skid, MSG_TYPE_CONNECT, task_get_connect_timeout(task), &msg);
    if (MSG_TYPE_TIMEOUT == msg.mtype) {
        ev_close(&task->loader->netev, *fd, *skid);
        LOG_WARN("task: %d, connect %s:%d timeout.", task->name, ip, port);
        return ERR_FAILED;
    }
    if (ERR_OK != msg.erro) {
        LOG_WARN("task: %d, connect %s:%d error.", task->name, ip, port);
        return ERR_FAILED;
    }
    if (NULL != evssl) {
        if (ERR_OK != _wait_ssl_exchanged(task, *fd, *skid)) {
            return ERR_FAILED;
        }
    }
    coro_sync(task, *fd, *skid);
    return ERR_OK;
}
static int32_t _wait_recved(task_ctx *task, SOCKET fd, uint64_t skid, message_ctx *msg) {
    _coro_wait(task, 0, skid, MSG_TYPE_RECV, task_get_netread_timeout(task), msg);
    if (MSG_TYPE_TIMEOUT == msg->mtype) {
        ev_close(&task->loader->netev, fd, skid);
        LOG_WARN("task %d, recve timeout, skid %"PRIu64".", task->name, skid);
        return ERR_FAILED;
    }
    if (MSG_TYPE_CLOSE == msg->mtype) {
        return ERR_FAILED;
    }
    return ERR_OK;
}
void *coro_send(task_ctx *task, SOCKET fd, uint64_t skid, void *data, size_t len, size_t *size, int32_t copy) {
    if (ERR_OK != ev_send(&task->loader->netev, fd, skid, data, len, copy)) {
        return NULL;
    }
    message_ctx msg;
    if (ERR_OK != _wait_recved(task, fd, skid, &msg)) {
        return NULL;
    }
    if (NULL != size) {
        *size = msg.size;
    }
    return msg.data;
}
void *coro_slice(task_ctx *task, SOCKET fd, uint64_t skid, size_t *size, int32_t *end) {
    message_ctx msg;
    if (ERR_OK != _wait_recved(task, fd, skid, &msg)) {
        return NULL;
    }
    if (PROT_SLICE_END == msg.slice) {
        *end = 1;
    }
    if (NULL != size) {
        *size = msg.size;
    }
    return msg.data;
}
void *coro_sendto(task_ctx *task, SOCKET fd, uint64_t skid, const char *ip, const uint16_t port,
    void *data, size_t len, size_t *size, int32_t copy) {
    coro_sync(task, fd, skid);
    if (ERR_OK != ev_sendto(&task->loader->netev, fd, skid, ip, port, data, len, copy)) {
        LOG_WARN("task %d, sendto error, skid %"PRIu64".", task->name, skid);
        ev_ud_sess(&task->loader->netev, fd, skid, 0);
        return NULL;
    }
    message_ctx msg;
    _coro_wait(task, 1, skid, MSG_TYPE_RECVFROM, task_get_netread_timeout(task), &msg);
    if (MSG_TYPE_TIMEOUT == msg.mtype) {
        ev_ud_sess(&task->loader->netev, fd, skid, 0);
        LOG_WARN("task %d, sendto timeout, skid %"PRIu64".", task->name, skid);
        return NULL;
    }
    if (NULL != size) {
        *size = msg.size;
    }
    return ((char *)msg.data) + sizeof(netaddr_ctx);
}
