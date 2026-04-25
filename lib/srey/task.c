#include "srey/task.h"
#include "containers/hashmap.h"

// 将任务名指针插入任务哈希表（重复时触发断言）
static void _map_task_set(struct hashmap *map, task_ctx *task) {
    name_t *key = &task->name;
    ASSERTAB(NULL == hashmap_set(map, &key), "task name repeat.");
}
// 从任务哈希表中删除指定任务名，返回被删除的元素指针
static void *_map_task_del(struct hashmap *map, name_t name) {
    name_t *key = &name;
    return (void *)hashmap_delete(map, &key);
}
// 按任务名从哈希表中查找并返回 task_ctx，未找到返回 NULL
static task_ctx *_map_task_get(struct hashmap *map, name_t name) {
    name_t *key = &name;
    name_t **ptr = (name_t **)hashmap_get(map, &key);
    if (NULL == ptr) {
        return NULL;
    }
    return UPCAST(*ptr, task_ctx, name);
}
// 处理启动消息：调用任务的 _task_startup 回调
static void _handle_startup(task_ctx *task, message_ctx *msg) {
    (void)msg;
    if (NULL != task->_task_startup) {
        task->_task_startup(task);
    }
}
// 处理关闭消息：调用 _task_closing 回调并释放任务引用
static void _handle_closing(task_ctx *task, message_ctx *msg) {
    (void)msg;
    if (NULL != task->_task_closing) {
        task->_task_closing(task);
    }
    task_ungrab(task);
}
// 处理超时消息：调用消息中携带的 _timeout_cb 回调函数
static void _handle_timeout(task_ctx *task, message_ctx *msg) {
    if (NULL != msg->data) {
        ((_timeout_cb)msg->data)(task, msg->sess);
    }
}
// 处理新连接接受消息
static void _handle_accept(task_ctx *task, message_ctx *msg) {
    if (NULL != task->_net_accept) {
        task->_net_accept(task, msg->fd, msg->skid, msg->pktype);
    }
}
// 处理连接建立消息
static void _handle_connect(task_ctx *task, message_ctx *msg) {
    if (NULL != task->_net_connect) {
        task->_net_connect(task, msg->fd, msg->skid, msg->pktype, msg->erro);
    }
}
// 处理 SSL 交换完成消息
static void _handle_ssl_exchanged(task_ctx *task, message_ctx *msg) {
    if (NULL != task->_ssl_exchanged) {
        task->_ssl_exchanged(task, msg->fd, msg->skid, msg->pktype, msg->client);
    }
}
// 处理应用层握手完成消息，处理后清理消息数据
static void _handle_handshaked(task_ctx *task, message_ctx *msg) {
    if (NULL != task->_net_handshaked) {
        task->_net_handshaked(task, msg->fd, msg->skid, msg->pktype, msg->client, msg->erro, msg->data, msg->size);
    }
    _message_clean(msg->mtype, msg->pktype, msg->data);
}
// 处理 TCP 数据接收消息，处理后清理消息数据
static void _handle_recv(task_ctx *task, message_ctx *msg) {
    if (NULL != task->_net_recv) {
        task->_net_recv(task, msg->fd, msg->skid, msg->pktype, msg->client, msg->slice, msg->data, msg->size);
    }
    _message_clean(msg->mtype, msg->pktype, msg->data);
}
// 处理数据发送完成消息
static void _handle_send(task_ctx *task, message_ctx *msg) {
    if (NULL != task->_net_send) {
        task->_net_send(task, msg->fd, msg->skid, msg->pktype, msg->client, msg->size);
    }
}
// 处理连接关闭消息
static void _handle_close(task_ctx *task, message_ctx *msg) {
    if (NULL != task->_net_close) {
        task->_net_close(task, msg->fd, msg->skid, msg->pktype, msg->client);
    }
}
// 处理 UDP 数据接收消息：从消息数据中解析出地址和载荷，处理后清理
static void _handle_recvfrom(task_ctx *task, message_ctx *msg) {
    if (NULL != task->_net_recvfrom) {
        netaddr_ctx *addr = msg->data;
        char ip[IP_LENS];
        netaddr_ip(addr, ip);
        uint16_t port = netaddr_port(addr);
        char *data = ((char*)msg->data) + sizeof(netaddr_ctx);
        task->_net_recvfrom(task, msg->fd, msg->skid, ip, port, data, msg->size);
    }
    _message_clean(msg->mtype, msg->pktype, msg->data);
}
// 处理任务间请求消息：若未注册请求回调，则返回错误响应
static void _handle_request(task_ctx *task, message_ctx *msg) {
    if (NULL != task->_request) {
        task->_request(task, msg->pktype, msg->sess, msg->src, msg->data, msg->size);
    } else {
        task_ctx *dtask = task_grab(task->loader, msg->src);
        if (NULL != dtask) {
            const char *erro = "not register request callback function.";
            task_response(dtask, msg->sess, ERR_FAILED, (void *)erro, strlen(erro), 1);
            task_ungrab(dtask);
        }
    }
    _message_clean(msg->mtype, msg->pktype, msg->data);
}
// 处理任务间响应消息，处理后清理消息数据
static void _handle_response(task_ctx *task, message_ctx *msg) {
    if (NULL != task->_response) {
        task->_response(task, msg->sess, msg->erro, msg->data, msg->size);
    }
    _message_clean(msg->mtype, msg->pktype, msg->data);
}
typedef void (*_msg_handler_t)(task_ctx *, message_ctx *);
// 按消息类型索引的处理函数表（静态分发，无 switch-case 开销）
static const _msg_handler_t _msg_handlers[MSG_TYPE_ALL] = {
    [MSG_TYPE_STARTUP]      = _handle_startup,
    [MSG_TYPE_CLOSING]      = _handle_closing,
    [MSG_TYPE_TIMEOUT]      = _handle_timeout,
    [MSG_TYPE_ACCEPT]       = _handle_accept,
    [MSG_TYPE_CONNECT]      = _handle_connect,
    [MSG_TYPE_SSLEXCHANGED] = _handle_ssl_exchanged,
    [MSG_TYPE_HANDSHAKED]   = _handle_handshaked,
    [MSG_TYPE_RECV]         = _handle_recv,
    [MSG_TYPE_SEND]         = _handle_send,
    [MSG_TYPE_CLOSE]        = _handle_close,
    [MSG_TYPE_RECVFROM]     = _handle_recvfrom,
    [MSG_TYPE_REQUEST]      = _handle_request,
    [MSG_TYPE_RESPONSE]     = _handle_response,
};
void _message_run(task_ctx *task, message_ctx *msg) {
    if (msg->mtype > MSG_TYPE_NONE
        && msg->mtype < MSG_TYPE_ALL
        && NULL != _msg_handlers[msg->mtype]) {
        _msg_handlers[msg->mtype](task, msg);
    }
}
// 默认消息分发函数（直接调用 _message_run）
static void _message_dispatch(task_dispatch_arg *arg) {
    _message_run(arg->task, &arg->msg);
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
    task->timeout_request = 3 * 1000;
    task->timeout_connect = 5 * 1000;
    task->timeout_netread = 10 * 1000;
    if (NULL == _dispatch) {
        task->_task_dispatch = _message_dispatch;
    } else {
        task->_task_dispatch = _dispatch;
    }
    task->_arg_free = _argfree;
    task->arg = arg;
    mspc_init(&task->qumsg, ONEK);
    task->overload = ONEK;
    return task;
}
void task_free(task_ctx *task) {
    if (NULL != task->_arg_free
        && NULL != task->arg) {
        task->_arg_free(task->arg);
    }
    message_ctx *msg;
    while (NULL != (msg = (message_ctx *)mspc_pop(&task->qumsg))) {
        _message_clean(msg->mtype, msg->pktype, msg->data);
        FREE(msg);
    }
    mspc_free(&task->qumsg);
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
        prots_pkfree(pktype, data);
        break;
    case MSG_TYPE_HANDSHAKED:
        prots_hsfree(pktype, data);
        break;
    case MSG_TYPE_REQUEST:
    case MSG_TYPE_RESPONSE:
        FREE(data);
        break;
    default:
        break;
    }
}
// 时间轮超时回调：将超时消息推入对应任务队列
static void _message_timeout_push(ud_cxt *ud) {
    task_ctx *task = task_grab(ud->loader, ud->name);
    if (NULL == task) {
        return;
    }
    message_ctx msg;
    msg.mtype = MSG_TYPE_TIMEOUT;
    msg.sess = ud->sess;
    msg.data = ud->context;
    _task_message_push(task, &msg);
    task_ungrab(task);
}
void task_timeout(task_ctx *task, uint64_t sess, uint32_t ms, _timeout_cb _timeout) {
    ASSERTAB((NULL == _timeout && 0 != sess) || (NULL != _timeout && 0 == sess), "parameter error");
    ud_cxt ud;
    ud.name = task->name;
    ud.loader = task->loader;
    ud.sess = sess;
    ud.context = (void*)_timeout;
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
        char *req;
        MALLOC(req, size + 1);
        memcpy(req, data, size);
        req[size] = '\0';
        msg.data = req;
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
            char *resp;
            MALLOC(resp, size + 1);
            memcpy(resp, data, size);
            resp[size] = '\0';
            msg.data = resp;
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
// 网络层接受回调：完成协议初始化并向任务推送 MSG_TYPE_ACCEPT 消息
static int32_t _net_accept(ev_ctx *ev, SOCKET fd, uint64_t skid, ud_cxt *ud) {
    task_ctx *task = task_grab(ud->loader, ud->name);
    if (NULL == task) {
        return ERR_FAILED;
    }
    int32_t rtn = prots_accepted(ev, fd, skid, ud);
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
    task_ctx *task = task_grab(ud->loader, ud->name);
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
// 网络层数据接收回调：循环解包并向任务推送 MSG_TYPE_RECV 消息
static void _net_recv(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client, buffer_ctx *buf, size_t size, ud_cxt *ud) {
    task_ctx *task = task_grab(ud->loader, ud->name);
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
        data = prots_unpack(ev, fd, skid, client, buf, ud, &msg.size, &status);
        if (NULL != data) {
            msg.data = data;
            msg.sess = ud->sess;
            if (BIT_CHECK(status, PROT_SLICE_START)) {
                msg.slice = PROT_SLICE_START;
            } else if(BIT_CHECK(status, PROT_SLICE)) {
                msg.slice = PROT_SLICE;
            } else if(BIT_CHECK(status, PROT_SLICE_END)) {
                msg.slice = PROT_SLICE_END;
            } else {
                msg.slice = 0;
            }
            _task_message_push(task, &msg);
        }
        if (BIT_CHECK(status, PROT_ERROR)
            || BIT_CHECK(status, PROT_CLOSE)) {
            ev_close(ev, fd, skid);
            break;
        }
        esize = buffer_size(buf);
        if (0 == esize
            || size == esize
            || BIT_CHECK(status, PROT_MOREDATA)) {
            break;
        }
    }
    task_ungrab(task);
}
// 网络层发送完成回调：向任务推送 MSG_TYPE_SEND 消息
static void _net_send(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client, size_t size, ud_cxt *ud) {
    task_ctx *task = task_grab(ud->loader, ud->name);
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
// 网络层 SSL 交换完成回调：完成协议 SSL 初始化并向任务推送 MSG_TYPE_SSLEXCHANGED 消息
static int32_t _net_ssl_exchanged(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client, ud_cxt *ud) {
    task_ctx *task = task_grab(ud->loader, ud->name);
    if (NULL == task) {
        return ERR_FAILED;
    }
    int32_t rtn = prots_ssl_exchanged(ev, fd, skid, client, ud);
    if (ERR_OK == rtn) {
        message_ctx msg;
        msg.mtype = MSG_TYPE_SSLEXCHANGED;
        msg.pktype = ud->pktype;
        msg.fd = fd;
        msg.skid = skid;
        msg.client = client;
        msg.sess = skid;
        _task_message_push(task, &msg);
    }
    task_ungrab(task);
    return rtn;
}
// 网络层连接关闭回调：通知协议层并向任务推送 MSG_TYPE_CLOSE 消息
static void _net_close(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client, ud_cxt *ud) {
    (void)ev;
    task_ctx *task = task_grab(ud->loader, ud->name);
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
    prots_closed(ud);
    _task_message_push(task, &msg);
    task_ungrab(task);
}
int32_t task_listen(task_ctx *task, pack_type pktype, struct evssl_ctx *evssl,
    const char *ip, uint16_t port, uint64_t *id, int32_t netev) {
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.pktype = pktype;
    ud.name = task->name;
    ud.loader = task->loader;
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
    cbs.ud_free = prots_udfree;
    return ev_listen(&task->loader->netev, evssl, ip, port, &cbs, &ud, id);
}
// 网络层连接建立回调：完成协议初始化并向任务推送 MSG_TYPE_CONNECT 消息
static int32_t _net_connect(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t err, ud_cxt *ud) {
    task_ctx *task = task_grab(ud->loader, ud->name);
    if (NULL == task) {
        return ERR_FAILED;
    }
    int32_t rtn = prots_connected(ev, fd, skid, ud, err);
    if (ERR_OK != rtn) {
        err = rtn;
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
    return err;
}
int32_t task_connect(task_ctx *task, pack_type pktype, struct evssl_ctx *evssl, const char *ip, uint16_t port, int32_t netev, void *extra,
    SOCKET *fd, uint64_t *skid) {
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.pktype = pktype;
    ud.name = task->name;
    ud.loader = task->loader;
    ud.context = extra;
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
    cbs.ud_free = prots_udfree;
    return ev_connect(&task->loader->netev, evssl, ip, port, &cbs, &ud, fd, skid);
}
// 网络层 UDP 接收回调：将地址和数据打包后向任务推送 MSG_TYPE_RECVFROM 消息
static void _net_recvfrom(ev_ctx *ev, SOCKET fd, uint64_t skid, char *buf, size_t size, netaddr_ctx *addr, ud_cxt *ud) {
    task_ctx *task = task_grab(ud->loader, ud->name);
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
int32_t task_udp(task_ctx *task, const char *ip, uint16_t port, SOCKET *fd, uint64_t *skid) {
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.name = task->name;
    ud.loader = task->loader;
    cbs_ctx cbs;
    ZERO(&cbs, sizeof(cbs));
    cbs.rf_cb = _net_recvfrom;
    cbs.ud_free = prots_udfree;
    return ev_udp(&task->loader->netev, ip, port, &cbs, &ud, fd, skid);
}
void task_set_request_timeout(task_ctx *task, uint32_t ms) {
    task->timeout_request = ms;
}
uint32_t task_get_request_timeout(task_ctx *task) {
    return task->timeout_request;
}
void task_set_connect_timeout(task_ctx *task, uint32_t ms) {
    task->timeout_connect = ms;
}
uint32_t task_get_connect_timeout(task_ctx *task) {
    return task->timeout_connect;
}
void task_set_netread_timeout(task_ctx *task, uint32_t ms) {
    task->timeout_netread = ms;
}
uint32_t task_get_netread_timeout(task_ctx *task) {
    return task->timeout_netread;
}
void task_accepted(task_ctx *task, _net_accept_cb _accept) {
    task->_net_accept = _accept;
}
void task_recved(task_ctx *task, _net_recv_cb _recv) {
    task->_net_recv = _recv;
}
void task_sended(task_ctx *task, _net_send_cb _send) {
    task->_net_send = _send;
}
void task_connected(task_ctx *task, _net_connect_cb _connect) {
    task->_net_connect = _connect;
}
void task_ssl_exchanged(task_ctx *task, _net_ssl_exchanged_cb _exchanged) {
    task->_ssl_exchanged = _exchanged;
}
void task_handshaked(task_ctx *task, _net_handshake_cb _handshake) {
    task->_net_handshaked = _handshake;
}
void task_closed(task_ctx *task, _net_close_cb _close) {
    task->_net_close = _close;
}
void task_recvedfrom(task_ctx *task, _net_recvfrom_cb _recvfrom) {
    task->_net_recvfrom = _recvfrom;
}
void task_requested(task_ctx *task, _request_cb _request) {
    task->_request = _request;
}
void task_responsed(task_ctx *task, _response_cb _response) {
    task->_response = _response;
}
