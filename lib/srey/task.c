#include "srey/task.h"
#include "srey/debug_request.h"
#include "containers/hashmap.h"

// 将任务名指针插入任务哈希表（重复时触发断言）
static void _task_map_set(struct hashmap *map, task_ctx *task) {
    name_t *key = &task->handle;
    ASSERTAB(NULL == hashmap_set(map, &key), "task name repeat.");
}
// 从任务哈希表中删除指定任务名，返回被删除的元素指针
static void *_task_map_del(struct hashmap *map, name_t handle) {
    name_t *key = &handle;
    return (void *)hashmap_delete(map, &key);
}
// 按任务名从哈希表中查找并返回 task_ctx，未找到返回 NULL
static task_ctx *_task_map_get(struct hashmap *map, name_t handle) {
    name_t *key = &handle;
    name_t **ptr = (name_t **)hashmap_get(map, &key);
    if (NULL == ptr) {
        return NULL;
    }
    return UPCAST(*ptr, task_ctx, handle);
}
// 写入 字符串名 → 句柄 索引；name 借用 task->name（调用方持 lckmaptasks 写锁，且已查重）
static void _task_name_map_set(struct hashmap *map, char *name, name_t handle) {
    name_handle_entry e = { .name = name, .handle = handle };
    hashmap_set(map, &e);
}
// 按字符串名查句柄，未找到返回 INVALID_TNAME（调用方持 lckmaptasks 读/写锁）
static name_t _task_name_map_get(struct hashmap *map, const char *name) {
    name_handle_entry q = { .name = (char *)name, .handle = INVALID_TNAME };
    name_handle_entry *r = (name_handle_entry *)hashmap_get(map, &q);
    return (NULL == r) ? INVALID_TNAME : r->handle;
}
// 删除 字符串名 索引项（元素借用 name，无 elfree；调用方持 lckmaptasks 写锁）
static void _task_name_map_del(struct hashmap *map, const char *name) {
    name_handle_entry q = { .name = (char *)name, .handle = INVALID_TNAME };
    hashmap_delete(map, &q);
}
// 处理启动消息：调用任务的 _task_startup 回调
static void _task_handle_startup(task_ctx *task, message_ctx *msg) {
    (void)msg;
    if (NULL != task->_task_startup) {
        task->_task_startup(task);
    }
}
// 处理关闭消息：调用 _task_closing 回调并释放任务引用
static void _task_handle_closing(task_ctx *task, message_ctx *msg) {
    (void)msg;
    if (NULL != task->_task_closing) {
        task->_task_closing(task);
    }
    task_ungrab(task);
}
// 处理超时消息：调用消息中携带的 _timeout_cb 回调函数
static void _task_handle_timeout(task_ctx *task, message_ctx *msg) {
    if (NULL != msg->data) {
        ((_timeout_cb)msg->data)(task, msg->sess);
    }
}
// 处理新连接接受消息
static void _task_handle_accept(task_ctx *task, message_ctx *msg) {
    if (NULL != task->_net_accept) {
        task->_net_accept(task, msg->fd, msg->skid, msg->subtype);
    }
}
// 处理连接建立消息
static void _task_handle_connect(task_ctx *task, message_ctx *msg) {
    if (NULL != task->_net_connect) {
        task->_net_connect(task, msg->fd, msg->skid, msg->subtype, msg->erro);
    }
}
// 处理 SSL 交换完成消息
static void _task_handle_ssl_exchanged(task_ctx *task, message_ctx *msg) {
    if (NULL != task->_ssl_exchanged) {
        task->_ssl_exchanged(task, msg->fd, msg->skid, msg->subtype, msg->client);
    }
}
// 处理应用层握手完成消息，处理后清理消息数据
static void _task_handle_handshaked(task_ctx *task, message_ctx *msg) {
    if (NULL != task->_net_handshaked) {
        task->_net_handshaked(task, msg->fd, msg->skid, msg->subtype, msg->client, msg->erro, msg->data, msg->size);
    }
    _message_clean(msg);
}
// 处理 TCP 数据接收消息，处理后清理消息数据
static void _task_handle_recv(task_ctx *task, message_ctx *msg) {
    if (NULL != task->_net_recv) {
        task->_net_recv(task, msg->fd, msg->skid, msg->subtype, msg->client, msg->slice, msg->data, msg->size);
    }
    _message_clean(msg);
}
// 处理数据发送完成消息
static void _task_handle_send(task_ctx *task, message_ctx *msg) {
    if (NULL != task->_net_send) {
        task->_net_send(task, msg->fd, msg->skid, msg->subtype, msg->client, msg->size);
    }
}
// 处理连接关闭消息
static void _task_handle_close(task_ctx *task, message_ctx *msg) {
    if (NULL != task->_net_close) {
        task->_net_close(task, msg->fd, msg->skid, msg->subtype, msg->client);
    }
}
// 处理 UDP 数据接收消息：从消息数据中解析出地址和载荷，处理后清理
static void _task_handle_recvfrom(task_ctx *task, message_ctx *msg) {
    if (NULL != task->_net_recvfrom) {
        netaddr_ctx *addr = msg->data;
        char ip[IP_LENS];
        netaddr_ip(addr, ip);
        uint16_t port = netaddr_port(addr);
        char *data = ((char*)msg->data) + sizeof(netaddr_ctx);
        task->_net_recvfrom(task, msg->fd, msg->skid, ip, port, data, msg->size);
    }
    _message_clean(msg);
}
// 处理任务间请求消息：若未注册请求回调，则返回错误响应
static void _task_handle_request(task_ctx *task, message_ctx *msg) {
    int32_t rtn = ERR_FAILED;//默认未处理；仅 REQ_DEBUG 被 _debug_request 处理后置 ERR_OK
    if (REQ_DEBUG == msg->subtype) {//公共的debug处理
        rtn = _debug_request(task, msg);
    }
    //REQ_SC_DELIVER 也走_request，自行处理。
    //sc_parse_deliver解析参数 path_matches_pattern 判断主题匹配
    if (ERR_OK != rtn) {//未被 debug 处理(非 REQ_DEBUG 或 debug 未接管)→ 透传给具体任务处理
        if (NULL != task->_request) {
            task->_request(task, msg->subtype, msg->sess, msg->src, msg->data, msg->size);
        } else {
            task_ctx *dtask = task_grab(task->loader, msg->src);
            if (NULL != dtask) {
                const char *erro = "not register request callback function.";
                task_response(dtask, msg->subtype, msg->sess, ERR_FAILED, (void *)erro, strlen(erro), 1);
                task_ungrab(dtask);
            }
        }
    }
    _message_clean(msg);
}
// 处理任务间响应消息，处理后清理消息数据
static void _task_handle_response(task_ctx *task, message_ctx *msg) {
    if (NULL != task->_response) {
        task->_response(task, msg->subtype, msg->sess, msg->erro, msg->data, msg->size);
    }
    _message_clean(msg);
}
// coro_fork 自发消息处理器：fork_item 由 coro_fork MALLOC、由 _message_clean(MSG_TYPE_FORK) FREE，
// 业务回调 item->func(task, item->arg) 跑在当前协程内（可 yield/resume 调下游）
static void _task_handle_fork(task_ctx *task, message_ctx *msg) {
    fork_item *item = (fork_item *)msg->data;
    if (NULL != item && NULL != item->func) {
        item->func(task, item->arg);
    }
    _message_clean(msg);
}
typedef void (*_msg_handler_t)(task_ctx *, message_ctx *);
// 按消息类型索引的处理函数表（静态分发，无 switch-case 开销）
static const _msg_handler_t _msg_handlers[MSG_TYPE_ALL] = {
    [MSG_TYPE_STARTUP]      = _task_handle_startup,
    [MSG_TYPE_CLOSING]      = _task_handle_closing,
    [MSG_TYPE_TIMEOUT]      = _task_handle_timeout,
    [MSG_TYPE_ACCEPT]       = _task_handle_accept,
    [MSG_TYPE_CONNECT]      = _task_handle_connect,
    [MSG_TYPE_SSLEXCHANGED] = _task_handle_ssl_exchanged,
    [MSG_TYPE_HANDSHAKED]   = _task_handle_handshaked,
    [MSG_TYPE_RECV]         = _task_handle_recv,
    [MSG_TYPE_SEND]         = _task_handle_send,
    [MSG_TYPE_CLOSE]        = _task_handle_close,
    [MSG_TYPE_RECVFROM]     = _task_handle_recvfrom,
    [MSG_TYPE_REQUEST]      = _task_handle_request,
    [MSG_TYPE_RESPONSE]     = _task_handle_response,
    [MSG_TYPE_FORK]         = _task_handle_fork,
};
void _message_run(task_ctx *task, message_ctx *msg) {
    if (msg->mtype > MSG_TYPE_NONE
        && msg->mtype < MSG_TYPE_ALL
        && NULL != _msg_handlers[msg->mtype]) {
        _msg_handlers[msg->mtype](task, msg);
    }
}
// 默认消息分发函数（直接调用 _message_run）
static void _task_message_dispatch(task_dispatch_arg *arg) {
    _message_run(arg->task, &arg->msg);
}
task_ctx *task_new(loader_ctx *loader, const char *name, size_t quecap,
                   _task_dispatch_cb _dispatch, free_cb _argfree, void *arg) {
    task_ctx *task;
    CALLOC(task, 1, sizeof(task_ctx));
    task->type = TASK_NORMAL;
    task->loader = loader;
    task->handle = createid();
    if (!EMPTYSTR(name)) {
        size_t len = strlen(name);
        MALLOC(task->name, len + 1);
        memcpy(task->name, name, len + 1);
    }
    task->ref = 1;
    task->timeout_request = 3 * 1000;
    task->timeout_connect = 5 * 1000;
    task->timeout_netread = 10 * 1000;
    if (NULL == _dispatch) {
        task->_task_dispatch = _task_message_dispatch;
    } else {
        task->_task_dispatch = _dispatch;
    }
    task->_arg_free = _argfree;
    task->arg = arg;
    fsqu_init(&task->qumsg, sizeof(message_ctx *), 0 == quecap ? ONEK : (uint32_t)quecap);
    tda_init(&task->tda, (size_t)(fsqu_capacity(&task->qumsg) / QUEUE_OVERLOAD_RATIO));
    return task;
}
void task_free(task_ctx *task) {
    if (NULL != task->_arg_free
        && NULL != task->arg) {
        task->_arg_free(task->arg);
    }
    // ref 归零进入 task_free，无其他持有者；qumsg 单消费者排空
    message_ctx *msg;
    while (ERR_OK == fsqu_pop_sc(&task->qumsg, &msg)) {
        _message_clean(msg);
        FREE(msg);
    }
    fsqu_free(&task->qumsg);
    FREE(task->name);
    FREE(task);
}
int32_t task_register(task_ctx *task, _task_startup_cb _startup, _task_closing_cb _closing) {
    task->_task_startup = _startup;
    task->_task_closing = _closing;
    message_ctx startup = { 0 };
    startup.mtype = MSG_TYPE_STARTUP;
    rwlock_distr_wrlock(&task->loader->lckmaptasks);
    // 字符串名重名拒绝（句柄由 createid 保证唯一，maptasks 由 _task_map_set 的断言兜底）
    if (NULL != task->name
        && INVALID_TNAME != _task_name_map_get(task->loader->mapnames, task->name)) {
        rwlock_distr_wrunlock(&task->loader->lckmaptasks);
        LOG_ERROR("task name %s repeat.", task->name);
        return ERR_FAILED;
    }
    _task_map_set(task->loader->maptasks, task);
    if (NULL != task->name) {
        _task_name_map_set(task->loader->mapnames, task->name, task->handle);
    }
    _task_message_push(task, &startup);
    // loader 正在广播关闭时（closing=1），此 task 晚于广播注册，
    // 不会收到全局 CLOSING，须在此立即补发，确保 task 能正常退出
    if (ATOMIC_GET(&task->loader->closing)) {
        message_ctx closing = { 0 };
        closing.mtype = MSG_TYPE_CLOSING;
        ATOMIC_SET(&task->closing, 1);
        _task_message_push(task, &closing);
    }
    rwlock_distr_wrunlock(&task->loader->lckmaptasks);
    return ERR_OK;
}
void task_close(task_ctx *task) {
    if (ATOMIC_CAS(&task->closing, 0, 1)) {
        message_ctx closing = { 0 };
        closing.mtype = MSG_TYPE_CLOSING;
        _task_message_push(task, &closing);
    }
}
int32_t task_isclosing(task_ctx *task) {
    // loader->closing：全局关闭广播已发出（_task_closing 置位）
    // task->closing ：本任务已收到 CLOSING 消息（task_close 或关闭广播触发）
    return ATOMIC_GET(&task->loader->closing) || ATOMIC_GET(&task->closing);
}
task_type task_get_type(task_ctx *task) {
    return task->type;
}
task_ctx *task_grab(loader_ctx *loader, name_t handle) {
    if (INVALID_TNAME == handle) {
        return NULL;
    }
    rwlock_distr_rdlock(&loader->lckmaptasks);
    task_ctx *task = _task_map_get(loader->maptasks, handle);
    if (NULL != task) {
        ATOMIC_ADD(&task->ref, 1);
    }
    rwlock_distr_runlock(&loader->lckmaptasks);
    return task;
}
name_t task_find_name(loader_ctx *loader, const char *name) {
    if (EMPTYSTR(name)) {
        return INVALID_TNAME;
    }
    rwlock_distr_rdlock(&loader->lckmaptasks);
    name_t handle = _task_name_map_get(loader->mapnames, name);
    rwlock_distr_runlock(&loader->lckmaptasks);
    return handle;
}
void task_incref(task_ctx *task) {
    ATOMIC_ADD(&task->ref, 1);
}
void task_ungrab(task_ctx *task) {
    loader_ctx *loader = task->loader;
    name_t handle = task->handle;
    if (1 != ATOMIC_ADD(&task->ref, -1)) {
        return;
    }
    void *ptr = NULL;
    // 反查一次，防止当前线程执行rwlock_distr_wrlock前被其他线程抢占
    // 并执行task_grab->task_ungrab完成了释放，造成task->ref崩溃
    rwlock_distr_wrlock(&loader->lckmaptasks);
    if (task == _task_map_get(loader->maptasks, handle)
        && 0 == ATOMIC_GET(&task->ref)) {
        ptr = _task_map_del(loader->maptasks, handle);
        if (NULL != task->name) {
            _task_name_map_del(loader->mapnames, task->name);
        }
    }
    rwlock_distr_wrunlock(&loader->lckmaptasks);
    if (NULL != ptr) {
        task_free(task);
    }
}
int32_t _message_should_clean(message_ctx *msg) {
    // shared 路径：task_multi_call / task_multi_request 广播,无论 data 是否为 NULL 都需 ref-- 防止泄漏
    if (NULL != msg->shared) {
        return ERR_OK;
    }
    if ((MSG_TYPE_RECV == msg->mtype
        || MSG_TYPE_RECVFROM == msg->mtype
        || MSG_TYPE_REQUEST == msg->mtype
        || MSG_TYPE_RESPONSE == msg->mtype
        || MSG_TYPE_HANDSHAKED == msg->mtype
        || MSG_TYPE_FORK == msg->mtype)
        && NULL != msg->data) {
        return ERR_OK;
    }
    return ERR_FAILED;
}
void _message_clean(message_ctx *msg) {
    // task_multi_call / task_multi_request 广播路径：N 个 message 共享同一份 data,各 task ref-- 归 0 才 FREE
    if (NULL != msg->shared) {
        if (1 == ATOMIC_ADD(&msg->shared->ref, -1)) {
            FREE(msg->shared->data);
            FREE(msg->shared);
        }
        return;
    }
    switch (msg->mtype) {
    case MSG_TYPE_RECV:
    case MSG_TYPE_RECVFROM:
        prots_pkfree(msg->subtype, msg->data);
        break;
    case MSG_TYPE_HANDSHAKED:
        prots_hsfree(msg->subtype, msg->data);
        break;
    case MSG_TYPE_REQUEST:
    case MSG_TYPE_RESPONSE:
        FREE(msg->data);
        break;
    case MSG_TYPE_FORK:
        // fork_item 由 coro_fork MALLOC，与 REQUEST/RESPONSE 一样 FREE；item->arg 由业务管理不释放
        FREE(msg->data);
        break;
    default:
        break;
    }
}
// 时间轮超时回调：将超时消息推入对应任务队列
static void _task_message_timeout_push(ud_cxt *ud) {
    task_ctx *task = task_grab(ud->loader, ud->handle);
    if (NULL == task) {
        return;
    }
    message_ctx msg = { 0 };
    msg.mtype = MSG_TYPE_TIMEOUT;
    msg.sess = ud->sess;
    msg.data = ud->context;
    _task_message_push(task, &msg);
    task_ungrab(task);
}
void task_timeout(task_ctx *task, uint64_t sess, uint32_t ms, _timeout_cb _timeout) {
    ASSERTAB((NULL == _timeout && 0 != sess) || (NULL != _timeout && 0 == sess), "parameter error");
    ud_cxt ud = { 0 };
    ud.handle = task->handle;
    ud.loader = task->loader;
    ud.sess = sess;
    ud.context = (void*)_timeout;
    tw_add(&task->loader->tw, ms, _task_message_timeout_push, NULL, &ud);
}
void task_request(task_ctx *dst, task_ctx *src, subtype_t reqtype, uint64_t sess,
                  void *data, size_t size, int32_t copy) {
    ASSERTAB((NULL != src && 0 != sess) || (NULL == src && 0 == sess), "parameter error");
    message_ctx msg = { 0 };
    msg.mtype = MSG_TYPE_REQUEST;
    msg.subtype = reqtype;
    if (NULL != src) {
        msg.src = src->handle;
        msg.sess = sess;
    } else {
        msg.src = INVALID_TNAME;
    }
    if (NULL != data && 0 != copy) {
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
void task_response(task_ctx *dst, subtype_t reqtype, uint64_t sess,
                   int32_t erro, void *data, size_t size, int32_t copy) {
    message_ctx msg = { 0 };
    msg.mtype = MSG_TYPE_RESPONSE;
    msg.sess = sess;
    msg.subtype = reqtype;
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
void task_call(task_ctx *dst, subtype_t reqtype, void *data, size_t size, int32_t copy) {
    task_request(dst, NULL, reqtype, 0, data, size, copy);
}
int32_t task_multi_request(task_ctx *dsts[], int32_t n, task_ctx *src, subtype_t reqtype,
                           uint64_t sess, void *data, size_t size, int32_t copy) {
    ASSERTAB((NULL != src && 0 != sess) || (NULL == src && 0 == sess), "parameter error");
    int32_t i;
    int32_t valid = 0;
    for (i = 0; i < n; i++) {
        if (NULL != dsts[i]) {
            valid++;
        }
    }
    // 没有有效 dst：按 copy 语义清理；copy=1 时 data 归调用方；copy=0 时所有权已转移给本函数，必须 FREE
    if (0 == valid) {
        if (0 == copy && NULL != data) {
            FREE(data);
        }
        return 0;
    }
    // 分配共享 pack：ref 初始 = valid；data 按 copy 决定拷贝 / 转移所有权
    shared_data *shared;
    MALLOC(shared, sizeof(shared_data));
    if (0 != copy && NULL != data) {
        char *buf;
        MALLOC(buf, size + 1);
        memcpy(buf, data, size);
        buf[size] = '\0';
        shared->data = buf;
    } else {
        shared->data = data;
    }
    ATOMIC_SET(&shared->ref, valid);
    // 投递 N 条 message：共用 shared，_message_clean 走 shared 分支 ref-- 归 0 才 FREE
    message_ctx msg = { 0 };
    msg.mtype = MSG_TYPE_REQUEST;
    msg.subtype = reqtype;
    if (NULL != src) {
        msg.src = src->handle;
        msg.sess = sess;
    } else {
        msg.src = INVALID_TNAME;
    }
    msg.data = shared->data;
    msg.size = size;
    msg.shared = shared;
    for (i = 0; i < n; i++) {
        if (NULL != dsts[i]) {
            _task_message_push(dsts[i], &msg);
        }
    }
    return valid;
}
void task_multi_call(task_ctx *dsts[], int32_t n, subtype_t reqtype,
                     void *data, size_t size, int32_t copy) {
    (void)task_multi_request(dsts, n, NULL, reqtype, 0, data, size, copy);
}
// 网络层接受回调：完成协议初始化并向任务推送 MSG_TYPE_ACCEPT 消息
static int32_t _task_net_accept(ev_ctx *ev, SOCKET fd, uint64_t skid, ud_cxt *ud) {
    task_ctx *task = task_grab(ud->loader, ud->handle);
    if (NULL == task) {
        return ERR_FAILED;
    }
    int32_t rtn = prots_accepted(ev, fd, skid, ud);
    if (ERR_OK == rtn) {
        message_ctx msg = { 0 };
        msg.mtype = MSG_TYPE_ACCEPT;
        msg.subtype = ud->pktype;
        msg.fd = fd;
        msg.skid = skid;
        _task_message_push(task, &msg);
    }
    task_ungrab(task);
    return rtn;
}
int32_t _message_handshaked_push(SOCKET fd, uint64_t skid, int32_t client, ud_cxt *ud, int32_t erro, void *data, size_t lens) {
    task_ctx *task = task_grab(ud->loader, ud->handle);
    if (NULL == task) {
        message_ctx tmp = { 0 };
        tmp.mtype = MSG_TYPE_HANDSHAKED;
        tmp.subtype = ud->pktype;
        tmp.data = data;
        _message_clean(&tmp);
        return ERR_FAILED;
    }
    message_ctx msg = { 0 };
    msg.mtype = MSG_TYPE_HANDSHAKED;
    msg.subtype = ud->pktype;
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
static void _task_net_recv(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client, buffer_ctx *buf, size_t size, ud_cxt *ud) {
    task_ctx *task = task_grab(ud->loader, ud->handle);
    if (NULL == task) {
        ev_close(ev, fd, skid, 1);
        return;
    }
    message_ctx msg = { 0 };
    msg.mtype = MSG_TYPE_RECV;
    msg.subtype = ud->pktype;
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
        if (BIT_CHECK(status, PROT_ERROR)) {
            ev_close(ev, fd, skid, 1);
            break;
        }
        if (BIT_CHECK(status, PROT_CLOSE)) {
            // 协议层正常关闭信号(如 WebSocket close frame),业务应答 close frame
            // 可能仍在 buf_s,immed=0 让其发完再关
            ev_close(ev, fd, skid, 0);
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
static void _task_net_send(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client, size_t size, ud_cxt *ud) {
    task_ctx *task = task_grab(ud->loader, ud->handle);
    if (NULL == task) {
        ev_close(ev, fd, skid, 1);
        return;
    }
    message_ctx msg = { 0 };
    msg.mtype = MSG_TYPE_SEND;
    msg.subtype = ud->pktype;
    msg.fd = fd;
    msg.skid = skid;
    msg.client = client;
    msg.size = size;
    _task_message_push(task, &msg);
    task_ungrab(task);
}
// 网络层 SSL 交换完成回调：完成协议 SSL 初始化并向任务推送 MSG_TYPE_SSLEXCHANGED 消息
static int32_t _task_net_ssl_exchanged(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client, ud_cxt *ud, void *ssl) {
    task_ctx *task = task_grab(ud->loader, ud->handle);
    if (NULL == task) {
        return ERR_FAILED;
    }
    int32_t rtn = prots_ssl_exchanged(ev, fd, skid, client, ud, ssl);
    if (ERR_OK == rtn) {
        message_ctx msg = { 0 };
        msg.mtype = MSG_TYPE_SSLEXCHANGED;
        msg.subtype = ud->pktype;
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
static void _task_net_close(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client, ud_cxt *ud) {
    (void)ev;
    task_ctx *task = task_grab(ud->loader, ud->handle);
    if (NULL == task) {
        return;
    }
    message_ctx msg = { 0 };
    msg.mtype = MSG_TYPE_CLOSE;
    msg.subtype = ud->pktype;
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
    ud_cxt ud = { 0 };
    ud.pktype = pktype;
    ud.handle = task->handle;
    ud.loader = task->loader;
    cbs_ctx cbs = { 0 };
    if (BIT_CHECK(netev, NETEV_ACCEPT)) {
        cbs.acp_cb = _task_net_accept;
    }
    if (BIT_CHECK(netev, NETEV_SEND)) {
        cbs.s_cb = _task_net_send;
    }
    if (NULL != evssl
        || BIT_CHECK(netev, NETEV_AUTHSSL)) {
        cbs.exch_cb = _task_net_ssl_exchanged;
    }
    cbs.r_cb = _task_net_recv;
    cbs.c_cb = _task_net_close;
    cbs.ud_free = prots_udfree;
    return ev_listen(&task->loader->netev, evssl, ip, port, &cbs, &ud, id);
}
// 网络层连接建立回调：完成协议初始化并向任务推送 MSG_TYPE_CONNECT 消息
static int32_t _task_net_connect(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t err, ud_cxt *ud) {
    task_ctx *task = task_grab(ud->loader, ud->handle);
    if (NULL == task) {
        return ERR_FAILED;
    }
    int32_t rtn = prots_connected(ev, fd, skid, ud, err);
    if (ERR_OK != rtn) {
        err = rtn;
    }
    message_ctx msg = { 0 };
    msg.mtype = MSG_TYPE_CONNECT;
    msg.subtype = ud->pktype;
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
    ud_cxt ud = { 0 };
    ud.pktype = pktype;
    ud.handle = task->handle;
    ud.loader = task->loader;
    ud.context = extra;
    cbs_ctx cbs = { 0 };
    if (BIT_CHECK(netev, NETEV_SEND)) {
        cbs.s_cb = _task_net_send;
    }
    if (NULL != evssl 
        || BIT_CHECK(netev, NETEV_AUTHSSL)) {
        cbs.exch_cb = _task_net_ssl_exchanged;
    }
    cbs.conn_cb = _task_net_connect;
    cbs.r_cb = _task_net_recv;
    cbs.c_cb = _task_net_close;
    cbs.ud_free = prots_udfree;
    return ev_connect(&task->loader->netev, evssl, ip, port, &cbs, &ud, fd, skid);
}
// 网络层 UDP 接收回调：将地址和数据打包后向任务推送 MSG_TYPE_RECVFROM 消息
static void _task_net_recvfrom(ev_ctx *ev, SOCKET fd, uint64_t skid, char *buf, size_t size, netaddr_ctx *addr, ud_cxt *ud) {
    task_ctx *task = task_grab(ud->loader, ud->handle);
    if (NULL == task) {
        ev_close(ev, fd, skid, 1);
        return;
    }
    message_ctx msg = { 0 };
    msg.mtype = MSG_TYPE_RECVFROM;
    msg.subtype = PACK_NONE; // UDP 路径透传原始数据，避免 _message_clean→prots_pkfree 进入特定协议释放路径
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
    ud_cxt ud = { 0 };
    ud.handle = task->handle;
    ud.loader = task->loader;
    cbs_ctx cbs = { 0 };
    cbs.rf_cb = _task_net_recvfrom;
    cbs.ud_free = prots_udfree;
    return ev_udp(&task->loader->netev, ip, port, &cbs, &ud, fd, skid);
}
void task_set_priority(task_ctx *task, int32_t priority) {
    int32_t p = priority;
    if (p < 0) {
        p = 0;
    } else if (p > TASK_PRIORITY_MAX) {
        p = TASK_PRIORITY_MAX;
    }
    ATOMIC_SET(&task->priority, (atomic_t)p);
}
int32_t task_get_priority(task_ctx *task) {
    return (int32_t)ATOMIC_GET(&task->priority);
}
void task_set_request_timeout(task_ctx *task, uint32_t ms) {
    if (0 == ms) {
        return;
    }
    ATOMIC_SET(&task->timeout_request, ms);
}
uint32_t task_get_request_timeout(task_ctx *task) {
    return (uint32_t)ATOMIC_GET(&task->timeout_request);
}
void task_set_connect_timeout(task_ctx *task, uint32_t ms) {
    if (0 == ms) {
        return;
    }
    ATOMIC_SET(&task->timeout_connect, ms);
}
uint32_t task_get_connect_timeout(task_ctx *task) {
    return (uint32_t)ATOMIC_GET(&task->timeout_connect);
}
void task_set_netread_timeout(task_ctx *task, uint32_t ms) {
    if (0 == ms) {
        return;
    }
    ATOMIC_SET(&task->timeout_netread, ms);
}
uint32_t task_get_netread_timeout(task_ctx *task) {
    return (uint32_t)ATOMIC_GET(&task->timeout_netread);
}
void task_stat(task_ctx *task, uint64_t nmsg[MSG_TYPE_ALL], uint64_t dispatch_cpu_ns[MSG_TYPE_ALL]) {
#if ENABLE_DISPATCH_STAT
    memcpy(nmsg, task->nmsg, sizeof(task->nmsg));
    memcpy(dispatch_cpu_ns, task->dispatch_cpu_ns, sizeof(task->dispatch_cpu_ns));
#else
    (void)task;
    ZERO(nmsg, sizeof(uint64_t) * MSG_TYPE_ALL);
    ZERO(dispatch_cpu_ns, sizeof(uint64_t) * MSG_TYPE_ALL);
#endif
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
