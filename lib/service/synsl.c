#include "service/synsl.h"
#if WITH_CORO
#include "service/srey.h"
#define MINICORO_IMPL
#include "minicoro/minicoro.h"
#include "proto/dns.h"
#include "proto/websock.h"
#include "proto/http.h"
#include "hashmap.h"
#include "buffer.h"

typedef enum timeout_type {
    TMO_TYPE_NONE = 0x00,
    TMO_TYPE_NORMAL,
    TMO_TYPE_WAIT
}timeout_type;
typedef struct coro_sess {
    timeout_type type;
    mco_coro *co;
    uint64_t sess;
    uint64_t assoc;
}coro_sess;
typedef struct coro_ctx {
    mco_coro *curco;
    struct hashmap *mapco;
    qu_ptr qucopool;
}coro_ctx;
static mco_desc _coro_desc;

#define COROPOOL_KEEP         8
#define COROPOOL_NDEL         3
#define CONNECT_TIMEOUT       3000
#define NETRD_TIMEOUT         1500
#define REQUEST_TIMEOUT       1000
#define CO_RESUME(arg, co) \
    arg->task->coro->curco = co; \
    mco_result _cortn = mco_push(co, &arg->msg, sizeof(message_ctx)); \
    ASSERTAB(MCO_SUCCESS == _cortn, mco_result_description(_cortn)); \
    _cortn = mco_resume(co); \
    ASSERTAB(MCO_SUCCESS == _cortn, mco_result_description(_cortn))

static inline uint64_t _map_cosess_hash(const void *item, uint64_t seed0, uint64_t seed1) {
    return hash((const char *)&(((coro_sess *)item)->sess), sizeof(((coro_sess *)item)->sess));
}
static inline int _map_cosess_compare(const void *a, const void *b, void *ud) {
    return (int)(((coro_sess *)a)->sess - ((coro_sess *)b)->sess);
}
static inline void _map_cosess_set(task_ctx *task, mco_coro *co, uint64_t sess, uint64_t assoc, timeout_type type) {
    coro_sess cosess;
    cosess.type = type;
    cosess.co = co;
    cosess.sess = sess;
    cosess.assoc = assoc;
    hashmap_set(task->coro->mapco, &cosess);
}
static inline void _map_cosess_del(task_ctx *task, uint64_t sess) {
    coro_sess key;
    key.sess = sess;
    hashmap_delete(task->coro->mapco, &key);
}
static inline int32_t _map_cosess_get(task_ctx *task, uint64_t sess, coro_sess *cosess) {
    coro_sess key;
    key.sess = sess;
    coro_sess *tmp = (coro_sess *)hashmap_get(task->coro->mapco, &key);
    if (NULL == tmp) {
        return ERR_FAILED;
    }
    *cosess = *tmp;
    if (TMO_TYPE_NONE != cosess->type) {
        hashmap_delete(task->coro->mapco, &key);
    }
    return  ERR_OK;
}
static void _co_cb(mco_coro *co) {
    task_msg_arg arg;
    mco_result cortn;
    for (;;) {
        cortn = mco_yield(co);
        ASSERTAB(MCO_SUCCESS == cortn, mco_result_description(cortn));
        cortn = mco_pop(co, &arg, sizeof(arg));
        ASSERTAB(MCO_SUCCESS == cortn, mco_result_description(cortn));
        srey_task_incref(arg.task);//保证task->_run里YIELD后不会被释放
        arg.task->_run[arg.msg.mtype](arg.task, &arg.msg);
        message_clean(arg.task, arg.msg.mtype, arg.msg.pktype, arg.msg.data);
        qu_ptr_push(&arg.task->coro->qucopool, (void **)&co);
        //_loop_worker 有一次grab所以这里的ungrab都不会释放
        srey_task_ungrab(arg.task);
        if (MSG_TYPE_CLOSING == arg.msg.mtype) {
            srey_task_ungrab(arg.task);
        }
    }
}
void _coro_init_desc(size_t stack_size) {
    _coro_desc = mco_desc_init(_co_cb, stack_size);
}
coro_ctx *_coro_new(void) {
    coro_ctx *coctx;
    CALLOC(coctx, 1, sizeof(coro_ctx));
    qu_ptr_init(&coctx->qucopool, 0);
    coctx->mapco = hashmap_new_with_allocator(_malloc, _realloc, _free,
                                              sizeof(coro_sess), ONEK, 0, 0,
                                              _map_cosess_hash, _map_cosess_compare, NULL, NULL);
    return coctx;
}
void _coro_free(coro_ctx *coctx) {
    if (NULL == coctx) {
        return;
    }
    mco_coro **co;
    mco_result cortn;
    while (NULL != (co = (mco_coro **)qu_ptr_pop(&coctx->qucopool))) {
        cortn = mco_destroy(*co);
        if (MCO_SUCCESS != cortn) {
            LOG_WARN("%s", mco_result_description(cortn));
        }
    }
    qu_ptr_free(&coctx->qucopool);
    hashmap_free(coctx->mapco);
    FREE(coctx);
}
static inline mco_coro *_co_pool_get(task_ctx *task) {
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
static inline void _co_create(task_msg_arg *arg) {
    if (NULL != arg->task->_run[arg->msg.mtype]) {
        arg->task->coro->curco = _co_pool_get(arg->task);
        mco_result cortn = mco_push(arg->task->coro->curco, arg, sizeof(task_msg_arg));
        ASSERTAB(MCO_SUCCESS == cortn, mco_result_description(cortn));
        cortn = mco_resume(arg->task->coro->curco);
        ASSERTAB(MCO_SUCCESS == cortn, mco_result_description(cortn));
    } else {
        message_clean(arg->task, arg->msg.mtype, arg->msg.pktype, arg->msg.data);
        if (MSG_TYPE_CLOSING == arg->msg.mtype) {
            srey_task_ungrab(arg->task);
        }
    }
}
static inline void _dispatch_timeout(task_msg_arg *arg) {
    coro_sess cosess;
    if (ERR_OK != _map_cosess_get(arg->task, arg->msg.sess, &cosess)) {
        message_clean(arg->task, arg->msg.mtype, arg->msg.pktype, arg->msg.data);
        return;
    }
    switch (cosess.type) {
    case TMO_TYPE_NORMAL:
        _co_create(arg);
        break;
    case TMO_TYPE_WAIT:
        if (0 != cosess.assoc) {
            _map_cosess_del(arg->task, cosess.assoc);
        }
        CO_RESUME(arg, cosess.co);
        break;
    default:
        break;
    }
}
static inline void _dispatch_connect(task_msg_arg *arg) {
    if (0 != arg->msg.sess) {
        coro_sess cosess;
        if (ERR_OK == _map_cosess_get(arg->task, arg->msg.sess, &cosess)) {
            CO_RESUME(arg, cosess.co);
        }
    } else {
        _co_create(arg);
    }
}
static inline void _dispatch_handshaked(task_msg_arg *arg) {
    if (0 != arg->msg.sess) {
        coro_sess cosess;
        if (ERR_OK == _map_cosess_get(arg->task, arg->msg.sess, &cosess)) {
            CO_RESUME(arg, cosess.co);
        }
    } else {
        _co_create(arg);
    }
}
static inline void _dispatch_netrd(task_msg_arg *arg) {
    if (0 != arg->msg.sess) {
        coro_sess cosess;
        if (ERR_OK == _map_cosess_get(arg->task, arg->msg.sess, &cosess)) {
            if (SLICE_START == arg->msg.slice) {
                _map_cosess_set(arg->task, cosess.co, cosess.sess, 0, TMO_TYPE_NONE);
            } else if (SLICE_END == arg->msg.slice) {
                _map_cosess_del(arg->task, arg->msg.sess);
            }
            CO_RESUME(arg, cosess.co);
        }
        message_clean(arg->task, arg->msg.mtype, arg->msg.pktype, arg->msg.data);
    } else {
        _co_create(arg);
    }
}
static inline void _dispatch_close(task_msg_arg *arg) {
    if (0 != arg->msg.sess) {
        coro_sess cosess;
        if (ERR_OK == _map_cosess_get(arg->task, arg->msg.sess, &cosess)) {
            if (TMO_TYPE_NONE == cosess.type) {
                _map_cosess_del(arg->task, arg->msg.sess);
            }
            CO_RESUME(arg, cosess.co);
        }
    }
    _co_create(arg);
}
static inline void _dispatch_response(task_msg_arg *arg) {
    coro_sess cosess;
    if (ERR_OK == _map_cosess_get(arg->task, arg->msg.sess, &cosess)) {
        CO_RESUME(arg, cosess.co);
    } else {
        LOG_ERROR("task: %d, can't find session:%"PRIu64, arg->task->name, arg->msg.sess);
    }
    message_clean(arg->task, arg->msg.mtype, arg->msg.pktype, arg->msg.data);
}
void _dispatch_coro(task_msg_arg *arg) {
    switch (arg->msg.mtype) {
    case MSG_TYPE_STARTUP:
        _co_create(arg);
        break;
    case MSG_TYPE_CLOSING:
        _co_create(arg);
        break;
    case MSG_TYPE_TIMEOUT:
        _dispatch_timeout(arg);
        break;
    case MSG_TYPE_ACCEPT:
        _co_create(arg);
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
        _co_create(arg);
        break;
    case MSG_TYPE_CLOSE:
        _dispatch_close(arg);
        break;
    case MSG_TYPE_RECVFROM:
        _dispatch_netrd(arg);
        break;
    case MSG_TYPE_REQUEST:
        _co_create(arg);
        break;
    case MSG_TYPE_RESPONSE:
        _dispatch_response(arg);
        break;
    default:
        break;
    }
}
static inline void _wait_until(task_ctx *task, uint32_t ms, uint64_t sess, uint64_t assoc, message_ctx *msg) {
    _map_cosess_set(task, task->coro->curco, sess, assoc, TMO_TYPE_WAIT);
    srey_timeout(task, sess, ms, NULL, NULL, NULL);
    mco_result cortn = mco_yield(task->coro->curco);
    ASSERTAB(MCO_SUCCESS == cortn, mco_result_description(cortn));
    cortn = mco_pop(task->coro->curco, msg, sizeof(message_ctx));
    ASSERTAB(MCO_SUCCESS == cortn, mco_result_description(cortn));
    ASSERTAB(0 != msg->sess && (sess == msg->sess || assoc == msg->sess), "different session.");
}
//MSG_TYPE_TIMEOUT触发
void syn_sleep(task_ctx *task, uint32_t ms) {
    message_ctx msg;
    _wait_until(task, ms, createid(), 0, &msg);
}
//MSG_TYPE_TIMEOUT触发
void syn_timeout(task_ctx *task, uint32_t ms, ctask_timeout _timeout, free_cb _argfree, void *arg) {
    uint64_t sess = createid();
    _map_cosess_set(task, task->coro->curco, sess, 0, TMO_TYPE_NORMAL);
    srey_timeout(task, sess, ms, _timeout, _argfree, arg);
}
//MSG_TYPE_RESPONSE触发
void *syn_request(task_ctx *dst, task_ctx *src, void *data, size_t size, int32_t copy, int32_t *erro, size_t *lens) {
    uint64_t sess = createid();
    srey_request(dst, src, sess, data, size, copy);
    message_ctx msg;
    _wait_until(src, REQUEST_TIMEOUT, sess, 0, &msg);
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
    message_ctx msg;
    uint64_t slice_sess = createid();
    _wait_until(task, NETRD_TIMEOUT, slice_sess, sess, &msg);
    if (MSG_TYPE_TIMEOUT == msg.mtype) {
        ev_ud_sess(&task->srey->netev, fd, skid, 0);
        ev_close(&task->srey->netev, fd, skid);
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
//MSG_TYPE_CONNECT MSG_TYPE_TIMEOUT触发
SOCKET syn_connect(task_ctx *task, pack_type pktype, struct evssl_ctx *evssl,
    const char *ip, uint16_t port, int32_t sendev, uint64_t *skid) {
    uint64_t sess = createid();
    SOCKET fd = srey_connect(task, sess, pktype, evssl, ip, port, sendev, skid);
    if (INVALID_SOCK == fd) {
        return INVALID_SOCK;
    }
    message_ctx msg;
    _wait_until(task, CONNECT_TIMEOUT, sess, 0, &msg);
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
    ev_send(&task->srey->netev, fd, skid, data, len, copy);
    message_ctx msg;
    _wait_until(task, NETRD_TIMEOUT, sess, 0, &msg);
    if (MSG_TYPE_TIMEOUT == msg.mtype) {
        ev_ud_sess(&task->srey->netev, fd, skid, 0);
        ev_close(&task->srey->netev, fd, skid);
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
    message_ctx msg;
    _wait_until(task, NETRD_TIMEOUT, sess, 0, &msg);
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
    ev_send(&task->srey->netev, fd, *skid, wbreq, strlen(wbreq), 0);
    message_ctx msg;
    _wait_until(task, NETRD_TIMEOUT, sess, 0, &msg);
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
        hpack = syn_slice(task, fd, skid, sess, &size, &end);
        if (NULL == hpack
            && 1 != end) {
            return NULL;
        }
        data = http_data(hpack, &size);
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
