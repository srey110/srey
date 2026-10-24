#include "srey/task.h"
#include "srey/debug_request.h"
#include "containers/hashmap.h"
#include "utils/idgen.h"

// task 消息队列默认条数
#define TASK_QUEUE_CAP 256
typedef void (*_msg_handler_t)(task_ctx *, message_ctx *);
// 网络事件 emit 实现：begin=grab 目标 task，emit=入队，end=激活+ungrab；经 _task_net_emit 注册给 prots 作为消息汇。
// [0] 本窗口已入队条数，[1] 窗口嵌套层数；begin/emit/end 三者同在一条线程上配对，故用线程局部变量
TLS_DEFINE(int32_t, _emit_win, 2)

// 将任务名指针插入任务哈希表（重复时触发断言）
static inline void _task_map_set(task_map *map, task_ctx *task) {
    name_t *key = &task->handle;
    ASSERTAB(NULL == task_map_set(map, &key), "task name repeat.");
}
// 按任务名从哈希表中查找并返回 task_ctx，未找到返回 NULL
static inline task_ctx *_task_map_get(task_map *map, name_t handle) {
    name_t *key = &handle;
    name_t **ptr = task_map_get(map, &key);
    if (NULL == ptr) {
        return NULL;
    }
    return UPCAST(*ptr, task_ctx, handle);
}
// 写入 字符串名 → 句柄 索引，重名返回 ERR_FAILED 且不改表；name 借用 task->name（调用方持 lckmaptasks 写锁）
static inline int32_t _task_name_map_add(tname_map *map, char *name, name_t handle) {
    int32_t found;
    name_handle_entry e = { .name = name, .handle = handle };
    tname_map_get_set(map, &e, &found);
    return found ? ERR_FAILED : ERR_OK;
}
// 按字符串名查句柄，未找到返回 INVALID_TNAME（调用方持 lckmaptasks 读/写锁）
static inline name_t _task_name_map_get(tname_map *map, const char *name) {
    name_handle_entry q = { .name = (char *)name, .handle = INVALID_TNAME };
    name_handle_entry *r = tname_map_get(map, &q);
    return (NULL == r) ? INVALID_TNAME : r->handle;
}
// 删除 字符串名 索引项（元素借用 name，无 elfree；调用方持 lckmaptasks 写锁）
static inline void _task_name_map_del(tname_map *map, const char *name) {
    name_handle_entry q = { .name = (char *)name, .handle = INVALID_TNAME };
    tname_map_delete(map, &q);
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
// 处理新连接接受消息(订阅了 NETEV_PEERADDR 时 data 是对端地址)
static void _task_handle_accept(task_ctx *task, message_ctx *msg) {
    if (NULL != task->_net_accept) {
        task->_net_accept(task, &msg->sk, msg->subtype, (const netaddr_ctx *)msg->data);
    }
}
// 处理连接建立消息
static void _task_handle_connect(task_ctx *task, message_ctx *msg) {
    if (NULL != task->_net_connect) {
        task->_net_connect(task, &msg->sk, msg->subtype, msg->erro);
    }
}
// 处理 SSL 交换完成消息
static void _task_handle_ssl_exchanged(task_ctx *task, message_ctx *msg) {
    if (NULL != task->_ssl_exchanged) {
        task->_ssl_exchanged(task, &msg->sk, msg->subtype, msg->client);
    }
}
// 处理应用层握手完成消息
static void _task_handle_handshaked(task_ctx *task, message_ctx *msg) {
    if (NULL != task->_net_handshaked) {
        task->_net_handshaked(task, &msg->sk, msg->subtype, msg->client, msg->erro, msg->data, msg->size);
    }
}
// 处理 TCP 数据接收消息
static void _task_handle_recv(task_ctx *task, message_ctx *msg) {
    if (NULL != task->_net_recv) {
        task->_net_recv(task, &msg->sk, msg->subtype, msg->client, msg->slice, msg->data, msg->size);
    }
}
// 处理数据发送完成消息
static void _task_handle_send(task_ctx *task, message_ctx *msg) {
    if (NULL != task->_net_send) {
        task->_net_send(task, &msg->sk, msg->subtype, msg->client, msg->size);
    }
}
// 处理连接关闭消息：先按注册顺序调观察者，再调业务回调。协程 task 经它的关闭协程走到这里
static void _task_handle_close(task_ctx *task, message_ctx *msg) {
    // NEVERCONN 的合成 CLOSE 只为唤醒等待方。连接从未建立,没有"关闭"可通知,故不投给业务;
    // 失败本身已由 CONNECT(TCP)或 HANDSHAKED(KCP)的 erro 交出去了
    if (CLOSE_TYPE_NEVERCONN == msg->erro) {
        return;
    }
    close_watch *w;
    uint32_t n = cwatch_arr_size(&task->watchers);// 观察者里新注册的不进本轮
    for (uint32_t i = 0; i < n; i++) {
        w = cwatch_arr_at(&task->watchers, (int32_t)i);
        w->cb(task, &msg->sk, msg->subtype, msg->client, msg->erro, msg->sess, w->arg);
    }
    if (NULL != task->_net_close) {
        task->_net_close(task, &msg->sk, msg->subtype, msg->client, msg->erro, msg->sess);
    }
}
// 处理 UDP 数据接收消息：源地址原样交给回调，不格式化成串
static void _task_handle_recvfrom(task_ctx *task, message_ctx *msg) {
    if (NULL != task->_net_recvfrom) {
        recvfrom_ctx *rf = msg->data;
        task->_net_recvfrom(task, &msg->sk, msg->subtype, &rf->addr, rf->data, rf->len);
    }
}
// 处理任务间请求消息：若未注册请求回调，则返回错误响应
static void _task_handle_request(task_ctx *task, message_ctx *msg) {
    int32_t rtn = ERR_FAILED;//默认未处理；仅 REQ_DEBUG 被 _debug_request 处理后置 ERR_OK
    if (REQ_DEBUG == msg->subtype) {//公共的debug处理
        rtn = _debug_request(task, msg);
    }
    if (ERR_OK != rtn) {//未被 debug 处理(非 REQ_DEBUG 或 debug 未接管)→ 透传给具体任务处理
        if (NULL != task->_request) {
            task->_request(task, msg->subtype, msg->sess, msg->src, msg->data, msg->size);
        } else {
            const char *erro = "not register request callback function.";
            (void)task_response_to(task->loader, msg->src, msg->subtype, msg->sess, ERR_FAILED,
                                   (void *)erro, strlen(erro), 1);
        }
    }
}
// 处理任务间响应消息
static void _task_handle_response(task_ctx *task, message_ctx *msg) {
    if (NULL != task->_response) {
        task->_response(task, msg->subtype, msg->sess, msg->erro, msg->data, msg->size);
    }
}
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
};
// 消息数据在这里统一清理，处理函数自己不清：哪种消息带数据只看 _message_data_kind，
// 新带上数据的消息类型不会因为某个处理函数漏写而泄漏。没有数据的消息清理是空操作
void _message_run(task_ctx *task, message_ctx *msg) {
    if (msg->mtype > MSG_TYPE_NONE
        && msg->mtype < MSG_TYPE_ALL
        && NULL != _msg_handlers[msg->mtype]) {
        _msg_handlers[msg->mtype](task, msg);
    }
    message_clean(msg);
}
// 默认消息分发函数（直接调用 _message_run）
static void _task_message_dispatch(task_dispatch_arg *arg) {
    _message_run(arg->task, arg->msg);
}
task_ctx *task_new(loader_ctx *loader, const char *name, uint32_t quecap,
                   _task_dispatch_cb _dispatch, free_cb _argfree, void *arg) {
    task_ctx *task;
    CALLOC(task, 1, sizeof(task_ctx));
    task->type = TASK_NORMAL;
    task->loader = loader;
    task->handle = createid();
    if (!EMPTYSTR(name)) {
        task->name = dup_zero(name, strlen(name));
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
    msgq_init(&task->qumsg, 0 == quecap ? TASK_QUEUE_CAP : quecap);
    tda_init(&task->tda, (size_t)(msgq_capacity(&task->qumsg) / QUEUE_OVERLOAD_RATIO));
    return task;
}
void task_free(task_ctx *task) {
    if (NULL != task->_arg_free
        && NULL != task->arg) {
        task->_arg_free(task->arg);
    }
    // ref 归零进入 task_free，无其他持有者；qumsg 单消费者排空
    message_ctx msg;
    while (ERR_OK == msgq_pop_sc(&task->qumsg, &msg)) {
        message_clean(&msg);
    }
    msgq_free(&task->qumsg);
    cwatch_arr_free(&task->watchers);
    FREE(task->acp_group);
    FREE(task->name);
    FREE(task);
}
// 投递 CLOSING（调用方已把 closing 置位）
static void _task_post_closing(task_ctx *task) {
    message_ctx closing = { 0 };
    closing.mtype = MSG_TYPE_CLOSING;
    _task_message_post(task, &closing);
}
int32_t task_register(task_ctx *task, _task_startup_cb _startup, _task_closing_cb _closing) {
    task->_task_startup = _startup;
    task->_task_closing = _closing;
    message_ctx startup = { 0 };
    startup.mtype = MSG_TYPE_STARTUP;
    rwlock_distr_wrlock(&task->loader->lckmaptasks);
    // 字符串名重名拒绝（句柄由 createid 保证唯一，maptasks 由 _task_map_set 的断言兜底）
    if (NULL != task->name
        && ERR_OK != _task_name_map_add(task->loader->mapnames, task->name, task->handle)) {
        rwlock_distr_wrunlock(&task->loader->lckmaptasks);
        LOG_ERROR("task name %s repeat.", task->name);
        return ERR_FAILED;
    }
    task->registered = 1;
    _task_map_set(task->loader->maptasks, task);
    // 注册前就关过的只记了标志，在这里补发；须在投 STARTUP 之前读，之后 startup 自己也可能关
    atomic_t closed = ATOMIC_GET(&task->closing);
    _task_message_post(task, &startup);
    if (closed) {
        _task_post_closing(task);
    } else if (ATOMIC_GET(&task->loader->closing)) {
        task_close(task);// 晚于 loader 的关闭广播注册，收不到全局 CLOSING，在此补发
    }
    rwlock_distr_wrunlock(&task->loader->lckmaptasks);
    return ERR_OK;
}
// 注册前关只记标志、不投递：那时投出去，worker 会与还在初始化它的注册线程同时动它
void task_close(task_ctx *task) {
    if (ATOMIC_CAS(&task->closing, 0, 1)
        && task->registered) {
        _task_post_closing(task);
    }
}
int32_t task_isclosing(task_ctx *task) {
    // loader->closing：全局关闭广播已发出（_task_closing 置位）
    // task->closing ：本任务已被要求关闭（task_close 或关闭广播触发）
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
        ATOMIC_ADD_RELAXED(&task->ref, 1);
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
int32_t task_exists(loader_ctx *loader, name_t handle) {
    int32_t alive;
    if (INVALID_TNAME == handle) {
        return 0;
    }
    rwlock_distr_rdlock(&loader->lckmaptasks);
    alive = (NULL != _task_map_get(loader->maptasks, handle));
    rwlock_distr_runlock(&loader->lckmaptasks);
    return alive;
}
void task_incref(task_ctx *task) {
    ATOMIC_ADD_RELAXED(&task->ref, 1);
}
// task_ungrab 摘表之后的释放，放完 nfreeing 减一：net 线程上的推到本轮派发之后，别的线程当场调。
// ev_free 时还在队里的也走它(作 fcb)，那时在 ev_free 的线程上、里面不得再调 ev_*；正常关停 _loader_task_closing
// 要等 nfreeing 归零，走不到那一步。
// 叫醒的握手：这边先减计数再查 closing，关停那边先置 closing 再查计数，两边都是足序，至少一方看得见对方
static void _task_free_deferred(void *arg) {
    task_ctx *task = arg;
    loader_ctx *loader = task->loader;
    task_free(task);
    if (1 == ATOMIC_ADD(&loader->nfreeing, -1)
        && 0 != ATOMIC_GET_SEQCST(&loader->closing)) {
        waiter_wake_held(&loader->closing_waiter);
    }
}
// 摘除后唤醒 _loader_task_closing：这是全仓唯一从 maptasks 摘 task 的地方。叫醒必须排在 wrunlock 之后——
// waiter 用 mutex + cond 实现时，等待方是持着 closing_waiter 的锁去拿 lckmaptasks，放进写锁内锁序就反过来了。
// closing 为 0 时（正常运行期）没有等待者，一条 atomic load 就短路掉。
// net 线程上归零的不当场释放：_arg_free 会析构上层句柄，此刻多半还嵌在协议回调或 tick 里，句柄析构经 ev_props
// 同线程当场执行会重入正在用的连接状态，推到本线程本轮派发之后再放。哪个线程摘的都记 nfreeing：
// 只记 net 线程的话，别的线程摘完表、task_free 还没做完时关停就看到表空往下走了
void task_ungrab(task_ctx *task) {
    loader_ctx *loader = task->loader;
    name_t handle = task->handle;
    if (1 != ATOMIC_ADD(&task->ref, -1)) {
        return;
    }
    int32_t idx = ev_cur_index(&loader->netev);
    void *ptr = NULL;
    name_t *key = &handle;
    name_t **found;
    uint64_t hash = _TASK_MAP_HASH(&key);// 查与删共用这一次哈希
    // 反查一次，防止当前线程执行rwlock_distr_wrlock前被其他线程抢占
    // 并执行task_grab->task_ungrab完成了释放，造成task->ref崩溃
    rwlock_distr_wrlock(&loader->lckmaptasks);
    found = task_map_get_with_hash(loader->maptasks, &key, hash);
    if (NULL != found && &task->handle == *found
        && 0 == ATOMIC_GET(&task->ref)) {
        ptr = task_map_delete_with_hash(loader->maptasks, &key, hash);
        if (NULL != task->name) {
            _task_name_map_del(loader->mapnames, task->name);
        }
        ATOMIC_ADD(&loader->nfreeing, 1);// 与摘表同在写锁里：关停那边不会看到表里没了、计数却还没记上
    }
    rwlock_distr_wrunlock(&loader->lckmaptasks);
    if (NULL == ptr) {
        return;
    }
    if (INVALID_INDEX != idx) {
        ev_defer_exec(&loader->netev, idx, _task_free_deferred, _task_free_deferred, task);
        return;
    }
    _task_free_deferred(task);
}
int32_t task_bind_net(task_ctx *task, int32_t index) {
    if (index < INVALID_INDEX
        || index >= (int32_t)task->loader->nnet) {
        return ERR_FAILED;
    }
    ATOMIC_SET_RELAXED(&task->evbind, (atomic_t)(INVALID_INDEX == index ? 0 : index + 1));
    return ERR_OK;
}
int32_t task_net_index(task_ctx *task) {
    int32_t bind = (int32_t)ATOMIC_GET_RELAXED(&task->evbind);
    return 0 == bind ? INVALID_INDEX : bind - 1;
}
// 读锁内按句柄查到就投递：读锁挡住 task_ungrab 摘表那一步，能查到就说明 task 还在表里、创建引用
// (task_new 的 ref=1，到 CLOSING 处理里才放)还在，投递之后不会被别人提前释放。
// 查不到时 copy 非 0 就放掉组包时 dup 的副本，copy=0 的载荷不碰、归调用方
static int32_t _task_post_to(loader_ctx *loader, name_t handle, message_ctx *msg, int32_t copy) {
    rwlock_distr_rdlock(&loader->lckmaptasks);
    task_ctx *task = _task_map_get(loader->maptasks, handle);
    if (NULL != task) {
        _task_message_post(task, msg);
    }
    rwlock_distr_runlock(&loader->lckmaptasks);
    if (NULL != task) {
        return ERR_OK;
    }
    if (0 != copy) {
        FREE(msg->data);
    }
    return ERR_FAILED;
}
// 时间轮超时回调：将超时消息推入对应任务队列。走 _task_post_to 不持引用，task 的最后一次释放就不会落在
// 时间轮线程上——Lua task 会在那里 lua_close，期间全进程的定时器都停
static void _task_message_timeout_push(ud_cxt *ud) {
    message_ctx msg = { 0 };
    msg.mtype = MSG_TYPE_TIMEOUT;
    msg.sess = ud->sess;
    msg.data = ud->context;
    (void)_task_post_to(ud->loader, ud->handle, &msg, 0);
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
// 组请求消息（msg 须已清零），copy 时载荷先 dup 一份
static inline void _task_request_msg(message_ctx *msg, task_ctx *src, subtype_t reqtype, uint64_t sess,
                                     void *data, size_t size, int32_t copy) {
    ASSERTAB((NULL != src && 0 != sess) || (NULL == src && 0 == sess), "parameter error");
    msg->mtype = MSG_TYPE_REQUEST;
    msg->subtype = reqtype;
    if (NULL != src) {
        msg->src = src->handle;
        msg->sess = sess;
    } else {
        msg->src = INVALID_TNAME;
    }
    if (NULL != data && 0 != copy) {
        msg->data = dup_zero(data, size);
    } else {
        msg->data = data;
    }
    msg->size = size;
}
// 组响应消息（msg 须已清零），copy 时载荷先 dup 一份
static inline void _task_response_msg(message_ctx *msg, subtype_t reqtype, uint64_t sess,
                                      int32_t erro, void *data, size_t size, int32_t copy) {
    msg->mtype = MSG_TYPE_RESPONSE;
    msg->sess = sess;
    msg->subtype = reqtype;
    msg->erro = erro;
    msg->size = size;
    if (NULL != data && 0 != copy) {
        msg->data = dup_zero(data, size);
    } else {
        msg->data = data;
    }
}
void task_request(task_ctx *dst, task_ctx *src, subtype_t reqtype, uint64_t sess,
                  void *data, size_t size, int32_t copy) {
    message_ctx msg = { 0 };
    _task_request_msg(&msg, src, reqtype, sess, data, size, copy);
    _task_message_post(dst, &msg);
}
void task_response(task_ctx *dst, subtype_t reqtype, uint64_t sess,
                   int32_t erro, void *data, size_t size, int32_t copy) {
    message_ctx msg = { 0 };
    _task_response_msg(&msg, reqtype, sess, erro, data, size, copy);
    _task_message_post(dst, &msg);
}
void task_call(task_ctx *dst, subtype_t reqtype, void *data, size_t size, int32_t copy) {
    task_request(dst, NULL, reqtype, 0, data, size, copy);
}
int32_t task_request_to(loader_ctx *loader, name_t handle, task_ctx *src, subtype_t reqtype, uint64_t sess,
                        void *data, size_t size, int32_t copy) {
    if (INVALID_TNAME == handle) {
        return ERR_FAILED;
    }
    message_ctx msg = { 0 };
    _task_request_msg(&msg, src, reqtype, sess, data, size, copy);
    return _task_post_to(loader, handle, &msg, copy);
}
int32_t task_response_to(loader_ctx *loader, name_t handle, subtype_t reqtype, uint64_t sess,
                         int32_t erro, void *data, size_t size, int32_t copy) {
    if (INVALID_TNAME == handle) {
        return ERR_FAILED;
    }
    message_ctx msg = { 0 };
    _task_response_msg(&msg, reqtype, sess, erro, data, size, copy);
    return _task_post_to(loader, handle, &msg, copy);
}
int32_t task_call_to(loader_ctx *loader, name_t handle, subtype_t reqtype, void *data, size_t size, int32_t copy) {
    return task_request_to(loader, handle, NULL, reqtype, 0, data, size, copy);
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
        shared->data = dup_zero(data, size);
    } else {
        shared->data = data;
    }
    ATOMIC_SET_RELAXED(&shared->ref, valid);
    // 投递 N 条 message：共用 shared，message_clean 走 shared 分支 ref-- 归 0 才 FREE
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
            _task_message_post(dsts[i], &msg);
        }
    }
    return valid;
}
// 读锁挡住 task 释放那一步(要写锁)，锁内查到的指针投递完之前都有效，口径同 _task_post_to
int32_t task_multi_request2(loader_ctx *loader, const name_t *handles, int32_t n, task_ctx *src,
                            subtype_t reqtype, uint64_t sess, void *data, size_t size, int32_t copy,
                            int32_t strict) {
    task_ctx *stk[MULTI_REQUEST2_STACK];
    task_ctx **dsts = stk;
    int32_t i;
    int32_t nfound = 0;
    int32_t nmiss = 0;
    int32_t rtn;
    if (n > MULTI_REQUEST2_STACK) {
        MALLOC(dsts, sizeof(task_ctx *) * (size_t)n);
    }
    rwlock_distr_rdlock(&loader->lckmaptasks);
    for (i = 0; i < n; i++) {
        dsts[i] = (INVALID_TNAME == handles[i]) ? NULL : _task_map_get(loader->maptasks, handles[i]);
        if (NULL != dsts[i]) {
            nfound++;
        } else if (INVALID_TNAME != handles[i]) {
            nmiss++;
        }
    }
    if (0 != strict && 0 != nmiss) {
        rtn = -1;
    } else if (0 == nfound) {
        rtn = 0;
    } else {
        rtn = task_multi_request(dsts, n, src, reqtype, sess, data, size, copy);
    }
    rwlock_distr_runlock(&loader->lckmaptasks);
    if (dsts != stk) {
        FREE(dsts);
    }
    return rtn;
}
void task_multi_call(task_ctx *dsts[], int32_t n, subtype_t reqtype,
                     void *data, size_t size, int32_t copy) {
    (void)task_multi_request(dsts, n, NULL, reqtype, 0, data, size, copy);
}
// 窗口会嵌套(解包时同线程关掉别的连接、推握手结果都会再开一层)：只有最外层清零，内层推的也记在
// 外层账上，开窗失败不算一层。否则内层把外层已推的条数抹掉，外层关窗时漏激活
static void *_task_emit_begin(void *loader, name_t handle) {
    task_ctx *task = task_grab(loader, handle);
    if (NULL != task) {
        int32_t *win = _emit_win_tls();
        if (0 == win[1]++) {
            win[0] = 0;
        }
    }
    return task;
}
static void _task_emit(void *target, message_ctx *msg) {
    int32_t *win = _emit_win_tls();
    // 只收最外层窗口里的不分片 RECV：嵌套窗口(握手结果、关掉别的连接)还在协议状态机中途，
    // 分片与关闭时补的末片交给队列保序。没绑 net 线程的先在这里挡掉，别为它去取线程下标
    if (MSG_TYPE_RECV == msg->mtype
        && 0 == msg->slice
        && 1 == win[1]
        && 0 != ATOMIC_GET_RELAXED(&((task_ctx *)target)->evbind)
        && ERR_OK == _loader_task_try_run((task_ctx *)target, msg)) {
        return;
    }
    msgq_push(&((task_ctx *)target)->qumsg, msg);
    ++win[0];
}
// 一次可读事件解出的多个包统一激活一次：每包都激活的话，worker 可能在还没解完时
// 就排空睡下，同一次事件里唤醒好几回。一条没解出来（半包）就不激活，免得白唤醒
static void _task_emit_end(void *target) {
    task_ctx *task = (task_ctx *)target;
    int32_t *win = _emit_win_tls();
    if (win[0] > 0) {
        _task_message_active(task);
    }
    --win[1];
    task_ungrab(task);
}
static prot_emit g_task_emit = { _task_emit_begin, _task_emit, _task_emit_end };
prot_emit *_task_net_emit(void) {
    return &g_task_emit;
}
int32_t task_accept_group(task_ctx *head, const name_t *handles, uint16_t n) {
    task_ctx *member;
    uint16_t i;
    uint16_t nmember = 0;
    int32_t bound;
    if (NULL != head->acp_group
        || n != head->loader->nnet) {
        return ERR_FAILED;
    }
    for (i = 0; i < n; i++) {
        if (INVALID_TNAME == handles[i]) {
            continue;// 空位：这个线程不收这一组的连接
        }
        member = task_grab(head->loader, handles[i]);
        if (NULL == member) {
            return ERR_FAILED;
        }
        bound = task_net_index(member);
        task_ungrab(member);
        if ((int32_t)i != bound) {
            return ERR_FAILED;
        }
        nmember++;
    }
    if (0 == nmember) {
        return ERR_FAILED;
    }
    MALLOC(head->acp_group, sizeof(name_t) * n);
    memcpy(head->acp_group, handles, sizeof(name_t) * n);
    return ERR_OK;
}
// 组头监听到新连接：交给连接所在 net 线程的组员；组头已退出就拒收。
// listen 只在有组员的线程上收，落到空位只会是误用，同样拒收
static int32_t _task_acp_pick(sock_ctx *sk, ud_cxt *ud) {
    task_ctx *head = task_grab(ud->loader, ud->handle);
    if (NULL == head) {
        return ERR_FAILED;
    }
    ud->handle = head->acp_group[sk->index];
    task_ungrab(head);
    return INVALID_TNAME == ud->handle ? ERR_FAILED : ERR_OK;
}
// 组员表里有组员的线程下标，给 ev_listen 只在这些线程上收；满员返回 NULL(全部线程)。非 NULL 时调用方 FREE
static int32_t *_task_acp_thrs(task_ctx *head, uint32_t *nthrs) {
    uint16_t nnet = head->loader->nnet;
    uint16_t i;
    int32_t *thrs;
    *nthrs = 0;
    for (i = 0; i < nnet; i++) {
        if (INVALID_TNAME != head->acp_group[i]) {
            (*nthrs)++;
        }
    }
    if (nnet == *nthrs) {
        *nthrs = 0;
        return NULL;
    }
    MALLOC(thrs, sizeof(int32_t) * (*nthrs));
    *nthrs = 0;
    for (i = 0; i < nnet; i++) {
        if (INVALID_TNAME != head->acp_group[i]) {
            thrs[(*nthrs)++] = (int32_t)i;
        }
    }
    return thrs;
}
static int32_t _task_acp_accept(ev_ctx *ev, sock_ctx *sk, ud_cxt *ud) {
    (void)ev;
    return _task_acp_pick(sk, ud);
}
// 同上，listen 时要了 NETEV_ACCEPT 的那种：换完目标再照常推 ACCEPT
static int32_t _task_acp_accept_emit(ev_ctx *ev, sock_ctx *sk, ud_cxt *ud) {
    if (ERR_OK != _task_acp_pick(sk, ud)) {
        return ERR_FAILED;
    }
    return prots_net_accept(ev, sk, ud);
}
int32_t task_listen(task_ctx *task, pack_type pktype, struct evssl_ctx *evssl,
    const char *ip, uint16_t port, uint64_t *id, int32_t netev) {
    ud_cxt ud = { 0 };
    int32_t *thrs = NULL;
    uint32_t nthrs = 0;
    int32_t rtn;
    ud.pktype = pktype;
    ud.handle = task->handle;
    ud.loader = task->loader;
    cbs_ctx cbs = { 0 };
    if (BIT_CHECK(netev, NETEV_ACCEPT)) {
        cbs.acp_cb = prots_net_accept;
    }
    if (BIT_CHECK(netev, NETEV_PEERADDR)) {
        BIT_SET(ud.flags, UD_PEERADDR);
    }
    if (BIT_CHECK(netev, NETEV_SESS)) {
        BIT_SET(ud.flags, UD_SESS);
    }
    if (NULL != task->acp_group) {
        cbs.acp_cb = BIT_CHECK(netev, NETEV_ACCEPT) ? _task_acp_accept_emit : _task_acp_accept;
        thrs = _task_acp_thrs(task, &nthrs);
    }
    if (BIT_CHECK(netev, NETEV_SEND)) {
        cbs.s_cb = prots_net_send;
    }
    if (NULL != evssl
        || BIT_CHECK(netev, NETEV_AUTHSSL)) {
        cbs.exch_cb = prots_net_ssl_exchanged;
    }
    cbs.r_cb = prots_net_recv;
    cbs.c_cb = prots_net_close;
    cbs.ud_free = prots_udfree;
    rtn = ev_listen(&task->loader->netev, evssl, ip, port, &cbs, &ud, thrs, nthrs, id);
    FREE(thrs);
    return rtn;
}
int32_t task_connect(task_ctx *task, pack_type pktype, struct evssl_ctx *evssl,
    const char *ip, uint16_t port, int32_t netev, void *extra,
    int32_t setsess, sock_ctx *sk) {
    ud_cxt ud = { 0 };
    ud.pktype = pktype;
    ud.handle = task->handle;
    ud.loader = task->loader;
    ud.context = extra;
    cbs_ctx cbs = { 0 };
    if (BIT_CHECK(netev, NETEV_SEND)) {
        cbs.s_cb = prots_net_send;
    }
    if (NULL != evssl
        || BIT_CHECK(netev, NETEV_AUTHSSL)) {
        cbs.exch_cb = prots_net_ssl_exchanged;
    }
    cbs.conn_cb = prots_net_connect;
    cbs.r_cb = prots_net_recv;
    cbs.c_cb = prots_net_close;
    cbs.ud_free = prots_udfree;
    // 绑了 net 线程的，连接落在同一条上，收发不再跨线程；没绑的是 INVALID_INDEX，照旧按 fd 分配
    return ev_connect(&task->loader->netev, evssl, ip, port, &cbs, &ud, setsess, task_net_index(task), sk);
}
int32_t task_udp(task_ctx *task, pack_type pktype, const char *ip, uint16_t port, int32_t reuse, sock_ctx *sk) {
    ud_cxt ud = { 0 };
    ud.pktype = pktype;
    ud.handle = task->handle;
    ud.loader = task->loader;
    if (0 != reuse) {
        BIT_SET(ud.flags, UD_REUSEADDR);
    }
    cbs_ctx cbs = { 0 };
    cbs.c_cb = prots_net_close;
    cbs.rf_cb = prots_net_recvfrom;
    cbs.ud_free = prots_udfree;
    return ev_udp(&task->loader->netev, ip, port, &cbs, &ud, task_net_index(task), sk);// 落线程同 task_connect
}
void task_set_priority(task_ctx *task, int32_t priority) {
    int32_t p = priority;
    if (p < 0) {
        p = 0;
    } else if (p > TASK_PRIORITY_MAX) {
        p = TASK_PRIORITY_MAX;
    }
    ATOMIC_SET_RELAXED(&task->priority, (atomic_t)p);
}
int32_t task_get_priority(task_ctx *task) {
    return (int32_t)ATOMIC_GET(&task->priority);
}
void task_set_request_timeout(task_ctx *task, uint32_t ms) {
    if (0 == ms) {
        return;
    }
    ATOMIC_SET_RELAXED(&task->timeout_request, ms > TASK_TIMEOUT_MAX ? TASK_TIMEOUT_MAX : ms);
}
uint32_t task_get_request_timeout(task_ctx *task) {
    return (uint32_t)ATOMIC_GET(&task->timeout_request);
}
void task_set_connect_timeout(task_ctx *task, uint32_t ms) {
    if (0 == ms) {
        return;
    }
    ATOMIC_SET_RELAXED(&task->timeout_connect, ms > TASK_TIMEOUT_MAX ? TASK_TIMEOUT_MAX : ms);
}
uint32_t task_get_connect_timeout(task_ctx *task) {
    return (uint32_t)ATOMIC_GET(&task->timeout_connect);
}
void task_set_netread_timeout(task_ctx *task, uint32_t ms) {
    if (0 == ms) {
        return;
    }
    ATOMIC_SET_RELAXED(&task->timeout_netread, ms > TASK_TIMEOUT_MAX ? TASK_TIMEOUT_MAX : ms);
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
void task_watch_closed(task_ctx *task, _net_close_watch_cb cb, void *arg) {
    close_watch w = { cb, arg };
    cwatch_arr_push_back(&task->watchers, &w);
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
void task_round_ended(task_ctx *task, _task_round_end_cb _round_end) {
    task->_round_end = _round_end;
}
