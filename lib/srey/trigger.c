#include "srey/trigger.h"
#include "srey/task.h"

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
        if (NULL != task->_auth_ssl) {
            task->_auth_ssl(task, msg->fd, msg->skid, msg->pktype, msg->client);
        }
        break;
    case MSG_TYPE_HANDSHAKED:
        if (NULL != task->_net_handshaked) {
            task->_net_handshaked(task, msg->fd, msg->skid, msg->pktype, msg->client, msg->erro);
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
            task_ctx *dtask = task_grab(task->scheduler, msg->src);
            if (NULL != dtask) {
                const char *erro = "not register request function.";
                trigger_response(dtask, msg->sess, ERR_FAILED, (void *)erro, strlen(erro), 1);
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
#if !WITH_CORO
void _message_dispatch(task_dispatch_arg *arg) {
    _message_run(arg->task, &arg->msg);
}
#endif
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
void trigger_timeout(task_ctx *task, uint64_t sess, uint32_t ms, _timeout_cb _timeout) {
    ASSERTAB((NULL == _timeout && 0 != sess) || (NULL != _timeout && 0 == sess), "parameter error");
    ud_cxt ud;
    ud.name = task->name;
    ud.data = task->scheduler;
    ud.sess = sess;
    ud.extra = (void*)_timeout;
    tw_add(&task->scheduler->tw, ms, _message_timeout_push, NULL, &ud);
}
void trigger_request(task_ctx *dst, task_ctx *src, uint8_t reqtype, uint64_t sess, void *data, size_t size, int32_t copy) {
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
void trigger_response(task_ctx *dst, uint64_t sess, int32_t erro, void *data, size_t size, int32_t copy) {
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
void trigger_call(task_ctx *dst, uint8_t reqtype, void *data, size_t size, int32_t copy) {
    trigger_request(dst, NULL, reqtype, 0, data, size, copy);
}
static int32_t _net_accept(ev_ctx *ev, SOCKET fd, uint64_t skid, ud_cxt *ud) {
    task_ctx *task = task_grab(ud->data, ud->name);
    if (NULL == task) {
        return ERR_FAILED;
    }
    message_ctx msg;
    msg.mtype = MSG_TYPE_ACCEPT;
    msg.pktype = ud->pktype;
    msg.fd = fd;
    msg.skid = skid;
    _task_message_push(task, &msg);
    task_ungrab(task);
    return ERR_OK;
}
int32_t _message_handshaked_push(SOCKET fd, uint64_t skid, int32_t client, ud_cxt *ud, int32_t erro, void *data, size_t lens) {
    task_ctx *task = task_grab(ud->data, ud->name);
    if (NULL == task) {
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
        if (BIT_CHECK(status, PROTO_ERROR)) {
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
    message_ctx msg;
    msg.mtype = MSG_TYPE_SSLEXCHANGED;
    msg.pktype = ud->pktype;
    msg.fd = fd;
    msg.skid = skid;
    msg.client = client;
    msg.sess = skid;
    _task_message_push(task, &msg);
    if (ERR_OK != protos_ssl_exchanged(ev, fd, skid, client, ud)) {
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
int32_t trigger_listen(task_ctx *task, pack_type pktype, struct evssl_ctx *evssl,
    const char *ip, uint16_t port, uint64_t *id, int32_t netev) {
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.pktype = pktype;
    ud.name = task->name;
    ud.data = task->scheduler;
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
    return ev_listen(&task->scheduler->netev, evssl, ip, port, &cbs, &ud, id);
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
SOCKET trigger_connect(task_ctx *task, pack_type pktype, struct evssl_ctx *evssl,
    const char *ip, uint16_t port, uint64_t *skid, int32_t netev) {
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.pktype = pktype;
    ud.name = task->name;
    ud.data = task->scheduler;
    cbs_ctx cbs;
    ZERO(&cbs, sizeof(cbs));
    if (BIT_CHECK(netev, NETEV_SEND)) {
        cbs.s_cb = _net_send;
    }
    if (NULL != evssl
        || BIT_CHECK(netev, NETEV_AUTHSSL)) {
        cbs.exch_cb = _net_ssl_exchanged;
    }
    cbs.conn_cb = _net_connect;
    cbs.r_cb = _net_recv;
    cbs.c_cb = _net_close;
    cbs.ud_free = protos_udfree;
    return ev_connect(&task->scheduler->netev, evssl, ip, port, &cbs, &ud, skid);
}
SOCKET trigger_conn_extra(task_ctx *task, pack_type pktype, void *extra,
    const char *ip, uint16_t port, uint64_t *skid, int32_t netev) {
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.pktype = pktype;
    ud.name = task->name;
    ud.data = task->scheduler;
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
    return ev_connect(&task->scheduler->netev, NULL, ip, port, &cbs, &ud, skid);
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
SOCKET trigger_udp(task_ctx *task, const char *ip, uint16_t port, uint64_t *skid) {
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.name = task->name;
    ud.data = task->scheduler;
    cbs_ctx cbs;
    ZERO(&cbs, sizeof(cbs));
    cbs.rf_cb = _net_recvfrom;
    cbs.ud_free = protos_udfree;
    return ev_udp(&task->scheduler->netev, ip, port, &cbs, &ud, skid);
}
