#include "srey/task.h"
#include "containers/hashmap.h"

static void _map_task_set(struct hashmap *map, task_ctx *task) {
    name_t *key = &task->name;
    ASSERTAB(NULL == hashmap_set(map, &key), "task name repeat.");
}
static void *_map_task_del(struct hashmap *map, name_t name) {
    name_t *key = &name;
    return (void *)hashmap_delete(map, &key);
}
static task_ctx *_map_task_get(struct hashmap *map, name_t name) {
    name_t *key = &name;
    name_t **ptr = (name_t **)hashmap_get(map, &key);
    if (NULL == ptr) {
        return NULL;
    }
    return UPCAST(*ptr, task_ctx, name);
}
task_ctx *task_new(loader_ctx *loader, name_t name, _task_dispatch_cb _dispatch, free_cb _argfree, void *arg) {
    if (INVALID_TNAME == name) {
        return NULL;
    }
    task_ctx *task;
    CALLOC(task, 1, sizeof(task_ctx));
    task->loader = loader;
    task->name = name;
    task->ref = 1;
    if (NULL == _dispatch) {
        task->_task_dispatch = _message_dispatch;
    } else {
        task->_task_dispatch = _dispatch;
    }
    task->_arg_free = _argfree;
    task->arg = arg;
    _mcoro_new(task);
    spin_init(&task->lckmsg, SPIN_CNT_TASKMSG);
    qu_message_init(&task->qumsg, ONEK);
    task->overload = ONEK;
    return task;
}
void task_free(task_ctx *task) {
    if (NULL != task->_arg_free
        && NULL != task->arg) {
        task->_arg_free(task->arg);
    }
    message_ctx *msg;
    while (NULL != (msg = qu_message_pop(&task->qumsg))) {
        _message_clean(msg->mtype, msg->pktype, msg->data);
    }
    _mcoro_free(task);
    qu_message_free(&task->qumsg);
    spin_free(&task->lckmsg);
    FREE(task);
}
int32_t task_register(task_ctx *task, _task_startup_cb _startup, _task_closing_cb _closing) {
    task->_task_startup = _startup;
    task->_task_closing = _closing;
    message_ctx startup;
    startup.mtype = MSG_TYPE_STARTUP;
    rwlock_wrlock(&task->loader->lckmaptasks);
    if (NULL != _map_task_get(task->loader->maptasks, task->name)) {
        rwlock_unlock(&task->loader->lckmaptasks);
        LOG_ERROR("task name %d repeat.", task->name);
        return ERR_FAILED;
    }
    _map_task_set(task->loader->maptasks, task);
    _task_message_push(task, &startup);
    rwlock_unlock(&task->loader->lckmaptasks);
    return ERR_OK;
}
void task_close(task_ctx *task) {
    if (ATOMIC_CAS(&task->closing, 0, 1)) {
        message_ctx closing;
        closing.mtype = MSG_TYPE_CLOSING;
        _task_message_push(task, &closing);
    }
}
task_ctx *task_grab(loader_ctx *loader, name_t name) {
    if (INVALID_TNAME == name) {
        return NULL;
    }
    rwlock_rdlock(&loader->lckmaptasks);
    task_ctx *task = _map_task_get(loader->maptasks, name);
    if (NULL != task) {
        ATOMIC_ADD(&task->ref, 1);
    }
    rwlock_unlock(&loader->lckmaptasks);
    return task;
}
void task_incref(task_ctx *task) {
    ATOMIC_ADD(&task->ref, 1);
}
void task_ungrab(task_ctx *task) {
    if (1 != ATOMIC_ADD(&task->ref, -1)) {
        return;
    }
    void *ptr = NULL;
    rwlock_wrlock(&task->loader->lckmaptasks);
    if (0 == task->ref) {
        ptr = _map_task_del(task->loader->maptasks, task->name);
    }
    rwlock_unlock(&task->loader->lckmaptasks);
    if (NULL != ptr) {
        task_free(task);
    }
}
int32_t _message_should_clean(message_ctx *msg) {
    if ((MSG_TYPE_RECV == msg->mtype
        || MSG_TYPE_RECVFROM == msg->mtype
        || MSG_TYPE_REQUEST == msg->mtype
        || MSG_TYPE_RESPONSE == msg->mtype
        || MSG_TYPE_HANDSHAKED == msg->mtype)
        && NULL != msg->data) {
        return ERR_OK;
    }
    return ERR_FAILED;
}
void _message_clean(msg_type mtype, pack_type pktype, void *data) {
    switch (mtype) {
    case MSG_TYPE_RECV:
    case MSG_TYPE_RECVFROM:
        protos_pkfree(pktype, data);
        break;
    case MSG_TYPE_HANDSHAKED:
        protos_hsfree(pktype, data);
        break;
    case MSG_TYPE_REQUEST:
    case MSG_TYPE_RESPONSE:
        FREE(data);
        break;
    default:
        break;
    }
}
void _message_run(task_ctx *task, message_ctx *msg) {
    switch (msg->mtype) {
    case MSG_TYPE_STARTUP:
        if (NULL != task->_task_startup) {
            task->_task_startup(task);
        }
        break;
    case MSG_TYPE_CLOSING:
        if (NULL != task->_task_closing) {
            task->_task_closing(task);
        }
        task_ungrab(task);
        break;
    case MSG_TYPE_TIMEOUT:
        if (NULL != msg->data) {
            ((_timeout_cb)msg->data)(task, msg->sess);
        }
        break;
    case MSG_TYPE_ACCEPT:
        if (NULL != task->_net_accept) {
            task->_net_accept(task, msg->fd, msg->skid, msg->pktype);
        }
        break;
    case MSG_TYPE_CONNECT:
        if (NULL != task->_net_connect) {
            task->_net_connect(task, msg->fd, msg->skid, msg->pktype, msg->erro);
        }
        break;
    case MSG_TYPE_SSLEXCHANGED:
        if (NULL != task->_ssl_exchanged) {
            task->_ssl_exchanged(task, msg->fd, msg->skid, msg->pktype, msg->client);
        }
        break;
    case MSG_TYPE_HANDSHAKED:
        if (NULL != task->_net_handshaked) {
            task->_net_handshaked(task, msg->fd, msg->skid, msg->pktype, msg->client, msg->erro, msg->data, msg->size);
        }
        _message_clean(msg->mtype, msg->pktype, msg->data);
        break;
    case MSG_TYPE_RECV:
        if (NULL != task->_net_recv) {
            task->_net_recv(task, msg->fd, msg->skid, msg->pktype, msg->client, msg->slice, msg->data, msg->size);
        }
        _message_clean(msg->mtype, msg->pktype, msg->data);
        break;
    case MSG_TYPE_SEND:
        if (NULL != task->_net_send) {
            task->_net_send(task, msg->fd, msg->skid, msg->pktype, msg->client, msg->size);
        }
        break;
    case MSG_TYPE_CLOSE:
        if (NULL != task->_net_close) {
            task->_net_close(task, msg->fd, msg->skid, msg->pktype, msg->client);
        }
        break;
    case MSG_TYPE_RECVFROM:
        if (NULL != task->_net_recvfrom) {
            netaddr_ctx *addr = msg->data;
            char ip[IP_LENS];
            netaddr_ip(addr, ip);
            uint16_t port = netaddr_port(addr);
            char *data = ((char*)msg->data) + sizeof(netaddr_ctx);
            task->_net_recvfrom(task, msg->fd, msg->skid, ip, port, data, msg->size);
        }
        _message_clean(msg->mtype, msg->pktype, msg->data);
        break;
    case MSG_TYPE_REQUEST:
        if (NULL != task->_request) {
            task->_request(task, msg->pktype, msg->sess, msg->src, msg->data, msg->size);
        } else {
            task_ctx *dtask = task_grab(task->loader, msg->src);
            if (NULL != dtask) {
                const char *erro = "not register request function.";
                task_response(dtask, msg->sess, ERR_FAILED, (void *)erro, strlen(erro), 1);
                task_ungrab(dtask);
            }
        }
        _message_clean(msg->mtype, msg->pktype, msg->data);
        break;
    case MSG_TYPE_RESPONSE:
        if (NULL != task->_response) {
            task->_response(task, msg->sess, msg->erro, msg->data, msg->size);
        }
        _message_clean(msg->mtype, msg->pktype, msg->data);
        break;
    default:
        break;
    }
}
static void _message_timeout_push(ud_cxt *ud) {
    task_ctx *task = task_grab(ud->data, ud->name);
    if (NULL == task) {
        return;
    }
    message_ctx msg;
    msg.mtype = MSG_TYPE_TIMEOUT;
    msg.sess = ud->sess;
    msg.data = ud->extra;
    _task_message_push(task, &msg);
    task_ungrab(task);
}
void task_timeout(task_ctx *task, uint64_t sess, uint32_t ms, _timeout_cb _timeout) {
    ASSERTAB((NULL == _timeout && 0 != sess) || (NULL != _timeout && 0 == sess), "parameter error");
    ud_cxt ud;
    ud.name = task->name;
    ud.data = task->loader;
    ud.sess = sess;
    ud.extra = (void*)_timeout;
    tw_add(&task->loader->tw, ms, _message_timeout_push, NULL, &ud);
}
void task_request(task_ctx *dst, task_ctx *src, uint8_t reqtype, uint64_t sess, void *data, size_t size, int32_t copy) {
    ASSERTAB((NULL != src && 0 != sess) || (NULL == src && 0 == sess), "parameter error");
    message_ctx msg;
    msg.mtype = MSG_TYPE_REQUEST;
    msg.pktype = reqtype;
    if (NULL != src) {
        msg.src = src->name;
        msg.sess = sess;
    } else {
        msg.src = INVALID_TNAME;
        msg.sess = 0;
    }
    if (0 != copy) {
        MALLOC(msg.data, size);
        memcpy(msg.data, data, size);
    } else {
        msg.data = data;
    }
    msg.size = size;
    _task_message_push(dst, &msg);
}
void task_response(task_ctx *dst, uint64_t sess, int32_t erro, void *data, size_t size, int32_t copy) {
    message_ctx msg;
    msg.mtype = MSG_TYPE_RESPONSE;
    msg.sess = sess;
    msg.erro = erro;
    msg.size = size;
    if (NULL != data) {
        if (0 != copy) {
            MALLOC(msg.data, size);
            memcpy(msg.data, data, size);
        } else {
            msg.data = data;
        }
    } else {
        msg.data = NULL;
    }
    _task_message_push(dst, &msg);
}
void task_call(task_ctx *dst, uint8_t reqtype, void *data, size_t size, int32_t copy) {
    task_request(dst, NULL, reqtype, 0, data, size, copy);
}
static int32_t _net_accept(ev_ctx *ev, SOCKET fd, uint64_t skid, ud_cxt *ud) {
    task_ctx *task = task_grab(ud->data, ud->name);
    if (NULL == task) {
        return ERR_FAILED;
    }
    int32_t rtn = protos_connected(ev, fd, skid, ud);
    if (ERR_OK == rtn) {
        message_ctx msg;
        msg.mtype = MSG_TYPE_ACCEPT;
        msg.pktype = ud->pktype;
        msg.fd = fd;
        msg.skid = skid;
        _task_message_push(task, &msg);
    }
    task_ungrab(task);
    return rtn;
}
int32_t _message_handshaked_push(SOCKET fd, uint64_t skid, int32_t client, ud_cxt *ud, int32_t erro, void *data, size_t lens) {
    task_ctx *task = task_grab(ud->data, ud->name);
    if (NULL == task) {
        _message_clean(MSG_TYPE_HANDSHAKED, ud->pktype, data);
        return ERR_FAILED;
    }
    message_ctx msg;
    msg.mtype = MSG_TYPE_HANDSHAKED;
    msg.pktype = ud->pktype;
    msg.fd = fd;
    msg.skid = skid;
    msg.client = client;
    msg.erro = erro;
    msg.data = data;
    msg.size = lens;
    msg.sess = skid;
    _task_message_push(task, &msg);
    task_ungrab(task);
    return ERR_OK;
}
static void _net_recv(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client, buffer_ctx *buf, size_t size, ud_cxt *ud) {
    task_ctx *task = task_grab(ud->data, ud->name);
    if (NULL == task) {
        ev_close(ev, fd, skid);
        return;
    }
    message_ctx msg;
    msg.mtype = MSG_TYPE_RECV;
    msg.pktype = ud->pktype;
    msg.fd = fd;
    msg.skid = skid;
    msg.client = client;
    void *data;
    int32_t status;
    size_t esize;
    for (;;) {
        size = buffer_size(buf);
        data = protos_unpack(ev, fd, skid, client, buf, ud, &msg.size, &status);
        if (NULL != data) {
            msg.data = data;
            msg.sess = ud->sess;
            ud->sess = 0;
            if (BIT_CHECK(status, PROTO_SLICE_START)) {
                msg.slice = PROTO_SLICE_START;
            } else if(BIT_CHECK(status, PROTO_SLICE)) {
                msg.slice = PROTO_SLICE;
            } else if(BIT_CHECK(status, PROTO_SLICE_END)) {
                msg.slice = PROTO_SLICE_END;
            } else {
                msg.slice = 0;
            }
            _task_message_push(task, &msg);
        }
        if (BIT_CHECK(status, PROTO_ERROR)
            || BIT_CHECK(status, PROTO_CLOSE)) {
            ev_close(ev, fd, skid);
            break;
        }
        esize = buffer_size(buf);
        if (0 == esize
            || size == esize
            || BIT_CHECK(status, PROTO_MOREDATA)) {
            break;
        }
    }
    task_ungrab(task);
}
static void _net_send(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client, size_t size, ud_cxt *ud) {
    task_ctx *task = task_grab(ud->data, ud->name);
    if (NULL == task) {
        ev_close(ev, fd, skid);
        return;
    }
    message_ctx msg;
    msg.mtype = MSG_TYPE_SEND;
    msg.pktype = ud->pktype;
    msg.fd = fd;
    msg.skid = skid;
    msg.client = client;
    msg.size = size;
    _task_message_push(task, &msg);
    task_ungrab(task);
}
static void _net_ssl_exchanged(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client, ud_cxt *ud) {
    task_ctx *task = task_grab(ud->data, ud->name);
    if (NULL == task) {
        ev_close(ev, fd, skid);
        return;
    }
    if (ERR_OK == protos_ssl_exchanged(ev, fd, skid, client, ud)) {
        message_ctx msg;
        msg.mtype = MSG_TYPE_SSLEXCHANGED;
        msg.pktype = ud->pktype;
        msg.fd = fd;
        msg.skid = skid;
        msg.client = client;
        msg.sess = skid;
        _task_message_push(task, &msg);
    } else {
        ev_close(ev, fd, skid);
    }
    task_ungrab(task);
}
static void _net_close(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client, ud_cxt *ud) {
    task_ctx *task = task_grab(ud->data, ud->name);
    if (NULL == task) {
        return;
    }
    message_ctx msg;
    msg.mtype = MSG_TYPE_CLOSE;
    msg.pktype = ud->pktype;
    msg.fd = fd;
    msg.skid = skid;
    msg.client = client;
    msg.sess = skid;
    protos_closed(ud);
    _task_message_push(task, &msg);
    task_ungrab(task);
}
int32_t task_listen(task_ctx *task, pack_type pktype, struct evssl_ctx *evssl,
    const char *ip, uint16_t port, uint64_t *id, int32_t netev) {
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.pktype = pktype;
    ud.name = task->name;
    ud.data = task->loader;
    cbs_ctx cbs;
    ZERO(&cbs, sizeof(cbs));
    if (BIT_CHECK(netev, NETEV_ACCEPT)) {
        cbs.acp_cb = _net_accept;
    }
    if (BIT_CHECK(netev, NETEV_SEND)) {
        cbs.s_cb = _net_send;
    }
    if (NULL != evssl
        || BIT_CHECK(netev, NETEV_AUTHSSL)) {
        cbs.exch_cb = _net_ssl_exchanged;
    }
    cbs.r_cb = _net_recv;
    cbs.c_cb = _net_close;
    cbs.ud_free = protos_udfree;
    return ev_listen(&task->loader->netev, evssl, ip, port, &cbs, &ud, id);
}
static int32_t _net_connect(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t err, ud_cxt *ud) {
    task_ctx *task = task_grab(ud->data, ud->name);
    if (NULL == task) {
        return ERR_FAILED;
    }
    message_ctx msg;
    msg.mtype = MSG_TYPE_CONNECT;
    msg.pktype = ud->pktype;
    msg.fd = fd;
    msg.skid = skid;
    msg.erro = err;
    msg.sess = skid;
    _task_message_push(task, &msg);
    task_ungrab(task);
    return ERR_OK;
}
SOCKET task_connect(task_ctx *task, pack_type pktype, struct evssl_ctx *evssl,
    const char *ip, uint16_t port, uint64_t *skid, int32_t netev, void *extra) {
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.pktype = pktype;
    ud.name = task->name;
    ud.data = task->loader;
    ud.extra = extra;
    cbs_ctx cbs;
    ZERO(&cbs, sizeof(cbs));
    if (BIT_CHECK(netev, NETEV_SEND)) {
        cbs.s_cb = _net_send;
    }
    if (BIT_CHECK(netev, NETEV_AUTHSSL)) {
        cbs.exch_cb = _net_ssl_exchanged;
    }
    cbs.conn_cb = _net_connect;
    cbs.r_cb = _net_recv;
    cbs.c_cb = _net_close;
    cbs.ud_free = protos_udfree;
    return ev_connect(&task->loader->netev, evssl, ip, port, &cbs, &ud, skid);
}
static void _net_recvfrom(ev_ctx *ev, SOCKET fd, uint64_t skid, char *buf, size_t size, netaddr_ctx *addr, ud_cxt *ud) {
    task_ctx *task = task_grab(ud->data, ud->name);
    if (NULL == task) {
        ev_close(ev, fd, skid);
        return;
    }
    message_ctx msg;
    msg.mtype = MSG_TYPE_RECVFROM;
    msg.fd = fd;
    msg.skid = skid;
    char *umsg;
    MALLOC(umsg, sizeof(netaddr_ctx) + size);
    memcpy(umsg, addr, sizeof(netaddr_ctx));
    memcpy(umsg + sizeof(netaddr_ctx), buf, size);
    msg.data = umsg;
    msg.size = size;
    msg.sess = ud->sess;
    ud->sess = 0;
    _task_message_push(task, &msg);
    task_ungrab(task);
}
SOCKET task_udp(task_ctx *task, const char *ip, uint16_t port, uint64_t *skid) {
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.name = task->name;
    ud.data = task->loader;
    cbs_ctx cbs;
    ZERO(&cbs, sizeof(cbs));
    cbs.rf_cb = _net_recvfrom;
    cbs.ud_free = protos_udfree;
    return ev_udp(&task->loader->netev, ip, port, &cbs, &ud, skid);
}
void on_accepted(task_ctx *task, _net_accept_cb _accept) {
    task->_net_accept = _accept;
}
void on_recved(task_ctx *task, _net_recv_cb _recv) {
    task->_net_recv = _recv;
}
void on_sended(task_ctx *task, _net_send_cb _send) {
    task->_net_send = _send;
}
void on_connected(task_ctx *task, _net_connect_cb _connect) {
    task->_net_connect = _connect;
}
void on_ssl_exchanged(task_ctx *task, _net_ssl_exchanged_cb _exchanged) {
    task->_ssl_exchanged = _exchanged;
}
void on_handshaked(task_ctx *task, _net_handshake_cb _handshake) {
    task->_net_handshaked = _handshake;
}
void on_closed(task_ctx *task, _net_close_cb _close) {
    task->_net_close = _close;
}
void on_recvedfrom(task_ctx *task, _net_recvfrom_cb _recvfrom) {
    task->_net_recvfrom = _recvfrom;
}
void on_requested(task_ctx *task, _request_cb _request) {
    task->_request = _request;
}
void on_responsed(task_ctx *task, _response_cb _response) {
    task->_response = _response;
}
