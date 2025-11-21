#include "srey/coro.h"
#include "srey/task.h"
#include "containers/hashmap.h"
#include "utils/timer.h"
#define MINICORO_IMPL
#include "srey/minicoro.h"

#define TIMEOUT_REQUEST 1500 
#define TIMEOUT_CONNECT 3000 
#define TIMEOUT_NETREAD 3000 

typedef struct coro_sess {
    msg_type mtype;
    mco_coro *coro;
    uint64_t sess;
    uint64_t timeout;
}coro_sess;
typedef struct coro_ctx {
    int32_t nyield;
    mco_coro *curcoro;
    struct hashmap *mapcoro;
    qu_ptr_ctx qucoropool;
    timer_ctx timer;
}coro_ctx;
static mco_desc _coro_desc;

static uint64_t _map_corosess_hash(const void *item, uint64_t seed0, uint64_t seed1) {
    return hash((const char *)&(((coro_sess *)item)->sess), sizeof(((coro_sess *)item)->sess));
}
static int _map_corosess_compare(const void *a, const void *b, void *ud) {
    return (int)(((coro_sess *)a)->sess - ((coro_sess *)b)->sess);
}
static inline void _map_corosess_set(task_ctx *task, mco_coro *coro, uint64_t sess, msg_type mtype, uint32_t ms) {
    coro_sess cosess;
    cosess.mtype = mtype;
    cosess.coro = coro;
    cosess.sess = sess;
    if (ms > 0) {
        cosess.timeout = timer_cur_ms(&task->coro->timer) + ms;
    } else {
        cosess.timeout = 0;
    }
    ASSERTAB(NULL == hashmap_set(task->coro->mapcoro, &cosess), "repeat session");
}
static inline int32_t _map_corosess_get(task_ctx *task, uint64_t sess, msg_type mtype, coro_sess *corosess) {
    coro_sess key;
    key.sess = sess;
    coro_sess *tmp = (coro_sess *)hashmap_get(task->coro->mapcoro, &key);
    if (NULL == tmp) {
        return ERR_FAILED;
    }
    if (mtype != tmp->mtype
        && MSG_TYPE_CLOSE != mtype) {
        return ERR_FAILED;
    }
    *corosess = *tmp;
    hashmap_delete(task->coro->mapcoro, &key);
    return  ERR_OK;
}
static void _mcoro_cb(mco_coro *coro) {
    mco_result rtn;
    task_dispatch_arg arg;
    for (;;) {
        rtn = mco_yield(coro);
        ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
        rtn = mco_pop(coro, &arg, sizeof(arg));
        ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
        task_incref(arg.task);//保证_message_run里yield后不会被释放
        _message_run(arg.task, &arg.msg);
        qu_ptr_push(&arg.task->coro->qucoropool, (void **)&coro);
        task_ungrab(arg.task);
    }
}
void _mcoro_init(size_t stack_size) {
    _coro_desc = mco_desc_init(_mcoro_cb, stack_size);
}
void _mcoro_new(task_ctx *task) {
    CALLOC(task->coro, 1, sizeof(coro_ctx));
    qu_ptr_init(&task->coro->qucoropool, 0);
    timer_init(&task->coro->timer);
    task->coro->mapcoro = hashmap_new_with_allocator(_malloc, _realloc, _free,
                                                   sizeof(coro_sess), ONEK, 0, 0,
                                                   _map_corosess_hash, _map_corosess_compare, NULL, NULL);
}
void _mcoro_free(task_ctx *task) {
    if (NULL == task->coro) {
        return;
    }
    mco_result rtn;
    mco_coro **coro;
    while (NULL != (coro = (mco_coro **)qu_ptr_pop(&task->coro->qucoropool))) {
        rtn = mco_destroy(*coro);
        if (MCO_SUCCESS != rtn) {
            LOG_WARN("%s", mco_result_description(rtn));
        }
    }
    qu_ptr_free(&task->coro->qucoropool);
    hashmap_free(task->coro->mapcoro);
    FREE(task->coro);
}
static mco_coro *_mcoro_pool_get(task_ctx *task) {
    mco_coro **coro = (mco_coro **)qu_ptr_pop(&task->coro->qucoropool);
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
static void _mcoro_create(task_dispatch_arg *arg) {
    arg->task->coro->curcoro = _mcoro_pool_get(arg->task);
    mco_result rtn = mco_push(arg->task->coro->curcoro, arg, sizeof(task_dispatch_arg));
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
    rtn = mco_resume(arg->task->coro->curcoro);
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
}
static void _mcoro_resume(mco_coro *coro, task_dispatch_arg *arg) {
    arg->task->coro->curcoro = coro;
    mco_result rtn = mco_push(coro, &arg->msg, sizeof(message_ctx));
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
    rtn = mco_resume(coro);
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
    _message_clean(arg->msg.mtype, arg->msg.pktype, arg->msg.data);
}
static void _timeout_dispatch(task_dispatch_arg *arg) {
    if (0 == arg->msg.sess) {
        _mcoro_create(arg);
        return;
    }
    coro_sess corosess;
    if (ERR_OK != _map_corosess_get(arg->task, arg->msg.sess, (msg_type)arg->msg.mtype, &corosess)) {
        LOG_WARN("task %d can't find session %"PRIu64, arg->task->name, arg->msg.sess);
        return;
    }
    _mcoro_resume(corosess.coro, arg);
}
static void _net_connect_dispatch(task_dispatch_arg *arg) {
    coro_sess corosess;
    if (ERR_OK != _map_corosess_get(arg->task, arg->msg.skid, (msg_type)arg->msg.mtype, &corosess)) {
        _mcoro_create(arg);
    } else {
        _mcoro_resume(corosess.coro, arg);
    }
}
static void _net_ssl_exchanged_dispatch(task_dispatch_arg *arg) {
    coro_sess corosess;
    if (ERR_OK != _map_corosess_get(arg->task, arg->msg.skid, (msg_type)arg->msg.mtype, &corosess)) {
        _mcoro_create(arg);
    } else {
        _mcoro_resume(corosess.coro, arg);
    }
}
static void _net_handshaked_dispatch(task_dispatch_arg *arg) {
    coro_sess corosess;
    if (ERR_OK != _map_corosess_get(arg->task, arg->msg.skid, (msg_type)arg->msg.mtype, &corosess)) {
        _mcoro_create(arg);
    } else {
        _mcoro_resume(corosess.coro, arg);
    }
}
static void _net_recv_dispatch(task_dispatch_arg *arg) {
    if (0 == arg->msg.sess) {
        if (0 != arg->msg.slice) {
            coro_sess corosess;
            if (ERR_OK != _map_corosess_get(arg->task, arg->msg.skid, (msg_type)arg->msg.mtype, &corosess)) {
                _mcoro_create(arg);
            } else {
                arg->msg.sess = arg->msg.skid;
                _mcoro_resume(corosess.coro, arg);
            }
        } else {
            _mcoro_create(arg);
        }
        return;
    }
    coro_sess corosess;
    if (ERR_OK != _map_corosess_get(arg->task, arg->msg.skid, (msg_type)arg->msg.mtype, &corosess)) {
        LOG_WARN("task %d can't find skid %"PRIu64".", arg->task->name, arg->msg.skid);
        _message_clean(arg->msg.mtype, arg->msg.pktype, arg->msg.data);
        return;
    }
    _mcoro_resume(corosess.coro, arg);
}
static void _net_close_dispatch(task_dispatch_arg *arg) {
    coro_sess corosess;
    if (ERR_OK == _map_corosess_get(arg->task, arg->msg.skid, (msg_type)arg->msg.mtype, &corosess)) {
        _mcoro_resume(corosess.coro, arg);
    }
    _mcoro_create(arg);
}
static void _net_recvfrom_dispatch(task_dispatch_arg *arg) {
    if (0 == arg->msg.sess) {
        _mcoro_create(arg);
        return;
    }
    coro_sess corosess;
    if (ERR_OK != _map_corosess_get(arg->task, arg->msg.skid, (msg_type)arg->msg.mtype, &corosess)) {
        LOG_WARN("task %d can't find skid %"PRIu64".", arg->task->name, arg->msg.skid);
        _message_clean(arg->msg.mtype, arg->msg.pktype, arg->msg.data);
        return;
    }
    _mcoro_resume(corosess.coro, arg);
}
static void _response_dispatch(task_dispatch_arg *arg) {
    coro_sess cosess;
    if (ERR_OK == _map_corosess_get(arg->task, arg->msg.sess, (msg_type)arg->msg.mtype, &cosess)) {
        _mcoro_resume(cosess.coro, arg);
    } else {
        _mcoro_create(arg);
    }
}
static void _mcoro_timeout(task_ctx *task, uint64_t sess) {
    if (task->coro->nyield > 0) {
        size_t iter = 0;
        mco_coro *coro;
        coro_sess *corosess;
        uint64_t now = timer_cur_ms(&task->coro->timer);
        task_dispatch_arg arg;
        arg.task = task;
        arg.msg.mtype = MSG_TYPE_TIMEOUT;
        while (hashmap_iter(task->coro->mapcoro, &iter, (void **)&corosess)) {
            if (corosess->timeout > 0
                && now >= corosess->timeout) {
                arg.msg.sess = corosess->sess;
                coro = corosess->coro;
                LOG_INFO("task %d resume timeout, session %"PRIu64".", task->name, corosess->sess);
                hashmap_delete(task->coro->mapcoro, corosess);
                _mcoro_resume(coro, &arg);
            }
        }
    }
    task_timeout(task, 0, 200, _mcoro_timeout);
}
void _message_dispatch(task_dispatch_arg *arg) {
    switch (arg->msg.mtype) {
    case MSG_TYPE_STARTUP:
        task_timeout(arg->task, 0, 200, _mcoro_timeout);
        _mcoro_create(arg);
        break;
    case MSG_TYPE_CLOSING:
        _mcoro_create(arg);
        if (arg->task->coro->nyield > 0) {
            LOG_WARN("task %d yield %d.", arg->task->name, arg->task->coro->nyield);
        }
        break;
    case MSG_TYPE_TIMEOUT:
        _timeout_dispatch(arg);
        break;
    case MSG_TYPE_ACCEPT:
        _mcoro_create(arg);
        break;
    case MSG_TYPE_CONNECT:
        _net_connect_dispatch(arg);
        break;
    case MSG_TYPE_SSLEXCHANGED:
        _net_ssl_exchanged_dispatch(arg);
        break;
    case MSG_TYPE_HANDSHAKED:
        _net_handshaked_dispatch(arg);
        break;
    case MSG_TYPE_RECV:
        _net_recv_dispatch(arg);
        break;
    case MSG_TYPE_SEND:
        _mcoro_create(arg);
        break;
    case MSG_TYPE_CLOSE:
        _net_close_dispatch(arg);
        break;
    case MSG_TYPE_RECVFROM:
        _net_recvfrom_dispatch(arg);
        break;
    case MSG_TYPE_REQUEST:
        _mcoro_create(arg);
        break;
    case MSG_TYPE_RESPONSE:
        _response_dispatch(arg);
        break;
    default:
        break;
    }
}
static inline void _mcoro_wait(task_ctx *task, uint64_t sess, msg_type mtype, uint32_t ms, message_ctx *msg) {
    _map_corosess_set(task, task->coro->curcoro, sess, mtype, ms);
    ++task->coro->nyield;
    mco_result rtn = mco_yield(task->coro->curcoro);
    --task->coro->nyield;
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
    rtn = mco_pop(task->coro->curcoro, msg, sizeof(message_ctx));
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
    ASSERTAB(sess == msg->sess, "different session");
}
void coro_sleep(task_ctx *task, uint32_t ms) {
    message_ctx msg;
    uint64_t sess = createid();
    task_timeout(task, sess, ms, NULL);
    _mcoro_wait(task, sess, MSG_TYPE_TIMEOUT, 0, &msg);
}
void *coro_request(task_ctx *dst, task_ctx *src, uint8_t rtype, void *data, size_t size, int32_t copy, int32_t *erro, size_t *lens) {
    uint64_t sess = createid();
    task_request(dst, src, rtype, sess, data, size, copy);
    message_ctx msg;
    _mcoro_wait(src, sess, MSG_TYPE_RESPONSE, TIMEOUT_REQUEST, &msg);
    if (MSG_TYPE_TIMEOUT == msg.mtype) {
        *lens = 0;
        *erro = ERR_FAILED;
        LOG_WARN("dst %d src %d, request timeout, session %"PRIu64".", dst->name, src->name, sess);
        return NULL;
    }
    *erro = msg.erro;
    *lens = msg.size;
    return msg.data;
}
static int32_t _wait_ssl_exchanged(task_ctx *task, SOCKET fd, uint64_t skid) {
    message_ctx msg;
    _mcoro_wait(task, skid, MSG_TYPE_SSLEXCHANGED, TIMEOUT_NETREAD, &msg);
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
    ev_ssl(&task->loader->netev, fd, skid, client, evssl);
    return _wait_ssl_exchanged(task, fd, skid);
}
void *coro_handshaked(task_ctx *task, SOCKET fd, uint64_t skid, int32_t *err, size_t *size) {
    message_ctx msg;
    _mcoro_wait(task, skid, MSG_TYPE_HANDSHAKED, TIMEOUT_NETREAD, &msg);
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
SOCKET coro_connect(task_ctx *task, pack_type pktype, struct evssl_ctx *evssl,
    const char *ip, uint16_t port, uint64_t *skid, int32_t netev, void *extra) {
    SOCKET fd = task_connect(task, pktype, evssl, ip, port, skid, netev, extra);
    if (INVALID_SOCK == fd) {
        LOG_WARN("task: %d, connect %s:%d error.", task->name, ip, port);
        return INVALID_SOCK;
    }
    message_ctx msg;
    _mcoro_wait(task, *skid, MSG_TYPE_CONNECT, TIMEOUT_CONNECT, &msg);
    if (MSG_TYPE_TIMEOUT == msg.mtype) {
        ev_close(&task->loader->netev, fd, *skid);
        LOG_WARN("task: %d, connect %s:%d timeout.", task->name, ip, port);
        return INVALID_SOCK;
    }
    if (ERR_OK != msg.erro) {
        LOG_WARN("task: %d, connect %s:%d error.", task->name, ip, port);
        return INVALID_SOCK;
    }
    if (NULL != evssl) {
        if (ERR_OK != _wait_ssl_exchanged(task, fd, *skid)) {
            return INVALID_SOCK;
        }
    }
    return fd;
}
static int32_t _wait_recved(task_ctx *task, SOCKET fd, uint64_t skid, message_ctx *msg) {
    _mcoro_wait(task, skid, MSG_TYPE_RECV, TIMEOUT_NETREAD, msg);
    if (MSG_TYPE_TIMEOUT == msg->mtype) {
        ev_close(&task->loader->netev, fd, skid);
        LOG_WARN("task %d, send timeout, skid %"PRIu64".", task->name, skid);
        return ERR_FAILED;
    }
    if (MSG_TYPE_CLOSE == msg->mtype) {
        return ERR_FAILED;
    }
    return ERR_OK;
}
void *coro_send(task_ctx *task, SOCKET fd, uint64_t skid, void *data, size_t len, size_t *size, int32_t copy) {
    ev_ud_sess(&task->loader->netev, fd, skid, skid);
    ev_send(&task->loader->netev, fd, skid, data, len, copy);
    message_ctx msg;
    if (ERR_OK != _wait_recved(task, fd, skid, &msg)) {
        return NULL;
    }
    *size = msg.size;
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
    *size = msg.size;
    return msg.data;
}
void *coro_sendto(task_ctx *task, SOCKET fd, uint64_t skid,
    const char *ip, const uint16_t port, void *data, size_t len, size_t *size) {
    ev_ud_sess(&task->loader->netev, fd, skid, skid);
    if (ERR_OK != ev_sendto(&task->loader->netev, fd, skid, ip, port, data, len)) {
        LOG_WARN("task %d, sendto error, skid %"PRIu64".", task->name, skid);
        ev_ud_sess(&task->loader->netev, fd, skid, 0);
        return NULL;
    }
    message_ctx msg;
    _mcoro_wait(task, skid, MSG_TYPE_RECVFROM, TIMEOUT_NETREAD, &msg);
    if (MSG_TYPE_TIMEOUT == msg.mtype) {
        ev_ud_sess(&task->loader->netev, fd, skid, 0);
        LOG_WARN("task %d, sendto timeout, skid %"PRIu64".", task->name, skid);
        return NULL;
    }
    *size = msg.size;
    return ((char *)msg.data) + sizeof(netaddr_ctx);
}
