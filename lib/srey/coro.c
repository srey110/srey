#include "srey/coro.h"
#include "srey/task.h"
#include "srey/trigger.h"
#include "ds/hashmap.h"
#if WITH_CORO
#define MINICORO_IMPL
#include "srey/minicoro.h"

typedef struct coro_sess {
    int32_t keep;
    mco_coro *co;
    uint64_t sess;
    uint64_t assoc;//关联
}coro_sess;
typedef struct coro_ctx {
    mco_coro *curco;
    struct hashmap *mapco;
    qu_ptr_ctx qucopool;
}coro_ctx;
static mco_desc _coro_desc;

#define CO_RESUME(arg, co) \
    arg->task->coro->curco = co; \
    mco_result _cortn = mco_push(co, &arg->msg, sizeof(message_ctx)); \
    ASSERTAB(MCO_SUCCESS == _cortn, mco_result_description(_cortn)); \
    _cortn = mco_resume(co); \
    ASSERTAB(MCO_SUCCESS == _cortn, mco_result_description(_cortn));\
    _message_clean(arg->msg.mtype, arg->msg.pktype, arg->msg.data)

static uint64_t _map_cosess_hash(const void *item, uint64_t seed0, uint64_t seed1) {
    return hash((const char *)&(((coro_sess *)item)->sess), sizeof(((coro_sess *)item)->sess));
}
static int _map_cosess_compare(const void *a, const void *b, void *ud) {
    return (int)(((coro_sess *)a)->sess - ((coro_sess *)b)->sess);
}
static void _map_cosess_set(task_ctx *task, mco_coro *co, uint64_t sess, uint64_t assoc, int32_t keep) {
    coro_sess cosess;
    cosess.keep = keep;
    cosess.co = co;
    cosess.sess = sess;
    cosess.assoc = assoc;
    hashmap_set(task->coro->mapco, &cosess);
}
static void _map_cosess_del(task_ctx *task, uint64_t sess) {
    coro_sess key;
    key.sess = sess;
    hashmap_delete(task->coro->mapco, &key);
}
static int32_t _map_cosess_get(task_ctx *task, uint64_t sess, coro_sess *cosess) {
    coro_sess key;
    key.sess = sess;
    coro_sess *tmp = (coro_sess *)hashmap_get(task->coro->mapco, &key);
    if (NULL == tmp) {
        return ERR_FAILED;
    }
    *cosess = *tmp;
    if (0 == cosess->keep) {
        hashmap_delete(task->coro->mapco, &key);
    }
    return  ERR_OK;
}
static void _coro_cb(mco_coro *co) {
    task_dispatch_arg arg;
    mco_result rtn;
    for (;;) {
        rtn = mco_yield(co);
        ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
        rtn = mco_pop(co, &arg, sizeof(arg));
        ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
        task_incref(arg.task);//保证task->_run里YIELD后不会被释放
        _message_run(arg.task, &arg.msg);
        qu_ptr_push(&arg.task->coro->qucopool, (void **)&co);
        task_ungrab(arg.task);
    }
}
void _coro_init(size_t stack_size) {
    _coro_desc = mco_desc_init(_coro_cb, stack_size);
}
void _coro_new(task_ctx *task) {
    CALLOC(task->coro, 1, sizeof(coro_ctx));
    qu_ptr_init(&task->coro->qucopool, 0);
    task->coro->mapco = hashmap_new_with_allocator(_malloc, _realloc, _free,
                                                   sizeof(coro_sess), ONEK, 0, 0,
                                                   _map_cosess_hash, _map_cosess_compare, NULL, NULL);
}
void _coro_free(task_ctx *task) {
    if (NULL == task->coro) {
        return;
    }
    mco_coro **co;
    mco_result cortn;
    while (NULL != (co = (mco_coro **)qu_ptr_pop(&task->coro->qucopool))) {
        cortn = mco_destroy(*co);
        if (MCO_SUCCESS != cortn) {
            LOG_WARN("%s", mco_result_description(cortn));
        }
    }
    qu_ptr_free(&task->coro->qucopool);
    hashmap_free(task->coro->mapco);
    FREE(task->coro);
}
static mco_coro *_coro_pool_get(task_ctx *task) {
    mco_coro **co = (mco_coro **)qu_ptr_pop(&task->coro->qucopool);
    if (NULL != co) {
        return *co;
    }
    mco_coro *conew;
    mco_result cortn = mco_create(&conew, &_coro_desc);
    ASSERTAB(MCO_SUCCESS == cortn, mco_result_description(cortn));
    cortn = mco_resume(conew);
    ASSERTAB(MCO_SUCCESS == cortn, mco_result_description(cortn));
    return conew;
}
static void _coro_create(task_dispatch_arg *arg) {
    arg->task->coro->curco = _coro_pool_get(arg->task);
    mco_result cortn = mco_push(arg->task->coro->curco, arg, sizeof(task_dispatch_arg));
    ASSERTAB(MCO_SUCCESS == cortn, mco_result_description(cortn));
    cortn = mco_resume(arg->task->coro->curco);
    ASSERTAB(MCO_SUCCESS == cortn, mco_result_description(cortn));
}
static void _timeout_dispatch(task_dispatch_arg *arg) {
    if (0 == arg->msg.sess) {
        if (NULL != arg->msg.data) {
            _coro_create(arg);
        }
        return;
    }
    coro_sess cosess;
    if (ERR_OK == _map_cosess_get(arg->task, arg->msg.sess, &cosess)) {
        if (0 != cosess.assoc) {
            _map_cosess_del(arg->task, cosess.assoc);
        }
        CO_RESUME(arg, cosess.co);
    } else {
        if (NULL != arg->msg.data) {
            _coro_create(arg);
        }
    }
}
static void _net_connect_dispatch(task_dispatch_arg *arg) {
    if (0 == arg->msg.sess) {
        _coro_create(arg);
        return;
    }
    coro_sess cosess;
    if (ERR_OK == _map_cosess_get(arg->task, arg->msg.sess, &cosess)) {
        CO_RESUME(arg, cosess.co);
    } else {
        _coro_create(arg);
    }
}
static void _net_handshaked_dispatch(task_dispatch_arg *arg) {
    if (0 == arg->msg.sess) {
        _coro_create(arg);
        return;
    }
    coro_sess cosess;
    if (ERR_OK == _map_cosess_get(arg->task, arg->msg.sess, &cosess)) {
        CO_RESUME(arg, cosess.co);
    } else {
        _coro_create(arg);
    }
}
static void _net_recv_dispatch(task_dispatch_arg *arg) {
    if (0 == arg->msg.sess) {
        _coro_create(arg);
        return;
    }
    coro_sess cosess;
    if (ERR_OK == _map_cosess_get(arg->task, arg->msg.sess, &cosess)) {
        if (SLICE_START == arg->msg.slice) {
            _map_cosess_set(arg->task, cosess.co, cosess.sess, 0, 1);
        } else if (SLICE_END == arg->msg.slice) {
            _map_cosess_del(arg->task, arg->msg.sess);
        }
        CO_RESUME(arg, cosess.co);
    } else {
        _coro_create(arg);
    }
}
static void _net_close_dispatch(task_dispatch_arg *arg) {
    if (0 != arg->msg.sess) {
        coro_sess cosess;
        if (ERR_OK == _map_cosess_get(arg->task, arg->msg.sess, &cosess)) {
            if (0 != cosess.keep) {
                _map_cosess_del(arg->task, arg->msg.sess);
            }
            CO_RESUME(arg, cosess.co);
        }
    }
    _coro_create(arg);
}
static void _net_recvfrom_dispatch(task_dispatch_arg *arg) {
    if (0 == arg->msg.sess) {
        _coro_create(arg);
        return;
    }
    coro_sess cosess;
    if (ERR_OK == _map_cosess_get(arg->task, arg->msg.sess, &cosess)) {
        CO_RESUME(arg, cosess.co);
    } else {
        _coro_create(arg);
    }
}
static void _response_dispatch(task_dispatch_arg *arg) {
    coro_sess cosess;
    if (ERR_OK == _map_cosess_get(arg->task, arg->msg.sess, &cosess)) {
        CO_RESUME(arg, cosess.co);
    } else {
        _coro_create(arg);
    }
}
void _message_dispatch(task_dispatch_arg *arg) {
    switch (arg->msg.mtype) {
    case MSG_TYPE_STARTUP:
        _coro_create(arg);
        break;
    case MSG_TYPE_CLOSING:
        _coro_create(arg);
        break;
    case MSG_TYPE_TIMEOUT:
        _timeout_dispatch(arg);
        break;
    case MSG_TYPE_ACCEPT:
        _coro_create(arg);
        break;
    case MSG_TYPE_CONNECT:
        _net_connect_dispatch(arg);
        break;
    case MSG_TYPE_HANDSHAKED:
        _net_handshaked_dispatch(arg);
        break;
    case MSG_TYPE_RECV:
        _net_recv_dispatch(arg);
        break;
    case MSG_TYPE_SEND:
        _coro_create(arg);
        break;
    case MSG_TYPE_CLOSE:
        _net_close_dispatch(arg);
        break;
    case MSG_TYPE_RECVFROM:
        _net_recvfrom_dispatch(arg);
        break;
    case MSG_TYPE_REQUEST:
        _coro_create(arg);
        break;
    case MSG_TYPE_RESPONSE:
        _response_dispatch(arg);
        break;
    default:
        break;
    }
}
void _coro_wait(task_ctx *task, int32_t ms, uint64_t sess, uint64_t assoc, message_ctx *msg) {
    _map_cosess_set(task, task->coro->curco, sess, assoc, 0);
    if (ms >= 0) {
        trigger_timeout(task, sess, ms, NULL);
    }
    mco_result cortn = mco_yield(task->coro->curco);
    ASSERTAB(MCO_SUCCESS == cortn, mco_result_description(cortn));
    cortn = mco_pop(task->coro->curco, msg, sizeof(message_ctx));
    ASSERTAB(MCO_SUCCESS == cortn, mco_result_description(cortn));
    ASSERTAB(0 != msg->sess && (sess == msg->sess || assoc == msg->sess), "different session.");
}
void coro_sleep(task_ctx *task, uint32_t ms) {
    message_ctx msg;
    _coro_wait(task, (int32_t)ms, createid(), 0, &msg);
}
void *coro_request(task_ctx *dst, task_ctx *src, int32_t ms,
    uint8_t rtype, void *data, size_t size, int32_t copy,
    int32_t *erro, size_t *lens) {
    uint64_t sess = createid();
    trigger_request(dst, src, rtype, sess, data, size, copy);
    message_ctx msg;
    _coro_wait(src, ms, sess, 0, &msg);
    if (MSG_TYPE_TIMEOUT == msg.mtype) {
        *lens = 0;
        *erro = ERR_FAILED;
        LOG_WARN("dst %d src %d, request timeout.", dst->name, src->name);
        return NULL;
    }
    *erro = msg.erro;
    *lens = msg.size;
    return msg.data;
}
void *coro_slice(task_ctx *task, int32_t ms,
    SOCKET fd, uint64_t skid, uint64_t sess,
    size_t *size, int32_t *end) {
    message_ctx msg;
    uint64_t slice_sess = createid();
    _coro_wait(task, ms, slice_sess, sess, &msg);
    if (MSG_TYPE_TIMEOUT == msg.mtype) {
        ev_ud_sess(&task->scheduler->netev, fd, skid, 0);
        ev_close(&task->scheduler->netev, fd, skid);
        LOG_WARN("task: %d, slice timeout.", task->name);
        return NULL;
    }
    _map_cosess_del(task, slice_sess);
    if (MSG_TYPE_CLOSE == msg.mtype) {
        LOG_WARN("task: %d, slice connction closed,session: %"PRIu64".", task->name, sess);
        return NULL;
    }
    if (SLICE_END == msg.slice) {
        *end = 1;
    }
    *size = msg.size;
    return msg.data;
}
SOCKET coro_connect(task_ctx *task, int32_t ms, pack_type pktype, struct evssl_ctx *evssl,
    const char *ip, uint16_t port, uint64_t *skid, int32_t appendev) {
    uint64_t sess = createid();
    SOCKET fd = trigger_connect(task, sess, pktype, evssl, ip, port, skid, appendev);
    if (INVALID_SOCK == fd) {
        return INVALID_SOCK;
    }
    message_ctx msg;
    _coro_wait(task, ms, sess, 0, &msg);
    if (MSG_TYPE_TIMEOUT == msg.mtype) {
        ev_ud_sess(&task->scheduler->netev, fd, *skid, 0);
        ev_close(&task->scheduler->netev, fd, *skid);
        LOG_WARN("task: %d, connect host %s:%d timeout.", task->name, ip, port);
        return INVALID_SOCK;
    }
    if (ERR_OK != msg.erro) {
        LOG_WARN("task: %d, connect host %s:%d error.", task->name, ip, port);
        return INVALID_SOCK;
    }
    return fd;
}
void *coro_send(task_ctx *task, int32_t ms, SOCKET fd, uint64_t skid, uint64_t sess,
    void *data, size_t len, size_t *size, int32_t copy) {
    ev_ud_sess(&task->scheduler->netev, fd, skid, sess);
    ev_send(&task->scheduler->netev, fd, skid, data, len, copy);
    message_ctx msg;
    _coro_wait(task, ms, sess, 0, &msg);
    if (MSG_TYPE_TIMEOUT == msg.mtype) {
        ev_ud_sess(&task->scheduler->netev, fd, skid, 0);
        ev_close(&task->scheduler->netev, fd, skid);
        LOG_WARN("task %d, send timeout.", task->name);
        return NULL;
    }
    if (MSG_TYPE_CLOSE == msg.mtype) {
        LOG_WARN("task %d, connction closed.", task->name);
        return NULL;
    }
    *size = msg.size;
    return msg.data;
}
void *coro_sendto(task_ctx *task, int32_t ms, SOCKET fd, uint64_t skid,
    const char *ip, const uint16_t port, void *data, size_t len, size_t *size) {
    uint64_t sess = createid();
    ev_ud_sess(&task->scheduler->netev, fd, skid, sess);
    if (ERR_OK != ev_sendto(&task->scheduler->netev, fd, skid, ip, port, data, len)) {
        ev_ud_sess(&task->scheduler->netev, fd, skid, 0);
        return NULL;
    }
    message_ctx msg;
    _coro_wait(task, ms, sess, 0, &msg);
    if (MSG_TYPE_TIMEOUT == msg.mtype) {
        ev_ud_sess(&task->scheduler->netev, fd, skid, 0);
        LOG_WARN("task %d, sendto timeout.", task->name);
        return NULL;
    }
    *size = msg.size;
    return ((char *)msg.data) + sizeof(netaddr_ctx);
}

#endif
