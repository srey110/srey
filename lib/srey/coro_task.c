#include "srey/coro_task.h"
#include "protocol/prots.h"

typedef void (*_coro_msg_handler_t)(task_dispatch_arg *arg);
// 协程 task 的 task->arg：调度器加上注册时传入的业务参数
typedef struct coro_task_ctx {
    coro_ctx *co;          // 调度器，owner 是 task 本身
    void *arg;             // 业务参数
    free_cb _arg_free;     // 业务参数释放回调
}coro_task_ctx;
// _coro_timeout_monitor 交给 coro_expire 的 ud：一条 TIMEOUT 消息给所有到期者复用
typedef struct coro_task_timeout {
    task_ctx *task;
    message_ctx msg;
}coro_task_timeout;

static size_t _stack_size; // 协程栈大小，启动期由 coro_task_stack 设定一次，之后只读

// 新协程入口：消息在调用方栈上(loader 批量缓冲)，第一次挂起后即失效，先拷到协程栈上
static void _coro_task_run(void *owner, void *payload) {
    message_ctx msg = *(message_ctx *)payload;
    _message_run((task_ctx *)owner, &msg);
}
// 协程开跑前加引用，保证挂起期间 task 不会被释放
static void _coro_task_before_run(void *owner) {
    task_incref((task_ctx *)owner);
}
static void _coro_task_after_run(void *owner) {
    task_ungrab((task_ctx *)owner);
}
static const char *_coro_task_tagstr(int32_t tag) {
    return message_str((msg_type)tag);
}
static const coro_hooks _coro_task_hooks = {
    _coro_task_run, _coro_task_before_run, _coro_task_after_run, _coro_task_tagstr
};
// 分发路径专用：task 必是协程 task，不再判型
static inline coro_ctx *_coro_co(task_ctx *task) {
    return ((coro_task_ctx *)task->arg)->co;
}
// 统一唤醒尾部：找到匹配等待者则唤醒，唤醒后清理消息；否则 warn!=0 时先告警(未找到即逻辑异常，与是否新建协程无关)，
// 再按 miss_create 决定新建协程处理(!=0)还是丢弃(==0，TIMEOUT 专属：正常情况下已被正常路径消费)
static inline void _coro_dispatch(task_dispatch_arg *arg, int32_t miss_create, int32_t warn) {
    coro_ctx *co = _coro_co(arg->task);
    if (0 == arg->msg->sess) {
        coro_spawn(co, arg->msg);
        return;
    }
    if (ERR_OK == coro_wake(co, arg->msg->sess, (int32_t)arg->msg->mtype, arg->msg)) {
        message_clean(arg->msg);
        return;
    }
    if (warn) {
        LOG_WARN("can't find session, maybe logic error. msg_type %d.", (int32_t)arg->msg->mtype);
    }
    if (miss_create) {
        coro_spawn(co, arg->msg);
    }
}
// 新建协程处理本条消息
static void _coro_handle_spawn(task_dispatch_arg *arg) {
    coro_spawn(_coro_co(arg->task), arg->msg);
}
static void _coro_handle_timeout(task_dispatch_arg *arg) {
    _coro_dispatch(arg, 0, 1);
}
// CONNECT / SSLEXCHANGED / HANDSHAKED / RECVFROM / RESPONSE 共用：找不到等待者静默新建协程，不告警。
// 各 mtype 的语义差异见下面分发表逐行的注释
static void _coro_handle_miss_create(task_dispatch_arg *arg) {
    _coro_dispatch(arg, 1, 0);
}
// 处理数据接收消息：sess==0 或协议不允许 resume 则新建协程，否则唤醒等待的协程
static void _coro_handle_recved(task_dispatch_arg *arg) {
    if (0 == arg->msg->sess
        || ERR_OK != prots_may_resume(arg->msg->subtype, arg->msg->data)) {
        _coro_handle_spawn(arg);
        return;
    }
    _coro_dispatch(arg, 1, 0);
}
// 处理连接关闭消息：sess(连接类即 skid)下全部等待者不看 mtype 一律唤醒，再新建协程跑关闭回调。
// NEVERCONN 的合成 CLOSE 只为唤醒等待方，不触发 on_close 观察者（见 close_type）。
// CLOSE 不带数据，唤醒后不必清理（见 protocol/prots.c 的 _message_data_kind）
static void _coro_handle_closed(task_dispatch_arg *arg) {
    coro_wake_all(_coro_co(arg->task), arg->msg->sess, arg->msg,
                  CLOSE_TYPE_NEVERCONN != arg->msg->erro ? arg->msg : NULL);
}
// 给每个到期的等待者造 TIMEOUT 消息：同一条复用，每个到期者只用到它下次挂起
static void *_coro_timeout_msg(void *ud, uint64_t sess, int32_t tag) {
    coro_task_timeout *tmo = (coro_task_timeout *)ud;
    LOG_INFO("task %s message type %d session %"PRIu64" timeout.",
             _NAME_OR(tmo->task->name), tag, sess);
    tmo->msg.sess = sess;
    return &tmo->msg;
}
// 定期（每 1 秒）唤醒已到期的挂起协程并注入超时消息。超时绝大多数等不到触发就被正常响应摘走，
// 全进程的时间轮上只占这一个节点；要准的定时（coro_sleep）才各挂一个时间轮节点
static void _coro_timeout_monitor(task_ctx *task, uint64_t sess) {
    (void)sess;
    coro_task_timeout tmo;
    ZERO(&tmo, sizeof(tmo));
    tmo.task = task;
    tmo.msg.mtype = MSG_TYPE_TIMEOUT;
    coro_expire(_coro_co(task), _coro_timeout_msg, &tmo);
    task_timeout(task, 0, 1 * 1000, _coro_timeout_monitor);
}
static void _coro_handle_startup(task_dispatch_arg *arg) {
    task_timeout(arg->task, 0, 1 * 1000, _coro_timeout_monitor);
    _coro_handle_spawn(arg);
}
static void _coro_handle_closing(task_dispatch_arg *arg) {
    coro_ctx *co = _coro_co(arg->task);
    coro_spawn(co, arg->msg);
    if (coro_suspended(co) > 0) {
        LOG_WARN("task %s yield %d.", _NAME_OR(arg->task->name), coro_suspended(co));
    }
}
static const _coro_msg_handler_t _coro_msg_handlers[MSG_TYPE_ALL] = {
    [MSG_TYPE_STARTUP]      = _coro_handle_startup,// 新建
    [MSG_TYPE_CLOSING]      = _coro_handle_closing,// 新建
    [MSG_TYPE_TIMEOUT]      = _coro_handle_timeout,// 新建或唤醒
    [MSG_TYPE_ACCEPT]       = _coro_handle_spawn,// 新建
    [MSG_TYPE_CONNECT]      = _coro_handle_miss_create, // 连接建立；未找到静默新建
    [MSG_TYPE_SSLEXCHANGED] = _coro_handle_miss_create, // SSL 握手；未找到静默新建
    [MSG_TYPE_HANDSHAKED]   = _coro_handle_miss_create, // 应用层握手；未找到静默新建
    [MSG_TYPE_RECV]         = _coro_handle_recved,// sess==0 或协议不允许 创建；未找到新建，否则唤醒
    [MSG_TYPE_SEND]         = _coro_handle_spawn,// 新建
    [MSG_TYPE_CLOSE]        = _coro_handle_closed,// sess直接赋值skid,尝试唤醒所有
    [MSG_TYPE_RECVFROM]     = _coro_handle_miss_create,// sess 0新建；未找到静默新建，不告警(UDP 不保证顺序与送达，迟到/孤儿包是常态)
    [MSG_TYPE_REQUEST]      = _coro_handle_spawn,// 新建
    [MSG_TYPE_RESPONSE]     = _coro_handle_miss_create,// 未找到静默新建，不告警(task_multi_request 广播的 N 个响应本就没有等待者；
                                                       // 真孤儿由请求方的 coro_request 超时告警报出)
};
// 协程任务的消息分发总入口：按消息类型路由，末尾把本轮 fork 出来的协程全部起掉
static void _coro_message_dispatch(task_dispatch_arg *arg) {
    if (arg->msg->mtype > MSG_TYPE_NONE
        && arg->msg->mtype < MSG_TYPE_ALL
        && NULL != _coro_msg_handlers[arg->msg->mtype]) {
        _coro_msg_handlers[arg->msg->mtype](arg);
    }
    coro_fork_drain(_coro_co(arg->task));
}
// 先放业务参数再放调度器：业务析构里的 *_quit / *_ping 头一件事就是读自己的 serial，
// 没显式释放的 serial 由 coro_free 兜底
static void _coro_task_free(void *arg) {
    coro_task_ctx *ct = (coro_task_ctx *)arg;
    if (NULL != ct->_arg_free
        && NULL != ct->arg) {
        ct->_arg_free(ct->arg);
    }
    coro_free(ct->co);
    FREE(ct);
}
// 判型：TASK_LUA 的 task->arg 是 ltask_ctx *、带 arg 的 TASK_NORMAL 是业务自己的指针，按 coro_task_ctx * 解引用就是读错偏移。
// 收 NULL 是有意的：调用方常在"连接还没建起来"的清理路径上问，那时手里的 task 可能还没填
static inline coro_task_ctx *_coro_task_get(task_ctx *task) {
    if (NULL == task
        || TASK_MCO != task_get_type(task)
        || NULL == task->arg) {
        return NULL;
    }
    return (coro_task_ctx *)task->arg;
}
void coro_task_stack(size_t stack_size) {
    size_t fit = coro_stack_fit(stack_size);
    if (0 != stack_size && fit > stack_size) {
        LOG_WARN("stacksize %zu too small, use min %zu.", stack_size, fit);
    } else if (fit < stack_size) {
        LOG_WARN("stacksize %zu too large, use max %zu.", stack_size, fit);
    }
    _stack_size = fit;
}
task_ctx *coro_task_register(loader_ctx *loader, const char *name, uint32_t quecap,
                             _task_startup_cb _startup, _task_closing_cb _closing,
                             free_cb _argfree, void *arg) {
    coro_task_ctx *ct;
    CALLOC(ct, 1, sizeof(coro_task_ctx));
    ct->arg = arg;
    ct->_arg_free = _argfree;
    task_ctx *task = task_new(loader, name, quecap, _coro_message_dispatch, _coro_task_free, ct);
    ct->co = coro_new(&_coro_task_hooks, task, _stack_size);
    task->type = TASK_MCO;
    if (ERR_OK != task_register(task, _startup, _closing)) {
        task_free(task);
        return NULL;
    }
    return task;
}
coro_ctx *coro_task_co(task_ctx *task) {
    coro_task_ctx *ct = _coro_task_get(task);
    return NULL == ct ? NULL : ct->co;
}
void *coro_get_arg(task_ctx *task) {
    coro_task_ctx *ct = _coro_task_get(task);
    return NULL == ct ? NULL : ct->arg;
}
int32_t coro_incoro(task_ctx *task) {
    coro_task_ctx *ct = _coro_task_get(task);
    return NULL == ct ? 0 : coro_running(ct->co);
}
int32_t coro_sync(task_ctx *task, sock_ctx *sk) {
    return ev_ud_sess(&task->loader->netev, sk, sk->skid);
}
// 挂起当前协程并等待下一条匹配消息；keep 由消息类型定，只在新建 sess 条目时生效
// 返回唤醒方给的消息指针（在唤醒方栈上），在本协程下次挂起前有效
message_ctx *_coro_wait(task_ctx *task, uint64_t sess, msg_type mtype, uint32_t ms) {
    message_ctx *msg = (message_ctx *)coro_wait(_coro_co(task), sess, (int32_t)mtype, message_may_keep(mtype), ms);
    /* 所有消息类型均保证 msg.sess 与注册 key 一致
     * （CONNECT/SSL/CLOSE 系 skid，TIMEOUT 系到期者的 sess，RESPONSE/RECV 系传入 sess），
     * 若此断言触发，说明分发逻辑有 bug。*/
    ASSERTAB(sess == msg->sess, "different session");
    return msg;
}
void coro_sleep(task_ctx *task, uint32_t ms) {
    if (0 == ms) {
        return;
    }
    uint64_t sess = createid();
    task_timeout(task, sess, ms, NULL);
    _coro_wait(task, sess, MSG_TYPE_TIMEOUT, 0);
}
void *coro_request(task_ctx *dst, task_ctx *src,
                   subtype_t rtype, void *data, size_t size, int32_t copy,
                   int32_t *erro, size_t *lens) {
    uint64_t sess = createid();
    task_request(dst, src, rtype, sess, data, size, copy);
    message_ctx *msg = _coro_wait(src, sess, MSG_TYPE_RESPONSE, task_get_request_timeout(src));
    if (MSG_TYPE_TIMEOUT == msg->mtype) {
        *erro = ERR_FAILED;
        LOG_WARN("dst %s src %s request type %d timeout, session %"PRIu64".", _NAME_OR(dst->name), _NAME_OR(src->name), rtype, sess);
        return NULL;
    }
    *erro = msg->erro;
    SET_PTR(lens, msg->size);
    return msg->data;
}
// 等一条指定类型的消息:超时则关连接并告警,连接已关则静默,两种都返 NULL。
// 四个等待点(ssl exchange / handshake / connect / recv)只差 mtype、超时值与告警里的动作名,
// tag 仅进日志。返回的指针在本协程下次 _coro_wait 前有效。
// CLOSE 分支有意不告警:对端关连接是正常事件,而调用方是每命令一轮的循环,一条连接断掉能刷出几十条
static inline message_ctx *_coro_wait_msg(task_ctx *task, sock_ctx *sk,
                                   msg_type mtype, uint32_t ms, const char *tag) {
    // 连接已 teardown 就别挂上去:等不到唤醒,只会挂满超时再对 INVALID_SOCK 调一次 ev_close、
    // 打一条假的 timeout 日志。四个 coro_* 入口都经本函数,守卫收在这里一处
    if (sock_is_invalid(sk)) {
        return NULL;
    }
    message_ctx *msg = _coro_wait(task, sk->skid, mtype, ms);
    if (MSG_TYPE_TIMEOUT == msg->mtype) {
        ev_close(&task->loader->netev, sk);
        LOG_WARN("task %s, %s timeout, skid %"PRIu64".", _NAME_OR(task->name), tag, sk->skid);
        return NULL;
    }
    if (MSG_TYPE_CLOSE == msg->mtype) {
        return NULL;
    }
    return msg;
}
// 等待 SSL 交换完成消息，失败的处理见 _coro_wait_msg
static inline int32_t _wait_ssl_exchanged(task_ctx *task, sock_ctx *sk) {
    return NULL == _coro_wait_msg(task, sk, MSG_TYPE_SSLEXCHANGED,
                                  task_get_netread_timeout(task), "ssl exchange")
           ? ERR_FAILED : ERR_OK;
}
int32_t coro_ssl_exchange(task_ctx *task, sock_ctx *sk, int32_t client, struct evssl_ctx *evssl) {
    if (ERR_OK != ev_ssl(&task->loader->netev, sk, client, evssl)) {
        return ERR_FAILED;
    }
    return _wait_ssl_exchanged(task, sk);
}
void *coro_handshaked(task_ctx *task, sock_ctx *sk, int32_t *err, size_t *size) {
    message_ctx *msg = _coro_wait_msg(task, sk, MSG_TYPE_HANDSHAKED,
                                      task_get_netread_timeout(task), "handshake");
    if (NULL == msg) {
        *err = ERR_FAILED;
        return NULL;
    }
    *err = msg->erro;
    SET_PTR(size, msg->size);
    return msg->data;
}
int32_t coro_wait_connect(task_ctx *task, sock_ctx *sk, struct evssl_ctx *evssl) {
    message_ctx *msg = _coro_wait_msg(task, sk, MSG_TYPE_CONNECT,
                                      task_get_connect_timeout(task), "connect");
    if (NULL == msg) {
        return ERR_FAILED;
    }
    if (ERR_OK != msg->erro) {
        LOG_WARN("task %s, connect error, skid %"PRIu64".", _NAME_OR(task->name), sk->skid);
        return ERR_FAILED;
    }
    if (NULL != evssl) {
        if (ERR_OK != _wait_ssl_exchanged(task, sk)) {
            return ERR_FAILED;
        }
    }
    return ERR_OK;
}
int32_t coro_connect(task_ctx *task, pack_type pktype,
                     struct evssl_ctx *evssl, const char *ip, uint16_t port,
                     int32_t netev, void *extra,
                     sock_ctx *sk) {
    if (ERR_OK != task_connect(task, pktype, evssl, ip, port, netev, extra, 1, sk)) {
        LOG_WARN("task: %s, connect %s:%d error.", _NAME_OR(task->name), ip, port);
        return ERR_FAILED;
    }
    return coro_wait_connect(task, sk, evssl);
}
void coro_close(task_ctx *task, sock_ctx *sk) {
    if (sock_is_invalid(sk)) {
        return;
    }
    ev_close(&task->loader->netev, sk);
    _coro_wait(task, sk->skid, MSG_TYPE_CLOSE, task_get_netread_timeout(task));
}
// 等待指定连接的下一条接收消息，失败的处理与指针有效期见 _coro_wait_msg
static inline message_ctx *_coro_wait_recved(task_ctx *task, sock_ctx *sk) {
    return _coro_wait_msg(task, sk, MSG_TYPE_RECV, task_get_netread_timeout(task), "netread");
}
void *coro_send(task_ctx *task, sock_ctx *sk, void *data, size_t len, size_t *size, int32_t copy) {
    if (ERR_OK != ev_send(&task->loader->netev, sk, data, len, copy)) {
        return NULL;
    }
    message_ctx *msg = _coro_wait_recved(task, sk);
    if (NULL == msg) {
        return NULL;
    }
    SET_PTR(size, msg->size);
    return msg->data;
}
// 只收不发:一次请求产生多个响应时续读后续包(如 MySQL 多结果集)。
// 与 coro_send 一样,返回的指针只在本协程下次挂起前有效
void *coro_recv(task_ctx *task, sock_ctx *sk, size_t *size) {
    message_ctx *msg = _coro_wait_recved(task, sk);
    if (NULL == msg) {
        return NULL;
    }
    SET_PTR(size, msg->size);
    return msg->data;
}
void *coro_slice(task_ctx *task, sock_ctx *sk, size_t *size, int32_t *end) {
    *end = 0;// 任何失败路径都不再往下写,统一在此归零,保证"返回 NULL 时 end 为 0"
    message_ctx *msg = _coro_wait_recved(task, sk);
    if (NULL == msg) {
        return NULL;
    }
    // 非分片完整消息(slice==0)也视为末片,通用客户端 while(!end) 循环不会误判还有后续分片而挂到超时
    *end = (PROT_SLICE_END == msg->slice || 0 == msg->slice) ? 1 : 0;
    SET_PTR(size, msg->size);
    return msg->data;
}
// 同步收发前须由调用方显式 coro_sync 一次(与 TCP 服务端 accept 连接同约定,见文件头注释)；
// 之后同一 skid 上可连续多次调用；并发多次调用（不等上一次响应返回）时两次响应按到达顺序 FIFO
// 匹配给两次调用，若网络乱序仍可能与发送顺序不一致——UDP 协议本身无法避免的限制
void *coro_sendto(task_ctx *task, sock_ctx *sk,
                  const char *ip, const uint16_t port,
                  void *data, size_t len, size_t *size, int32_t copy) {
    if (ERR_OK != ev_sendto(&task->loader->netev, sk, ip, port, data, len, copy)) {
        LOG_WARN("task %s, sendto error, skid %"PRIu64".", _NAME_OR(task->name), sk->skid);
        return NULL;
    }
    message_ctx *msg = _coro_wait(task, sk->skid, MSG_TYPE_RECVFROM, task_get_netread_timeout(task));
    if (MSG_TYPE_TIMEOUT == msg->mtype) {
        LOG_WARN("task %s, sendto timeout, skid %"PRIu64".", _NAME_OR(task->name), sk->skid);
        return NULL;
    }
    if (MSG_TYPE_CLOSE == msg->mtype) {
        return NULL;
    }
    recvfrom_ctx *rfmsg = msg->data;
    SET_PTR(size, rfmsg->len);
    return rfmsg->data;
}
