#include "srey/coro.h"
#include "containers/hashmap.h"
#include "containers/heap.h"
#include "utils/timer.h"
#define MINICORO_IMPL
#include "srey/minicoro.h"

// 超时堆节点：嵌入最小堆，存储过期时间和关联 session
typedef struct timeout_entry {
    heap_node hnode;     // 必须在首位，供 UPCAST 使用
    uint64_t  timeout;   // 到期时间戳（毫秒）
    uint64_t  sess;      // 关联的 session ID
} timeout_entry;
// 从堆节点指针还原 timeout_entry 指针
#define _TE_FROM_HNODE(n) UPCAST(n, timeout_entry, hnode)

// 单个挂起协程的等待信息
typedef struct coro_info {
    msg_type       mtype;   // 期望唤醒的消息类型
    mco_coro      *co;      // 挂起的协程对象
    uint64_t       timeout; // 绝对到期时间（毫秒），0 表示无超时
    timeout_entry *te;      // 非 NULL 表示已注册到超时堆
}coro_info;
QUEUE_DECL(coro_info, qu_coinfo)
// session 到挂起协程的映射节点
typedef struct coro_sess {
    int32_t disposable; // 1 = 一次性（用完即删），0 = 持久（连接生命期内可复用）
    uint64_t sess;      // session ID（一次性时为请求ID，持久时为 skid）
    union {
        coro_info     coinfo;   // disposable == 1：单个挂起协程
        qu_coinfo_ctx qucoinfo; // disposable == 0：挂起协程队列（内联，无额外 malloc）
    };
}coro_sess;
// 协程任务的运行时上下文，挂在 task->arg
typedef struct coro_ctx {
    int32_t nyield;          // 当前挂起（yield）中的协程数量
    mco_coro *curco;         // 正在运行的协程指针
    struct hashmap *mapco;   // sess → coro_sess 哈希映射
    qu_ptr_ctx qucopool;     // 空闲协程对象池
    timer_ctx timer;         // 用于获取当前毫秒时间戳
    heap_ctx timeout_heap;   // 按到期时间排序的最小堆，O(1) 检查最早超时
}coro_ctx;

static mco_desc _coro_desc; // 全局协程描述符，由 coro_desc_init 初始化

// 最小堆比较函数：timeout 小的优先（堆顶是最早到期的）
static int _timeout_cmp(const heap_node *lhs, const heap_node *rhs) {
    return _TE_FROM_HNODE(lhs)->timeout < _TE_FROM_HNODE(rhs)->timeout;
}
// 计算 coro_sess 在哈希表中的哈希值（基于 sess 字段）
static uint64_t _map_cosess_hash(const void *item, uint64_t seed0, uint64_t seed1) {
    (void)seed0;
    (void)seed1;
    return hash((const char *)&(((coro_sess *)item)->sess), sizeof(((coro_sess *)item)->sess));
}
// 比较两个 coro_sess 节点（按 sess 升序）
static int _map_cosess_compare(const void *a, const void *b, void *ud) {
    (void)ud;
    uint64_t sa = ((const coro_sess *)a)->sess;
    uint64_t sb = ((const coro_sess *)b)->sess;
    return (sa < sb) ? -1 : (sa > sb) ? 1 : 0;
}
// 创建 timeout_entry 并插入超时堆，返回堆节点指针（用于后续删除）
static timeout_entry *_te_insert(coro_ctx *coctx, uint64_t timeout, uint64_t sess) {
    timeout_entry *te;
    MALLOC(te, sizeof(timeout_entry));
    te->hnode.parent = te->hnode.left = te->hnode.right = NULL;
    te->timeout = timeout;
    te->sess    = sess;
    heap_insert(&coctx->timeout_heap, &te->hnode);
    return te;
}
// 将挂起的协程注册到 mapco，disposable=1 为一次性，0 为持久（可排队多个）
static void _map_cosess_set(task_ctx *task, int32_t disposable, mco_coro *coro, uint64_t sess, msg_type mtype, uint32_t ms) {
    coro_ctx *coctx = task->arg;
    coro_sess key;
    key.sess = sess;
    if (disposable) {
        coro_sess cosess;
        cosess.disposable = 1;
        cosess.sess = sess;
        cosess.coinfo.te = NULL;
        cosess.coinfo.timeout = ms > 0 ? timer_cur_ms(&coctx->timer) + ms : 0;
        cosess.coinfo.co = coro;
        cosess.coinfo.mtype = mtype;
        ASSERTAB(NULL == hashmap_set(coctx->mapco, &cosess), "repeat session");
        if (ms > 0) {
            /* hashmap_set 已拷贝 cosess；取真实存储位置设 te */
            coro_sess *stored = (coro_sess *)hashmap_get(coctx->mapco, &key);
            stored->coinfo.te = _te_insert(coctx, cosess.coinfo.timeout, sess);
        }
        return;
    }
    /* non-disposable: inline qu_coinfo_ctx, no heap allocation for the queue header */
    coro_info coinfo;
    coinfo.te = NULL;
    coinfo.timeout = ms > 0 ? timer_cur_ms(&coctx->timer) + ms : 0;
    coinfo.co = coro;
    coinfo.mtype = mtype;
    coro_sess *cofind = (coro_sess *)hashmap_get(coctx->mapco, &key);
    if (NULL != cofind) {
        /* entry already exists: push directly into the inline queue */
        qu_coinfo_push(&cofind->qucoinfo, &coinfo);
        if (ms > 0) {
            coro_info *last = qu_coinfo_at(&cofind->qucoinfo,
                                           qu_coinfo_size(&cofind->qucoinfo) - 1);
            last->te = _te_insert(coctx, coinfo.timeout, sess);
        }
    } else {
        /* new entry: init inline queue on the stack, hashmap_set copies the whole struct */
        coro_sess cosess;
        cosess.disposable = 0;
        cosess.sess = sess;
        qu_coinfo_init(&cosess.qucoinfo, 0);   /* allocates cosess.qucoinfo.ptr on heap */
        qu_coinfo_push(&cosess.qucoinfo, &coinfo);
        hashmap_set(coctx->mapco, &cosess);
        /* hashmap_set copies cosess (struct copy); qucoinfo.ptr is now owned by the stored copy.
         * cosess goes out of scope but the heap array lives on through the stored copy. */
        if (ms > 0) {
            coro_sess *stored = (coro_sess *)hashmap_get(coctx->mapco, &key);
            coro_info *last = qu_coinfo_at(&stored->qucoinfo,
                                           qu_coinfo_size(&stored->qucoinfo) - 1);
            last->te = _te_insert(coctx, coinfo.timeout, sess);
        }
    }
}
// 从 mapco 查找匹配 sess 和 mtype 的挂起协程节点
// 返回哈希表内部存储的直接指针，调用方在使用完毕前不得调用 _map_cosess_delete
static coro_sess *_map_cosess_get(coro_ctx *coctx, uint64_t sess, msg_type mtype) {
    coro_sess key;
    key.sess = sess;
    coro_sess *cofind = (coro_sess *)hashmap_get(coctx->mapco, &key);
    if (NULL == cofind) {
        return NULL;
    }
    if (cofind->disposable) {
        if (mtype != cofind->coinfo.mtype && MSG_TYPE_CLOSE != mtype) {
            return NULL;
        }
    } else {
        coro_info *coinfo = qu_coinfo_peek(&cofind->qucoinfo);
        if (NULL == coinfo) {
            return NULL;
        }
        if (mtype != coinfo->mtype && MSG_TYPE_CLOSE != mtype) {
            return NULL;
        }
    }
    return cofind;
}
// 从 mapco 中删除指定 sess 的记录
static void _map_cosess_delete(coro_ctx *coctx, uint64_t sess) {
    coro_sess key;
    key.sess = sess;
    hashmap_delete(coctx->mapco, &key);
}
// 从 coro_sess 中取出协程对象，并在必要时从超时堆中移除对应条目
static inline mco_coro *_get_mco(task_ctx *task, coro_sess *cosess) {
    coro_ctx *coctx = (coro_ctx *)task->arg;
    coro_info *coinfo;
    mco_coro  *co;
    if (cosess->disposable) {
        coinfo = &cosess->coinfo;
        co     = coinfo->co;
    } else {
        coinfo = qu_coinfo_pop(&cosess->qucoinfo);  /* inline queue: pass address */
        if (NULL == coinfo) {
            return NULL;
        }
        co = coinfo->co;
    }
    /* 协程在超时前已被正常唤醒：从堆中删除对应超时节点 */
    if (NULL != coinfo->te) {
        heap_remove(&coctx->timeout_heap, &coinfo->te->hnode);
        FREE(coinfo->te);
    }
    return co;
}
// 协程主循环：每次 resume 后弹出分发参数指针，执行消息处理，结束后归还协程到对象池
static void _mco_cb(mco_coro *coro) {
    mco_result rtn;
    for (;;) {
        rtn = mco_yield(coro);
        ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
        // 弹出 8 字节指针并在协程栈上复制一份，保证 arg.fd/arg.skid 在整个生命期内有效
        task_dispatch_arg *argp;
        rtn = mco_pop(coro, &argp, sizeof(argp));
        ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
        task_dispatch_arg arg = *argp;             // 在协程栈上保存一份副本
        task_incref(arg.task); // 保证 _message_run 在 yield 后 task 不会被释放
        _message_run(arg.task, &arg.msg);
        qu_ptr_push(&((coro_ctx *)arg.task->arg)->qucopool, (void **)&coro);
        task_ungrab(arg.task);
    }
}
void coro_desc_init(size_t stack_size) {
    _coro_desc = mco_desc_init(_mco_cb, stack_size);
}
// 初始化协程任务运行时上下文
static coro_ctx *_coro_ctx_init(void) {
    coro_ctx *coctx;
    CALLOC(coctx, 1, sizeof(coro_ctx));
    qu_ptr_init(&coctx->qucopool, 0);
    timer_init(&coctx->timer);
    coctx->mapco = hashmap_new_with_allocator(_malloc, _realloc, _free,
                                              sizeof(coro_sess), ONEK, 0, 0,
                                              _map_cosess_hash, _map_cosess_compare, NULL, NULL);
    heap_init(&coctx->timeout_heap, _timeout_cmp);
    return coctx;
}
// 释放协程任务运行时上下文（包括对象池、超时堆、哈希表）
static void _coro_ctx_free(void *arg) {
    coro_ctx *coctx = arg;
    mco_result rtn;
    mco_coro **coro;
    while (NULL != (coro = (mco_coro **)qu_ptr_pop(&coctx->qucopool))) {
        rtn = mco_destroy(*coro);
        if (MCO_SUCCESS != rtn) {
            LOG_WARN("%s", mco_result_description(rtn));
        }
    }
    qu_ptr_free(&coctx->qucopool);
    /* 先释放超时堆（堆节点独立分配，不依赖 mapco） */
    while (NULL != coctx->timeout_heap.root) {
        timeout_entry *te = _TE_FROM_HNODE(coctx->timeout_heap.root);
        heap_dequeue(&coctx->timeout_heap);
        FREE(te);
    }
    size_t iter = 0;
    coro_sess *corosess;
    while (hashmap_iter(coctx->mapco, &iter, (void **)&corosess)) {
        if (!corosess->disposable) {
            qu_coinfo_free(&corosess->qucoinfo); /* free inline queue's ptr array; no struct FREE */
        }
    }
    hashmap_free(coctx->mapco);
    FREE(coctx);
}
// 从协程对象池取出可用协程，池为空时新建并首次 resume 到第一个 yield 点
static mco_coro *_coro_pool_get(task_ctx *task) {
    coro_ctx *coctx = task->arg;
    mco_coro **coro = (mco_coro **)qu_ptr_pop(&coctx->qucopool);
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
// 从对象池取出协程并推入分发参数，开始执行新的消息处理流程
static void _co_create(task_dispatch_arg *arg) {
    coro_ctx *coctx = arg->task->arg;
    coctx->curco = _coro_pool_get(arg->task);
    // 推入 8 字节指针而非整个结构体，由 _mco_cb 在 resume 后自行复制
    mco_result rtn = mco_push(coctx->curco, &arg, sizeof(arg));
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
    rtn = mco_resume(coctx->curco);
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
}
// 唤醒已挂起的协程，推入消息指针后 resume，返回后清理消息资源
static void _co_resume(mco_coro *coro, task_dispatch_arg *arg) {
    coro_ctx *coctx = arg->task->arg;
    coctx->curco = coro;
    // 推入 8 字节消息指针，避免拷贝整个 message_ctx
    message_ctx *msgptr = &arg->msg;
    mco_result rtn = mco_push(coro, &msgptr, sizeof(msgptr));
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
    rtn = mco_resume(coro);
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
    _message_clean(arg->msg.mtype, arg->msg.pktype, arg->msg.data);
}
// 处理超时消息：sess==0 新建协程，否则唤醒对应挂起协程
static void _timeout_dispatch(task_dispatch_arg *arg) {
    if (0 == arg->msg.sess) {
        _co_create(arg);
        return;
    }
    coro_ctx *coctx = arg->task->arg;
    coro_sess *cosess = _map_cosess_get(coctx, arg->msg.sess, (msg_type)arg->msg.mtype);
    if (NULL == cosess) {
        LOG_WARN("task %d message type %d, can't find session %"PRIu64, arg->task->name, arg->msg.mtype, arg->msg.sess);
        return;
    }
    mco_coro *coro = _get_mco(arg->task, cosess);
    if (cosess->disposable) {
        _map_cosess_delete(coctx, arg->msg.sess);
    }
    _co_resume(coro, arg);
}
// 处理连接建立消息：若有等待该 skid 的协程则唤醒，否则新建协程
static void _connected_dispatch(task_dispatch_arg *arg) {
    coro_ctx *coctx = arg->task->arg;
    coro_sess *cosess = _map_cosess_get(coctx, arg->msg.skid, (msg_type)arg->msg.mtype);
    if (NULL == cosess) {
        _co_create(arg);
        return;
    }
    mco_coro *coro = _get_mco(arg->task, cosess);
    if (NULL == coro) {
        _co_create(arg);
    } else {
        _co_resume(coro, arg);
    }
}
// 处理 SSL 握手完成消息：唤醒等待该 skid 的协程，或新建协程
static void _ssl_exchanged_dispatch(task_dispatch_arg *arg) {
    coro_ctx *coctx = arg->task->arg;
    coro_sess *cosess = _map_cosess_get(coctx, arg->msg.skid, (msg_type)arg->msg.mtype);
    if (NULL == cosess) {
        _co_create(arg);
        return;
    }
    mco_coro *coro = _get_mco(arg->task, cosess);
    if (NULL == coro) {
        _co_create(arg);
    } else {
        _co_resume(coro, arg);
    }
}
// 处理应用层握手完成消息：唤醒等待该 skid 的协程，或新建协程
static void _handshaked_dispatch(task_dispatch_arg *arg) {
    coro_ctx *coctx = arg->task->arg;
    coro_sess *cosess = _map_cosess_get(coctx, arg->msg.skid, (msg_type)arg->msg.mtype);
    if (NULL == cosess) {
        _co_create(arg);
        return;
    }
    mco_coro *coro = _get_mco(arg->task, cosess);
    if (NULL == coro) {
        _co_create(arg);
    } else {
        _co_resume(coro, arg);
    }
}
// 处理数据接收消息：sess==0 或协议不允许 resume 则新建协程，否则唤醒等待的协程
static void _recved_dispatch(task_dispatch_arg *arg) {
    if (0 == arg->msg.sess
        || ERR_OK != prots_may_resume(arg->msg.pktype, arg->msg.data)) {
        _co_create(arg);
        return;
    }
    coro_ctx *coctx = arg->task->arg;
    coro_sess *cosess = _map_cosess_get(coctx, arg->msg.sess, (msg_type)arg->msg.mtype);
    if (NULL == cosess) {
        _co_create(arg);
        return;
    }
    mco_coro *coro = _get_mco(arg->task, cosess);
    if (NULL == coro) {
        _co_create(arg);
    } else {
        _co_resume(coro, arg);
    }
}
// 处理连接关闭消息：排干所有等待该 skid 的挂起协程，再新建协程处理关闭事件
static void _closed_dispatch(task_dispatch_arg *arg) {
    coro_ctx *coctx = arg->task->arg;
    coro_sess *cosess = _map_cosess_get(coctx, arg->msg.skid, (msg_type)arg->msg.mtype);
    if (NULL != cosess) {
        mco_coro *coro;
        if (cosess->disposable) {
            /* disposable 只有一个协程，_get_mco 不消耗节点，不能循环 */
            coro = _get_mco(arg->task, cosess);
            if (NULL != coro) {
                _co_resume(coro, arg);
                arg->msg.data = NULL; /* _co_resume 已清理，置 NULL 防后续重复释放 */
            }
        } else {
            /* non-disposable：排空队列，每次 resume 后置 NULL 防重复释放 */
            while (NULL != (coro = _get_mco(arg->task, cosess))) {
                _co_resume(coro, arg);
                arg->msg.data = NULL;
            }
            qu_coinfo_free(&cosess->qucoinfo);
        }
        _map_cosess_delete(coctx, arg->msg.skid);
    }
    _co_create(arg);
}
// 处理 UDP 数据接收消息：sess==0 新建协程，否则唤醒对应一次性挂起协程
static void _recvfrom_dispatch(task_dispatch_arg *arg) {
    if (0 == arg->msg.sess) {
        _co_create(arg);
        return;
    }
    coro_ctx *coctx = arg->task->arg;
    coro_sess *cosess = _map_cosess_get(coctx, arg->msg.sess, (msg_type)arg->msg.mtype);
    if (NULL == cosess) {
        _co_create(arg);
    } else {
        /* recvfrom sessions are always disposable */
        mco_coro *coro = _get_mco(arg->task, cosess);
        _map_cosess_delete(coctx, arg->msg.sess);
        _co_resume(coro, arg);
    }
}
// 处理任务间通信响应消息：唤醒对应一次性挂起协程，或新建协程
static void _response_dispatch(task_dispatch_arg *arg) {
    coro_ctx *coctx = arg->task->arg;
    coro_sess *cosess = _map_cosess_get(coctx, arg->msg.sess, (msg_type)arg->msg.mtype);
    if (NULL == cosess) {
        _co_create(arg);
    } else {
        /* response sessions are always disposable */
        mco_coro *coro = _get_mco(arg->task, cosess);
        _map_cosess_delete(coctx, arg->msg.sess);
        _co_resume(coro, arg);
    }
}
// 定期（每 3 秒）扫描超时堆，唤醒所有已到期的挂起协程并注入超时消息
static void _timeout_monitor(task_ctx *task, uint64_t sess) {
    (void)sess;
    coro_ctx *coctx = task->arg;
    /* 只有存在挂起协程且堆非空才需要检查 */
    if (coctx->nyield > 0 && NULL != coctx->timeout_heap.root) {
        uint64_t now = timer_cur_ms(&coctx->timer);
        task_dispatch_arg arg;
        arg.task = task;
        arg.msg.mtype = MSG_TYPE_TIMEOUT;
        /* 堆顶是最早到期的条目：若堆顶未到期，后续全部未到期，O(1) 退出 */
        while (NULL != coctx->timeout_heap.root) {
            timeout_entry *te = _TE_FROM_HNODE(coctx->timeout_heap.root);
            if (te->timeout > now) {
                break; /* 最早的都没到期，无需继续 */
            }
            heap_dequeue(&coctx->timeout_heap);
            coro_sess key;
            key.sess = te->sess;
            coro_sess *cosess = (coro_sess *)hashmap_get(coctx->mapco, &key);
            if (NULL == cosess) {
                /* 已被正常路径消费（_get_mco 已删堆节点），此处只需释放 te */
                FREE(te);
                continue;
            }
            mco_coro *coro;
            coro_info *coinfo;
            if (cosess->disposable) {
                coinfo = &cosess->coinfo;
                coinfo->te = NULL; /* 堆节点已由 heap_dequeue 移除 */
                coro = coinfo->co;
                LOG_INFO("task %d message type %d session %"PRIu64" timeout.",
                         task->name, coinfo->mtype, te->sess);
                hashmap_delete(coctx->mapco, &key);
            } else {
                coinfo = qu_coinfo_peek(&cosess->qucoinfo);
                if (NULL == coinfo) {
                    FREE(te);
                    continue;
                }
                coinfo->te = NULL;
                coro = coinfo->co;
                LOG_INFO("task %d message type %d session %"PRIu64" timeout.",
                         task->name, coinfo->mtype, te->sess);
                qu_coinfo_pop(&cosess->qucoinfo);
            }
            arg.msg.sess = te->sess;
            FREE(te);
            _co_resume(coro, &arg);
        }
    }
    task_timeout(task, 0, 3 * 1000, _timeout_monitor);
}
// 协程任务的消息分发总入口，根据消息类型路由到对应的处理函数
static void _startup_dispatch(task_dispatch_arg *arg) {
    task_timeout(arg->task, 0, 3 * 1000, _timeout_monitor);
    _co_create(arg);
}
static void _closing_dispatch(task_dispatch_arg *arg) {
    _co_create(arg);
    if (((coro_ctx *)arg->task->arg)->nyield > 0) {
        LOG_WARN("task %d yield %d.", arg->task->name, ((coro_ctx *)arg->task->arg)->nyield);
    }
}
typedef void (*_coro_msg_handler_t)(task_dispatch_arg *arg);
static const _coro_msg_handler_t _coro_msg_handlers[MSG_TYPE_ALL] = {
    [MSG_TYPE_STARTUP]      = _startup_dispatch,
    [MSG_TYPE_CLOSING]      = _closing_dispatch,
    [MSG_TYPE_TIMEOUT]      = _timeout_dispatch,
    [MSG_TYPE_ACCEPT]       = _co_create,
    [MSG_TYPE_CONNECT]      = _connected_dispatch,
    [MSG_TYPE_SSLEXCHANGED] = _ssl_exchanged_dispatch,
    [MSG_TYPE_HANDSHAKED]   = _handshaked_dispatch,
    [MSG_TYPE_RECV]         = _recved_dispatch,
    [MSG_TYPE_SEND]         = _co_create,
    [MSG_TYPE_CLOSE]        = _closed_dispatch,
    [MSG_TYPE_RECVFROM]     = _recvfrom_dispatch,
    [MSG_TYPE_REQUEST]      = _co_create,
    [MSG_TYPE_RESPONSE]     = _response_dispatch,
};
static void _message_dispatch(task_dispatch_arg *arg) {
    if (arg->msg.mtype > MSG_TYPE_NONE
        && arg->msg.mtype < MSG_TYPE_ALL
        && NULL != _coro_msg_handlers[arg->msg.mtype]) {
        _coro_msg_handlers[arg->msg.mtype](arg);
    }
}
task_ctx *coro_task_register(loader_ctx *loader, name_t name, _task_startup_cb _startup, _task_closing_cb _closing) {
    coro_ctx *coctx = _coro_ctx_init();
    task_ctx *task = task_new(loader, name, _message_dispatch, _coro_ctx_free, coctx);
    if (ERR_OK != task_register(task, _startup, _closing)) {
        task_free(task);
        return NULL;
    }
    return task;
}
void coro_sync(task_ctx *task, SOCKET fd, uint64_t skid) {
    ev_ud_sess(&task->loader->netev, fd, skid, skid);
}
// 挂起当前协程并等待下一条匹配消息
// 返回指向分发参数中 msg 的指针，在下次 _coro_wait 或 _co_resume 返回前有效
static inline message_ctx *_coro_wait(task_ctx *task, int32_t disposable, uint64_t sess, msg_type mtype, uint32_t ms) {
    coro_ctx *coctx = task->arg;
    _map_cosess_set(task, disposable, coctx->curco, sess, mtype, ms);
    ++coctx->nyield;
    mco_result rtn = mco_yield(coctx->curco);
    --coctx->nyield;
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
    // 弹出 _co_resume 推入的 8 字节消息指针，避免拷贝整个 message_ctx
    message_ctx *msg;
    rtn = mco_pop(coctx->curco, &msg, sizeof(msg));
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
    ASSERTAB(sess == msg->sess, "different session");
    return msg;
}
void coro_sleep(task_ctx *task, uint32_t ms) {
    uint64_t sess = createid();
    task_timeout(task, sess, ms, NULL);
    _coro_wait(task, 1, sess, MSG_TYPE_TIMEOUT, 0);
}
void *coro_request(task_ctx *dst, task_ctx *src, uint8_t rtype, void *data, size_t size, int32_t copy, int32_t *erro, size_t *lens) {
    uint64_t sess = createid();
    task_request(dst, src, rtype, sess, data, size, copy);
    message_ctx *msg = _coro_wait(src, 1, sess, MSG_TYPE_RESPONSE, task_get_request_timeout(src));
    if (MSG_TYPE_TIMEOUT == msg->mtype) {
        *erro = ERR_FAILED;
        LOG_WARN("dst %d src %d request type %d timeout, session %"PRIu64".", dst->name, src->name, rtype, sess);
        return NULL;
    }
    *erro = msg->erro;
    SET_PTR(lens, msg->size);
    return msg->data;
}
// 等待 SSL 交换完成消息，超时或连接关闭时关闭连接并返回 ERR_FAILED
static int32_t _wait_ssl_exchanged(task_ctx *task, SOCKET fd, uint64_t skid) {
    message_ctx *msg = _coro_wait(task, 0, skid, MSG_TYPE_SSLEXCHANGED, task_get_netread_timeout(task));
    if (MSG_TYPE_TIMEOUT == msg->mtype) {
        ev_close(&task->loader->netev, fd, skid);
        LOG_WARN("task %d, ssl exchange timeout, skid %"PRIu64".", task->name, skid);
        return ERR_FAILED;
    }
    if (MSG_TYPE_CLOSE == msg->mtype) {
        return ERR_FAILED;
    }
    return ERR_OK;
}
int32_t coro_ssl_exchange(task_ctx *task, SOCKET fd, uint64_t skid, int32_t client, struct evssl_ctx *evssl) {
    if (ERR_OK != ev_ssl(&task->loader->netev, fd, skid, client, evssl)) {
        return ERR_FAILED;
    }
    return _wait_ssl_exchanged(task, fd, skid);
}
void *coro_handshaked(task_ctx *task, SOCKET fd, uint64_t skid, int32_t *err, size_t *size) {
    message_ctx *msg = _coro_wait(task, 0, skid, MSG_TYPE_HANDSHAKED, task_get_netread_timeout(task));
    if (MSG_TYPE_TIMEOUT == msg->mtype) {
        *err = ERR_FAILED;
        ev_close(&task->loader->netev, fd, skid);
        LOG_WARN("task: %d, handshake timeout, skid %"PRIu64".", task->name, skid);
        return NULL;
    }
    if (MSG_TYPE_CLOSE == msg->mtype) {
        *err = ERR_FAILED;
        return NULL;
    }
    *err = msg->erro;
    SET_PTR(size, msg->size);
    return msg->data;
}
int32_t coro_connect(task_ctx *task, pack_type pktype, struct evssl_ctx *evssl, const char *ip, uint16_t port, int32_t netev, void *extra,
    SOCKET *fd, uint64_t *skid) {
    if (ERR_OK != task_connect(task, pktype, evssl, ip, port, netev, extra, fd, skid)) {
        LOG_WARN("task: %d, connect %s:%d error.", task->name, ip, port);
        return ERR_FAILED;
    }
    message_ctx *msg = _coro_wait(task, 0, *skid, MSG_TYPE_CONNECT, task_get_connect_timeout(task));
    if (MSG_TYPE_TIMEOUT == msg->mtype) {
        ev_close(&task->loader->netev, *fd, *skid);
        LOG_WARN("task: %d, connect %s:%d timeout.", task->name, ip, port);
        return ERR_FAILED;
    }
    if (ERR_OK != msg->erro) {
        LOG_WARN("task: %d, connect %s:%d error.", task->name, ip, port);
        return ERR_FAILED;
    }
    if (NULL != evssl) {
        if (ERR_OK != _wait_ssl_exchanged(task, *fd, *skid)) {
            return ERR_FAILED;
        }
    }
    coro_sync(task, *fd, *skid);
    return ERR_OK;
}
// 等待指定连接的下一条接收消息，超时或连接关闭时返回 NULL
// 返回的指针在下次 _coro_wait 调用前有效
static message_ctx *_wait_recved(task_ctx *task, SOCKET fd, uint64_t skid) {
    message_ctx *msg = _coro_wait(task, 0, skid, MSG_TYPE_RECV, task_get_netread_timeout(task));
    if (MSG_TYPE_TIMEOUT == msg->mtype) {
        ev_close(&task->loader->netev, fd, skid);
        LOG_WARN("task %d, recve timeout, skid %"PRIu64".", task->name, skid);
        return NULL;
    }
    if (MSG_TYPE_CLOSE == msg->mtype) {
        return NULL;
    }
    return msg;
}
void *coro_send(task_ctx *task, SOCKET fd, uint64_t skid, void *data, size_t len, size_t *size, int32_t copy) {
    if (ERR_OK != ev_send(&task->loader->netev, fd, skid, data, len, copy)) {
        return NULL;
    }
    message_ctx *msg = _wait_recved(task, fd, skid);
    if (NULL == msg) {
        return NULL;
    }
    SET_PTR(size, msg->size);
    return msg->data;
}
void *coro_slice(task_ctx *task, SOCKET fd, uint64_t skid, size_t *size, int32_t *end) {
    message_ctx *msg = _wait_recved(task, fd, skid);
    if (NULL == msg) {
        return NULL;
    }
    if (PROT_SLICE_END == msg->slice) {
        *end = 1;
    }
    SET_PTR(size, msg->size);
    return msg->data;
}
void *coro_sendto(task_ctx *task, SOCKET fd, uint64_t skid, const char *ip, const uint16_t port,
    void *data, size_t len, size_t *size, int32_t copy) {
    coro_sync(task, fd, skid);
    if (ERR_OK != ev_sendto(&task->loader->netev, fd, skid, ip, port, data, len, copy)) {
        LOG_WARN("task %d, sendto error, skid %"PRIu64".", task->name, skid);
        ev_ud_sess(&task->loader->netev, fd, skid, 0);
        return NULL;
    }
    message_ctx *msg = _coro_wait(task, 1, skid, MSG_TYPE_RECVFROM, task_get_netread_timeout(task));
    if (MSG_TYPE_TIMEOUT == msg->mtype) {
        ev_ud_sess(&task->loader->netev, fd, skid, 0);
        LOG_WARN("task %d, sendto timeout, skid %"PRIu64".", task->name, skid);
        return NULL;
    }
    SET_PTR(size, msg->size);
    return ((char *)msg->data) + sizeof(netaddr_ctx);
}
