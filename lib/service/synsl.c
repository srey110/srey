#include "service/synsl.h"
#if WITH_CORO
#include "service/srey.h"
#include "service/minicoro.h"
#include "service/maps.h"
#include "proto/dns.h"
#include "proto/websock.h"
#include "proto/http.h"
#include "buffer.h"

typedef enum timeout_type {
    TMO_TYPE_SLEEP = 0x01,
    TMO_TYPE_NORMAL,
    TMO_TYPE_SESS
}timeout_type;
typedef struct coro_ctx {
    uint32_t cnt;
    uint32_t nnew;
    mco_coro *curco;
    mapco_ctx mapco;
    qu_ptr qucopool;
}coro_ctx;

#define COROPOOL_KEEP         8
#define COROPOOL_NDEL         3
#define CONNECT_TIMEOUT       3000
#define NETRD_TIMEOUT         1500
#define REQUEST_TIMEOUT       1000
static mco_desc _coro_desc;

#define CO_CREATE(arg)\
do {\
    arg->task->coro->curco = _co_create(arg->task);\
    mco_result cortn = mco_push(arg->task->coro->curco, arg, sizeof(task_msg_arg));\
    ASSERTAB(MCO_SUCCESS == cortn, mco_result_description(cortn));\
    cortn = mco_resume(arg->task->coro->curco);\
    ASSERTAB(MCO_SUCCESS == cortn, mco_result_description(cortn));\
} while (0)
#define CO_YIELD(task)\
do {\
    mco_result cortn = mco_yield(task->coro->curco); \
    ASSERTAB(MCO_SUCCESS == cortn, mco_result_description(cortn));\
} while (0)
#define CO_RESUME(arg, co) \
do {\
    arg->task->coro->curco = co; \
    mco_result cortn = mco_push(co, &arg->msg, sizeof(message_ctx)); \
    ASSERTAB(MCO_SUCCESS == cortn, mco_result_description(cortn)); \
    cortn = mco_resume(co); \
    ASSERTAB(MCO_SUCCESS == cortn, mco_result_description(cortn));\
} while (0)
#define CO_POP(co, msg)\
do {\
    mco_result cortn = mco_pop(co, &msg, sizeof(msg));\
    ASSERTAB(MCO_SUCCESS == cortn, mco_result_description(cortn));\
} while (0)

static inline mco_coro *_co_create(task_ctx *task) {
    mco_result cortn;
    mco_coro **co = (mco_coro **)qu_ptr_pop(&task->coro->qucopool);
    if (NULL != co) {
        cortn = mco_uninit(*co);
        ASSERTAB(MCO_SUCCESS == cortn, mco_result_description(cortn));
        cortn = mco_init(*co, &_coro_desc);
        ASSERTAB(MCO_SUCCESS == cortn, mco_result_description(cortn));
        return *co;
    }
    mco_coro *conew;
    cortn = mco_create(&conew, &_coro_desc);
    ASSERTAB(MCO_SUCCESS == cortn, mco_result_description(cortn));
    task->coro->nnew++;
    return conew;
}
static void _coro(mco_coro *co) {
    task_msg_arg arg;
    CO_POP(co, arg);
    srey_task_addref(arg.task);//保证YIELD后不会被释放
    arg.task->_run(arg.task, &arg.msg);
    message_clean(arg.msg.mtype, arg.msg.pktype, arg.msg.data);
    qu_ptr_push(&arg.task->coro->qucopool, (void **)&co);
    srey_task_release(arg.task);
    if (MSG_TYPE_CLOSING == arg.msg.mtype) {
        srey_task_release(arg.task);
    }
}
void _coro_init_desc(size_t stack_size) {
    _coro_desc = mco_desc_init(_coro, stack_size);
}
coro_ctx *_coro_new(void) {
    coro_ctx *coctx;
    MALLOC(coctx, sizeof(coro_ctx));
    coctx->cnt = 0;
    coctx->nnew = 0;
    coctx->curco = NULL;
    _map_co_init(&coctx->mapco);
    qu_ptr_init(&coctx->qucopool, ONEK);
    return coctx;
}
void _coro_free(coro_ctx *coctx) {
    if (NULL == coctx) {
        return;
    }
    mco_coro **co;
    while (NULL != (co = (mco_coro **)qu_ptr_pop(&coctx->qucopool))) {
        mco_destroy(*co);
    }
    qu_ptr_free(&coctx->qucopool);
    _map_co_free(&coctx->mapco);
    FREE(coctx);
}
static inline void _coro_pool_shrink(coro_ctx *coctx) {
    size_t npool = qu_ptr_size(&coctx->qucopool);
    if (npool <= COROPOOL_KEEP) {
        return;
    }
    size_t ndel, reduce, atmost;
    atmost = npool - COROPOOL_KEEP;
    reduce = COROPOOL_KEEP * (coctx->cnt - (COROPOOL_NDEL - 1));
    if (reduce <= atmost) {
        ndel = reduce;
    } else {
        ndel = atmost;
    }
    mco_coro **co;
    for (size_t i = 0; i < ndel; i++) {
        co = (mco_coro **)qu_ptr_pop(&coctx->qucopool);
        mco_destroy(*co);
    }
}
void _coro_shrink(coro_ctx *coctx) {
    if (NULL == coctx) {
        return;
    }
    //活跃转为一直无消息，则不能
    if (0 == coctx->nnew) {
        coctx->cnt++;
        if (coctx->cnt >= COROPOOL_NDEL) {
            _coro_pool_shrink(coctx);
        }
    } else {
        coctx->cnt = 0;
    }
    coctx->nnew = 0;
}
static inline void _dispatch_wakeup(task_msg_arg *arg) {
    co_sess_ctx cosess;
    if (ERR_OK == _map_cosess_get(&arg->task->coro->mapco, arg->msg.sess, &cosess, 1)) {
        CO_RESUME(arg, cosess.co);
    } else {
        LOG_ERROR("task: %d, can't find session:%"PRIu64, arg->task->name, arg->msg.sess);
    }
}
static inline void _dispatch_timeout(task_msg_arg *arg) {
    co_tmo_ctx cotmo;
    if (ERR_OK != _map_cotmo_get(&arg->task->coro->mapco, arg->msg.sess, &cotmo)) {
        return;
    }
    switch (cotmo.type) {
    case TMO_TYPE_SLEEP:
        CO_RESUME(arg, cotmo.co);
        break;
    case TMO_TYPE_NORMAL:
        CO_CREATE(arg);
        break;
    case TMO_TYPE_SESS: {
        co_sess_ctx cosess;
        if (ERR_OK == _map_cosess_get(&arg->task->coro->mapco, arg->msg.sess, &cosess, 1)) {
            CO_RESUME(arg, cosess.co);
        }
        break;
    }
    default:
        break;
    }
}
static inline void _dispatch_connect(task_msg_arg *arg) {
    if (0 != arg->msg.sess) {
        co_sess_ctx cosess;
        if (ERR_OK == _map_cosess_get(&arg->task->coro->mapco, arg->msg.sess, &cosess, 1)) {
            _map_cotmo_del(&arg->task->coro->mapco, arg->msg.sess);
            CO_RESUME(arg, cosess.co);
        }
    } else {
        CO_CREATE(arg);
    }
}
static inline void _dispatch_handshaked(task_msg_arg *arg) {
    if (0 != arg->msg.sess) {
        co_sess_ctx cosess;
        if (ERR_OK == _map_cosess_get(&arg->task->coro->mapco, arg->msg.sess, &cosess, 1)) {
            _map_cotmo_del(&arg->task->coro->mapco, arg->msg.sess);
            CO_RESUME(arg, cosess.co);
        }
    } else {
        CO_CREATE(arg);
    }
}
static inline void _dispatch_netrd(task_msg_arg *arg) {
    if (0 != arg->msg.sess) {
        int32_t del;
        if (SLICE == arg->msg.slice
            || SLICE_START == arg->msg.slice) {
            del = 0;
        } else {
            del = 1;
        }
        co_sess_ctx cosess;
        if (ERR_OK == _map_cosess_get(&arg->task->coro->mapco, arg->msg.sess, &cosess, del)) {
            if (SLICE_NONE == arg->msg.slice
                || SLICE_START == arg->msg.slice) {
                _map_cotmo_del(&arg->task->coro->mapco, arg->msg.sess);
            }
            CO_RESUME(arg, cosess.co);
        }
        message_clean(arg->msg.mtype, arg->msg.pktype, arg->msg.data);
    } else {
        CO_CREATE(arg);
    }
}
static inline void _dispatch_close(task_msg_arg *arg) {
    if (0 != arg->msg.sess) {
        co_sess_ctx cosess;
        if (ERR_OK == _map_cosess_get(&arg->task->coro->mapco, arg->msg.sess, &cosess, 1)) {
            _map_cotmo_del(&arg->task->coro->mapco, arg->msg.sess);
            CO_RESUME(arg, cosess.co);
        }
    }
    CO_CREATE(arg);
}
static inline void _dispatch_response(task_msg_arg *arg) {
    co_sess_ctx cosess;
    if (ERR_OK == _map_cosess_get(&arg->task->coro->mapco, arg->msg.sess, &cosess, 1)) {
        CO_RESUME(arg, cosess.co);
    } else {
        LOG_ERROR("task: %d, can't find session:%"PRIu64, arg->task->name, arg->msg.sess);
    }
    message_clean(arg->msg.mtype, arg->msg.pktype, arg->msg.data);
}
void _dispatch_coro(task_msg_arg *arg) {
    switch (arg->msg.mtype) {
    case MSG_TYPE_WAKEUP:
        _dispatch_wakeup(arg);
        break;
    case MSG_TYPE_STARTUP:
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
        _dispatch_handshaked(arg);
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

//MSG_TYPE_WAKEUP触发
int32_t syn_task_new(task_ctx *task, task_type ttype, name_t name, uint16_t maxcnt, uint16_t maxmsgqulens,
    task_new _init, task_run _run, free_cb _tfree, free_cb _argfree, void *arg) {
    uint64_t sess = createid();
    if (ERR_OK != srey_task_new(task->srey, ttype, name, maxcnt, maxmsgqulens,
                                task->name, sess, _init, _run, _tfree, _argfree, arg)) {
        return ERR_FAILED;
    }
    _map_cosess_add(&task->coro->mapco, task->coro->curco, sess);
    CO_YIELD(task);
    message_ctx msg;
    CO_POP(task->coro->curco, msg);
    if (sess != msg.sess) {
        _map_cosess_del(&task->coro->mapco, sess);
        LOG_FATAL("task: %d, request session: %"PRIu64", response session: %"PRIu64" not the same.",
            task->name, sess, msg.sess);
        return ERR_FAILED;
    }
    return msg.erro;
}
//MSG_TYPE_TIMEOUT触发 TMO_TYPE_SLEEP
void syn_sleep(task_ctx *task, uint32_t ms) {
    uint64_t sess = createid();
    _map_cotmo_add(&task->coro->mapco, TMO_TYPE_SLEEP, task->coro->curco, sess);
    srey_timeout(task, sess, ms);
    CO_YIELD(task);
    message_ctx msg;
    CO_POP(task->coro->curco, msg);
    if (sess != msg.sess) {
        _map_cosess_del(&task->coro->mapco, sess);
        LOG_FATAL("task: %d, request session: %"PRIu64", response session: %"PRIu64" not the same.",
            task->name, sess, msg.sess);
    }
}
//MSG_TYPE_TIMEOUT触发 TMO_TYPE_NORMAL
void syn_timeout(task_ctx *task, uint64_t sess, uint32_t ms) {
    _map_cotmo_add(&task->coro->mapco, TMO_TYPE_NORMAL, task->coro->curco, sess);
    srey_timeout(task, sess, ms);
}
//MSG_TYPE_RESPONSE触发
void *syn_request(task_ctx *dst, task_ctx *src, void *data, size_t size, int32_t copy, int32_t *erro, size_t *lens) {
    uint64_t sess = createid();
    _map_cosess_add(&src->coro->mapco, src->coro->curco, sess);
    _map_cotmo_add(&src->coro->mapco, TMO_TYPE_SESS, src->coro->curco, sess);
    srey_timeout(src, sess, REQUEST_TIMEOUT);
    srey_request(dst, src, sess, data, size, copy);
    CO_YIELD(src);
    message_ctx msg;
    CO_POP(src->coro->curco, msg);
    if (sess != msg.sess) {
        _map_cosess_del(&src->coro->mapco, sess);
        _map_cotmo_del(&src->coro->mapco, sess);
        *lens = 0;
        *erro = ERR_FAILED;
        LOG_FATAL("dst %d src %d, request session: %"PRIu64", response session: %"PRIu64" not the same.",
            dst->name, src->name, sess, msg.sess);
        return NULL;
    }
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

//MSG_TYPE_RECV MSG_TYPE_CLOSE触发
void *syn_slice(task_ctx *task, SOCKET fd, uint64_t skid, uint64_t sess, size_t *size, int32_t *end) {
    CO_YIELD(task);
    message_ctx msg;
    CO_POP(task->coro->curco, msg);
    if (sess != msg.sess) {
        _map_cosess_del(&task->coro->mapco, sess);
        ev_ud_sess(&task->srey->netev, fd, skid, 0);
        ev_close(&task->srey->netev, fd, skid);
        LOG_FATAL("task: %d, request session: %"PRIu64", response session: %"PRIu64" not the same.",
            task->name, sess, msg.sess);
        return NULL;
    }
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
//MSG_TYPE_CONNECT MSG_TYPE_TIMEOUT触发
SOCKET syn_connect(task_ctx *task, pack_type pktype, struct evssl_ctx *evssl,
    const char *ip, uint16_t port, int32_t sendev, uint64_t *skid) {
    uint64_t sess = createid();
    SOCKET fd = srey_connect(task, sess, pktype, evssl, ip, port, sendev, skid);
    if (INVALID_SOCK == fd) {
        return INVALID_SOCK;
    }
    _map_cosess_add(&task->coro->mapco, task->coro->curco, sess);
    _map_cotmo_add(&task->coro->mapco, TMO_TYPE_SESS, task->coro->curco, sess);
    srey_timeout(task, sess, CONNECT_TIMEOUT);
    CO_YIELD(task);
    message_ctx msg;
    CO_POP(task->coro->curco, msg);
    if (sess != msg.sess) {
        _map_cosess_del(&task->coro->mapco, sess);
        _map_cotmo_del(&task->coro->mapco, sess);
        ev_ud_sess(&task->srey->netev, fd, *skid, 0);
        ev_close(&task->srey->netev, fd, *skid);
        LOG_FATAL("task: %d, request session: %"PRIu64", response session: %"PRIu64" not the same.",
            task->name, sess, msg.sess);
        return INVALID_SOCK;
    }
    if (MSG_TYPE_TIMEOUT == msg.mtype) {
        ev_ud_sess(&task->srey->netev, fd, *skid, 0);
        ev_close(&task->srey->netev, fd, *skid);
        LOG_WARN("task: %d, connect host %s:%d timeout.", task->name, ip, port);
        return INVALID_SOCK;
    }
    if (ERR_OK != msg.erro) {
        LOG_WARN("task: %d, connect host %s:%d error.", task->name, ip, port);
        return INVALID_SOCK;
    }
    return fd;
}
//MSG_TYPE_RECV MSG_TYPE_TIMEOUT MSG_TYPE_CLOSE触发
void *syn_send(task_ctx *task, SOCKET fd, uint64_t skid, uint64_t sess,
    void *data, size_t len, size_t *size, int32_t copy) {
    ev_ud_sess(&task->srey->netev, fd, skid, sess);
    _map_cosess_add(&task->coro->mapco, task->coro->curco, sess);
    _map_cotmo_add(&task->coro->mapco, TMO_TYPE_SESS, task->coro->curco, sess);
    srey_timeout(task, sess, NETRD_TIMEOUT);
    ev_send(&task->srey->netev, fd, skid, data, len, copy);
    CO_YIELD(task);
    message_ctx msg;
    CO_POP(task->coro->curco, msg);
    if (sess != msg.sess) {
        _map_cosess_del(&task->coro->mapco, sess);
        _map_cotmo_del(&task->coro->mapco, sess);
        ev_ud_sess(&task->srey->netev, fd, skid, 0);
        ev_close(&task->srey->netev, fd, skid);
        LOG_FATAL("task: %d, request session: %"PRIu64", response session: %"PRIu64" not the same.",
            task->name, sess, msg.sess);
        return NULL;
    }
    if (MSG_TYPE_TIMEOUT == msg.mtype) {
        ev_ud_sess(&task->srey->netev, fd, skid, 0);
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
//MSG_TYPE_RECVFROM MSG_TYPE_TIMEOUT触发
void *syn_sendto(task_ctx *task, SOCKET fd, uint64_t skid,
    const char *ip, const uint16_t port, void *data, size_t len, size_t *size) {
    uint64_t sess = createid();
    ev_ud_sess(&task->srey->netev, fd, skid, sess);
    if (ERR_OK != ev_sendto(&task->srey->netev, fd, skid, ip, port, data, len)) {
        ev_ud_sess(&task->srey->netev, fd, skid, 0);
        return NULL;
    }
    _map_cosess_add(&task->coro->mapco, task->coro->curco, sess);
    _map_cotmo_add(&task->coro->mapco, TMO_TYPE_SESS, task->coro->curco, sess);
    srey_timeout(task, sess, NETRD_TIMEOUT);
    CO_YIELD(task);
    message_ctx msg;
    CO_POP(task->coro->curco, msg);
    if (sess != msg.sess) {
        _map_cosess_del(&task->coro->mapco, sess);
        _map_cotmo_del(&task->coro->mapco, sess);
        ev_ud_sess(&task->srey->netev, fd, skid, 0);
        LOG_FATAL("task: %d, request session: %"PRIu64", response session: %"PRIu64" not the same.",
            task->name, sess, msg.sess);
        return NULL;
    }
    if (MSG_TYPE_TIMEOUT == msg.mtype) {
        ev_ud_sess(&task->srey->netev, fd, skid, 0);
        LOG_WARN("task %d, sendto timeout.", task->name);
        return NULL;
    }
    *size = msg.size;
    return ((char *)msg.data) + sizeof(netaddr_ctx);
}
dns_ip *syn_dns_lookup(task_ctx *task, const char *dns, const char *domain, int32_t ipv6, size_t *cnt) {
    uint64_t skid;
    SOCKET fd;
    if (ERR_OK == is_ipv6(dns)) {
        fd = srey_udp(task, "::", 0, &skid);
    } else {
        fd = srey_udp(task, "0.0.0.0", 0, &skid);
    }
    if (INVALID_SOCK == fd) {
        return NULL;
    }
    char buf[ONEK] = { 0 };
    size_t lens = dns_request_pack(buf, domain, ipv6);
    void *resp = syn_sendto(task, fd, skid, dns, 53, buf, lens, &lens);
    ev_close(&task->srey->netev, fd, skid);
    if (NULL == resp) {
        return NULL;
    }
    return dns_parse_pack(resp, cnt);
}
//MSG_TYPE_HANDSHAKED MSG_TYPE_CLOSE MSG_TYPE_TIMEOUT触发
SOCKET syn_websock_connect(task_ctx *task, const char *host, uint16_t port, struct evssl_ctx *evssl, uint64_t *skid) {
    SOCKET fd = syn_connect(task, PACK_WEBSOCK, evssl, host, port, 0, skid);
    if (INVALID_SOCK == fd) {
        return INVALID_SOCK;
    }
    uint64_t sess = createid();
    char *wbreq = websock_handshake_pack(host);
    ev_ud_sess(&task->srey->netev, fd, *skid, sess);
    _map_cosess_add(&task->coro->mapco, task->coro->curco, sess);
    _map_cotmo_add(&task->coro->mapco, TMO_TYPE_SESS, task->coro->curco, sess);
    srey_timeout(task, sess, NETRD_TIMEOUT);
    ev_send(&task->srey->netev, fd, *skid, wbreq, strlen(wbreq), 0);
    CO_YIELD(task);
    message_ctx msg;
    CO_POP(task->coro->curco, msg);
    if (sess != msg.sess) {
        _map_cosess_del(&task->coro->mapco, sess);
        _map_cotmo_del(&task->coro->mapco, sess);
        ev_ud_sess(&task->srey->netev, fd, *skid, 0);
        ev_close(&task->srey->netev, fd, *skid);
        LOG_FATAL("task: %d, request session: %"PRIu64", response session: %"PRIu64" not the same.",
            task->name, sess, msg.sess);
        return INVALID_SOCK;
    }
    if (MSG_TYPE_TIMEOUT == msg.mtype) {
        ev_ud_sess(&task->srey->netev, fd, *skid, 0);
        ev_close(&task->srey->netev, fd, *skid);
        LOG_WARN("task %d, host %s:%d, handshake timeout.", task->name, host, port);
        return INVALID_SOCK;
    }
    if (MSG_TYPE_CLOSE == msg.mtype) {
        LOG_WARN("task %d, connection closed.", task->name);
        return INVALID_SOCK;
    }
    if (ERR_OK != msg.erro) {
        LOG_WARN("task %d, handshake error.", task->name);
        return INVALID_SOCK;
    }
    return fd;
}
static void syn_websock_continua(task_ctx *task, SOCKET fd, uint64_t skid, int32_t mask, websock_proto proto,
    send_chunck sendck, free_cb _sckdata_free, void *arg) {
    char *data;
    size_t size;
    data = sendck(&size, arg);
    if (NULL == data) {
        LOG_WARN("continua, but get NULL data at first pack.");
        return;
    }
    if (WBSK_TEXT == proto) {
        websock_text(&task->srey->netev, fd, skid, mask, 0, data, size);
    } else {
        websock_binary(&task->srey->netev, fd, skid, mask, 0, data, size);
    }
    if (NULL != _sckdata_free) {
        _sckdata_free(data);
    }
    for (;;) {
        data = sendck(&size, arg);
        if (NULL == data) {
            websock_continuation(&task->srey->netev, fd, skid, mask, 1, NULL, 0);
            break;
        } else {
            websock_continuation(&task->srey->netev, fd, skid, mask, 0, data, size);
            if (NULL != _sckdata_free) {
                _sckdata_free(data);
            }
            syn_sleep(task, 10);
        }
    }
}
void syn_websock_text(task_ctx *task, SOCKET fd, uint64_t skid, int32_t mask,
    send_chunck sendck, free_cb _sckdata_free, void *arg) {
    syn_websock_continua(task, fd, skid, mask, WBSK_TEXT, sendck, _sckdata_free, arg);
}
void syn_websock_binary(task_ctx *task, SOCKET fd, uint64_t skid, int32_t mask,
    send_chunck sendck, free_cb _sckdata_free, void *arg) {
    syn_websock_continua(task, fd, skid, mask, WBSK_BINARY, sendck, _sckdata_free, arg);
}
static struct http_pack_ctx *_http_send_content(task_ctx *task, SOCKET fd, uint64_t skid, uint64_t sess,
    int32_t req, buffer_ctx *buf) {
    char *data;
    size_t size =  buffer_size(buf);
    MALLOC(data, size);
    buffer_remove(buf, data, size);
    if (0 != req) {
        return syn_send(task, fd, skid, sess, data, size, &size, 0);
    } else {
        ev_send(&task->srey->netev, fd, skid, data, size, 0);
        return NULL;
    }
}
static struct http_pack_ctx *_http_send_chuncked(task_ctx *task, SOCKET fd, uint64_t skid, uint64_t sess,
    int32_t req, buffer_ctx *buf, send_chunck sendck, free_cb _sckdata_free, void *arg) {
    char *data;
    size_t size;
    for (;;) {
        data = sendck(&size, arg);
        if (NULL == data) {
            http_pack_chunked(buf, NULL, 0);
            size = buffer_size(buf);
            MALLOC(data, size);
            buffer_remove(buf, data, size);
            if (0 != req) {
                return syn_send(task, fd, skid, sess, data, size, &size, 0);
            } else {
                ev_send(&task->srey->netev, fd, skid, data, size, 0);
                return NULL;
            }
        } else {
            http_pack_chunked(buf, data, size);
            if (NULL != _sckdata_free) {
                _sckdata_free(data);
            }
            size = buffer_size(buf);
            MALLOC(data, size);
            buffer_remove(buf, data, size);
            ev_send(&task->srey->netev, fd, skid, data, size, 0);
            syn_sleep(task, 10);
        }
    }
    return NULL;
}
static struct http_pack_ctx *_syn_http_send(task_ctx *task, SOCKET fd, uint64_t skid, int32_t req, buffer_ctx *buf,
    send_chunck sendck, recv_chunck recvck, free_cb _sckdata_free, void *arg) {
    uint64_t sess;
    if (0 != req) {
        sess = createid();
    } else  {
        sess = 0;
    }
    struct http_pack_ctx *hpack;
    if (NULL == sendck) {
        hpack = _http_send_content(task, fd, skid, sess, req, buf);
    } else {
        hpack = _http_send_chuncked(task, fd, skid, sess, req, buf, sendck, _sckdata_free, arg);
    }
    if (NULL == hpack
        || NULL == recvck){
        return hpack;
    }
    char *data;
    size_t size;
    int32_t end = 0;
    for (;;) {
        data = syn_slice(task, fd, skid, sess, &size, &end);
        if (NULL == data
            && 1 != end) {
            return NULL;
        }
        data = http_data((struct http_pack_ctx *)data, &size);
        recvck((void *)data, size, end, arg);
        if (0 != end) {
            break;
        }
    }
    return hpack;
}
struct http_pack_ctx * http_get(task_ctx *task, SOCKET fd, uint64_t skid, buffer_ctx *buf,
    recv_chunck recvck, void *arg) {
    return _syn_http_send(task, fd, skid, 1, buf, NULL, recvck, NULL, arg);
}
struct http_pack_ctx *http_post(task_ctx *task, SOCKET fd, uint64_t skid, buffer_ctx *buf,
    send_chunck sendck, recv_chunck recvck, free_cb _sckdata_free, void *arg) {
    return _syn_http_send(task, fd, skid, 1, buf, sendck, recvck, _sckdata_free, arg);
}
void http_response(task_ctx *task, SOCKET fd, uint64_t skid, buffer_ctx *buf,
    send_chunck sendck, free_cb _sckdata_free, void *arg) {
    _syn_http_send(task, fd, skid, 0, buf, sendck, NULL, _sckdata_free, arg);
}

#endif
