#include "srey/coro.h"
#include "containers/hashmap.h"
#include "containers/heap.h"
#include "utils/timer.h"
#include "utils/binary.h"
#include "utils/pool.h"
#define MINICORO_IMPL
#include "srey/minicoro.h"

#define COROPOOL_CAP      128
#define COROPOOL_MIN_KEEP 4

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
    uint64_t       since;   // 挂起起始时刻（毫秒），用于 debug dump 计算挂起时长
    timeout_entry *te;      // 非 NULL 表示已注册到超时堆
}coro_info;
// session 到挂起协程的映射节点
typedef struct coro_sess {
    int32_t disposable; // 1 = 一次性（用完即删），0 = 持久（连接生命期内可复用）
    uint64_t sess;      // session ID（一次性时为请求ID，持久时为 skid）
    union {
        coro_info coinfo;   // disposable == 1：单个挂起协程
        queue_ctx qucoinfo; // disposable == 0：挂起协程队列（内联，无额外 malloc，元素 coro_info）
    };
}coro_sess;
// 协程任务的运行时上下文，挂在 task->arg
typedef struct coro_ctx {
    int32_t nyield;          // 当前挂起（yield）中的协程数量
    mco_coro *curco;         // 正在运行的协程指针
    struct hashmap *mapco;   // sess → coro_sess 哈希映射
    void *arg;               // 用户自定义数据
    free_cb _arg_free;       // 用户数据释放回调
    uint64_t shrink_ms;      // 上次协程池收缩的时间戳(ms)，按 SHRINK_TIME 门控
    pool_ctx copool;         // 空闲协程对象池（元素 mco_coro *，含负载趋势）
    timer_ctx timer;         // 用于获取当前毫秒时间戳
    heap_ctx timeout_heap;   // 按到期时间排序的最小堆,O(1) 检查最早超时
}coro_ctx;
// fork_wait 屏障：栈分配于 coro_fork_wait 内，子协程跑完 stub 递减 pending；
// 归零时唤醒 waiter（栈生命周期到 coro_fork_wait return 才结束，覆盖 yield 期间）
typedef struct fork_barrier {
    int32_t pending;  // 未完成的 fork 子协程数；stub 跑完递减 1，归零唤醒 waiter
    mco_coro *waiter; // 等待 barrier 归零的父协程；pending=0 时 mco_resume 唤醒
} fork_barrier;
// fork_wait 内部包装：每个 func 配一份 slot，stub 跑完后释放
typedef struct fork_wait_slot {
    void (*func)(task_ctx *task, void *arg);      // 业务回调函数
    void *arg;                                    // 透传给 func 的参数（生命周期由 coro_fork_wait 调用方管理）
    fork_barrier *barrier;                        // 所属 barrier 指针；stub 跑完递减 barrier->pending
} fork_wait_slot;
// serial waiter 链表节点：cs 挂起协程的 FIFO 元素，进队时 MALLOC、出队由前一个协程的 _coro_serial_release FREE
typedef struct serial_node {
    mco_coro *co;             // 等待中的协程
    struct serial_node *next; // 下一节点；NULL 表示队尾
} serial_node;
struct coro_serial_ctx {
    int32_t ref;           // 嵌套深度（同协程多次进入累加）
    task_ctx *task;        // 所属 task；resume 时同步 coctx->curco 需要
    mco_coro *current;     // 当前持锁协程；NULL 表示无锁
    serial_node *head;     // waiters FIFO 队头
    serial_node *tail;     // waiters FIFO 队尾
};

static mco_desc _coro_desc; // 全局协程描述符，由 coro_desc_init 初始化

// 最小堆比较函数：timeout 小的优先（堆顶是最早到期的）
static int _coro_timeout_cmp(const heap_node *lhs, const heap_node *rhs) {
    return _TE_FROM_HNODE(lhs)->timeout < _TE_FROM_HNODE(rhs)->timeout;
}
// 创建 timeout_entry 并插入超时堆，返回堆节点指针（用于后续删除）
static timeout_entry *_coro_te_insert(coro_ctx *coctx, uint64_t timeout, uint64_t sess) {
    timeout_entry *te;
    MALLOC(te, sizeof(timeout_entry));
    te->hnode.parent = te->hnode.left = te->hnode.right = NULL;
    te->timeout = timeout;
    te->sess    = sess;
    heap_insert(&coctx->timeout_heap, &te->hnode);
    return te;
}
// 计算 coro_sess 在哈希表中的哈希值（基于 sess 字段）
static uint64_t _coro_cosess_hash(const void *item, uint64_t seed0, uint64_t seed1) {
    (void)seed0;
    (void)seed1;
    return hash_u64(((coro_sess *)item)->sess);
}
// 比较两个 coro_sess 节点（按 sess 升序）
static int _coro_cosess_compare(const void *a, const void *b, void *ud) {
    (void)ud;
    uint64_t sa = ((const coro_sess *)a)->sess;
    uint64_t sb = ((const coro_sess *)b)->sess;
    return (sa < sb) ? -1 : (sa > sb) ? 1 : 0;
}
// 将挂起的协程注册到 mapco，disposable=1 为一次性，0 为持久（可排队多个）
static void _coro_cosess_set(task_ctx *task, int32_t disposable, mco_coro *coro, uint64_t sess, msg_type mtype, uint32_t ms) {
    coro_ctx *coctx = task->arg;
    coro_sess key;
    key.sess = sess;
    uint64_t now = timer_cur_ms(&coctx->timer);
    if (disposable) {
        coro_sess cosess;
        cosess.disposable = 1;
        cosess.sess = sess;
        cosess.coinfo.te = NULL;
        cosess.coinfo.since = now;
        cosess.coinfo.timeout = ms > 0 ? now + ms : 0;
        cosess.coinfo.co = coro;
        cosess.coinfo.mtype = mtype;
        ASSERTAB(NULL == hashmap_set(coctx->mapco, &cosess), "repeat session");
        if (ms > 0) {
            /* hashmap_set 已拷贝 cosess；取真实存储位置设 te */
            coro_sess *stored = (coro_sess *)hashmap_get(coctx->mapco, &key);
            stored->coinfo.te = _coro_te_insert(coctx, cosess.coinfo.timeout, sess);
        }
        return;
    }
    /* non-disposable: inline queue_ctx, no heap allocation for the queue header */
    coro_info coinfo;
    coinfo.te = NULL;
    coinfo.since = now;
    coinfo.timeout = ms > 0 ? now + ms : 0;
    coinfo.co = coro;
    coinfo.mtype = mtype;
    coro_sess *cofind = (coro_sess *)hashmap_get(coctx->mapco, &key);
    if (NULL != cofind) {
        /* entry already exists: push directly into the inline queue */
        queue_push(&cofind->qucoinfo, &coinfo);
        if (ms > 0) {
            coro_info *last = queue_at(&cofind->qucoinfo,
                                       queue_size(&cofind->qucoinfo) - 1);
            last->te = _coro_te_insert(coctx, coinfo.timeout, sess);
        }
    } else {
        /* new entry: init inline queue on the stack, hashmap_set copies the whole struct */
        coro_sess cosess;
        cosess.disposable = 0;
        cosess.sess = sess;
        queue_init(&cosess.qucoinfo, sizeof(coro_info), 4);   /* allocates cosess.qucoinfo.ptr on heap */
        queue_push(&cosess.qucoinfo, &coinfo);
        hashmap_set(coctx->mapco, &cosess);
        /* hashmap_set copies cosess (struct copy); qucoinfo.ptr is now owned by the stored copy.
         * cosess goes out of scope but the heap array lives on through the stored copy. */
        if (ms > 0) {
            coro_sess *stored = (coro_sess *)hashmap_get(coctx->mapco, &key);
            coro_info *last = queue_at(&stored->qucoinfo,
                                       queue_size(&stored->qucoinfo) - 1);
            last->te = _coro_te_insert(coctx, coinfo.timeout, sess);
        }
    }
}
// 从 mapco 查找匹配 sess 和 mtype 的挂起协程节点
// 返回哈希表内部存储的直接指针，调用方在使用完毕前不得调用 _coro_cosess_delete
static coro_sess *_coro_cosess_get(coro_ctx *coctx, uint64_t sess, msg_type mtype) {
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
        coro_info *coinfo = queue_peek(&cofind->qucoinfo);
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
static void _coro_cosess_delete(coro_ctx *coctx, uint64_t sess) {
    coro_sess key;
    key.sess = sess;
    hashmap_delete(coctx->mapco, &key);
}
// 从 coro_sess 中取出协程对象，并在必要时从超时堆中移除对应条目
static inline mco_coro *_coro_get_mco(task_ctx *task, coro_sess *cosess) {
    coro_ctx *coctx = (coro_ctx *)task->arg;
    coro_info *coinfo;
    mco_coro  *co;
    if (cosess->disposable) {
        coinfo = &cosess->coinfo;
        co = coinfo->co;
    } else {
        coinfo = queue_pop(&cosess->qucoinfo);  /* inline queue: pass address */
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
static void _coro_mco_cb(mco_coro *coro) {
    mco_result rtn;
    task_dispatch_arg *argp;
    for (;;) {
        rtn = mco_yield(coro);
        ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
        // 弹出 8 字节指针并在协程栈上复制一份，保证 arg.fd/arg.skid 在整个生命期内有效
        rtn = mco_pop(coro, &argp, sizeof(argp));
        ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
        task_dispatch_arg arg = *argp; // 在协程栈上保存一份副本
        task_incref(arg.task); // 保证 _message_run 在 yield 后 task 不会被释放
        _message_run(arg.task, &arg.msg);
        coro_ctx *ctx = (coro_ctx *)arg.task->arg;
        if (ERR_OK != pool_push(&ctx->copool, coro, POOL_OP_NOFREE)) {
            task_ungrab(arg.task);
            break; // 池满时跳出循环，让函数自然返回使协程进入 MCO_DEAD 状态
        }
        task_ungrab(arg.task);
    }
}
void coro_desc_init(size_t stack_size) {
    _coro_desc = mco_desc_init(_coro_mco_cb, stack_size);
}
// 对象池 _elnew：新建协程并首次 resume 到第一个 yield 点
static void *_coro_new(void *args) {
    (void)args;
    mco_coro *co;
    mco_result rtn = mco_create(&co, &_coro_desc);
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
    rtn = mco_resume(co);
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
    return co;
}
// 对象池 _elfree：销毁协程对象
static void _coro_free(void *co) {
    mco_result rtn = mco_destroy((mco_coro *)co);
    if (MCO_SUCCESS != rtn) {
        LOG_WARN("%s", mco_result_description(rtn));
    }
}
// 初始化协程任务运行时上下文
static coro_ctx *_coro_ctx_init(free_cb _argfree, void *arg) {
    el_cbs _coro_pool_cbs = { _coro_new, _coro_free, NULL, NULL };
    coro_ctx *coctx;
    CALLOC(coctx, 1, sizeof(coro_ctx));
    coctx->arg = arg;
    coctx->_arg_free = _argfree;
    pool_init(&coctx->copool, 0, COROPOOL_CAP, COROPOOL_MIN_KEEP, 0, &_coro_pool_cbs);
    timer_init(&coctx->timer);
    coctx->shrink_ms = timer_cur_ms(&coctx->timer);
    coctx->mapco = hashmap_new_with_allocator(_malloc, _realloc, _free,
                                              sizeof(coro_sess), ONEK, 0, 0,
                                              _coro_cosess_hash, _coro_cosess_compare, NULL, NULL);
    heap_init(&coctx->timeout_heap, _coro_timeout_cmp);
    return coctx;
}
// 释放协程任务运行时上下文（包括对象池、超时堆、哈希表）
static void _coro_ctx_free(void *arg) {
    coro_ctx *coctx = (coro_ctx *)arg;
    pool_free(&coctx->copool);
    /* 先释放超时堆（堆节点独立分配，不依赖 mapco） */
    while (NULL != coctx->timeout_heap.root) {
        timeout_entry *te = _TE_FROM_HNODE(coctx->timeout_heap.root);
        heap_dequeue(&coctx->timeout_heap);
        FREE(te);
    }
    size_t iter = 0;
    coro_sess *corosess;
    coro_info *ci;
    uint32_t i, n;
    // 注意：上面已释放整个 timeout_heap，此处 coinfo->te 均为悬空指针，禁止解引用；
    // 仅销毁 coinfo->co 协程对象即可，te 内存随堆释放无需单独 FREE
    while (hashmap_iter(coctx->mapco, &iter, (void **)&corosess)) {
        if (corosess->disposable) {
            /* 一次性 session：直接销毁挂起的协程对象 */
            if (NULL != corosess->coinfo.co) {
                mco_destroy(corosess->coinfo.co);
            }
        } else {
            /* 持久 session：逐项销毁队列中每个挂起协程，再释放 ptr 数组 */
            n = queue_size(&corosess->qucoinfo);
            for (i = 0; i < n; i++) {
                ci = queue_at(&corosess->qucoinfo, i);
                if (NULL != ci && NULL != ci->co) {
                    mco_destroy(ci->co);
                }
            }
            queue_free(&corosess->qucoinfo); /* free inline queue's ptr array; no struct FREE */
        }
    }
    hashmap_free(coctx->mapco);
    //释放用户数据
    if (NULL != coctx->_arg_free
        && NULL != coctx->arg) {
        coctx->_arg_free(coctx->arg);
    }
    FREE(coctx);
}
// 从协程对象池取出可用协程，池为空时新建并首次 resume 到第一个 yield 点
static mco_coro *_coro_pool_get(task_ctx *task) {
    coro_ctx *coctx = task->arg;
    return (mco_coro *)pool_pop(&coctx->copool, NULL, 0);
}
// 从对象池取出协程并推入分发参数，开始执行新的消息处理流程
static void _coro_mco_create(task_dispatch_arg *arg) {
    coro_ctx *coctx = arg->task->arg;
    mco_coro *co = _coro_pool_get(arg->task);
    coctx->curco = co;
    // 推入 8 字节指针而非整个结构体，由 _coro_mco_cb 在 resume 后自行复制
    mco_result rtn = mco_push(co, &arg, sizeof(arg));
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
    rtn = mco_resume(co);
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
    if (MCO_DEAD == mco_status(co)) {
        mco_destroy(co); // 池满导致 _coro_mco_cb 返回，协程已死亡，须在此释放
    }
}
// 唤醒已挂起的协程，推入消息指针后 resume，返回后清理消息资源
static void _coro_mco_resume(mco_coro *coro, task_dispatch_arg *arg) {
    coro_ctx *coctx = arg->task->arg;
    coctx->curco = coro;
    // 推入 8 字节消息指针，避免拷贝整个 message_ctx
    message_ctx *msgptr = &arg->msg;
    mco_result rtn = mco_push(coro, &msgptr, sizeof(msgptr));
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
    rtn = mco_resume(coro);
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
    _message_clean(&arg->msg);
    if (MCO_DEAD == mco_status(coro)) {
        mco_destroy(coro); // 池满导致 _coro_mco_cb 返回，协程已死亡，须在此释放
    }
}
// 处理超时消息：sess==0 新建协程，否则唤醒对应挂起协程
static void _coro_handle_timeout(task_dispatch_arg *arg) {
    if (0 == arg->msg.sess) {
        _coro_mco_create(arg);
        return;
    }
    coro_ctx *coctx = arg->task->arg;
    coro_sess *cosess = _coro_cosess_get(coctx, arg->msg.sess, arg->msg.mtype);
    if (NULL == cosess) {
        LOG_WARN("task %s message type %d, can't find session %"PRIu64, _NAME_OR(arg->task->name), arg->msg.mtype, arg->msg.sess);
        return;
    }
    mco_coro *coro = _coro_get_mco(arg->task, cosess);
    if (cosess->disposable) {
        _coro_cosess_delete(coctx, arg->msg.sess);
    }
    _coro_mco_resume(coro, arg);
}
// non-disposable 唤醒(skid 作 key):cosess 或 _coro_get_mco 队列空时新建协程,不删 sess。
static void _coro_handle_non_disposable(task_dispatch_arg *arg) {
    coro_ctx *coctx = arg->task->arg;
    coro_sess *cosess = _coro_cosess_get(coctx, arg->msg.skid, arg->msg.mtype);
    if (NULL == cosess) {
        _coro_mco_create(arg);
        return;
    }
    mco_coro *coro = _coro_get_mco(arg->task, cosess);
    if (NULL == coro) {
        _coro_mco_create(arg);
    } else {
        _coro_mco_resume(coro, arg);
    }
}
// disposable 唤醒(sess 作 key):由 _coro_wait disposable=1 注册,_coro_get_mco 不会 NULL;唤醒后删 sess。
static void _coro_handle_disposable(task_dispatch_arg *arg) {
    coro_ctx *coctx = arg->task->arg;
    coro_sess *cosess = _coro_cosess_get(coctx, arg->msg.sess, arg->msg.mtype);
    if (NULL == cosess) {
        _coro_mco_create(arg);
        return;
    }
    mco_coro *coro = _coro_get_mco(arg->task, cosess);
    _coro_cosess_delete(coctx, arg->msg.sess);
    _coro_mco_resume(coro, arg);
}
// 处理数据接收消息：sess==0 或协议不允许 resume 则新建协程，否则唤醒等待的协程
static void _coro_handle_recved(task_dispatch_arg *arg) {
    if (0 == arg->msg.sess
        || ERR_OK != prots_may_resume(arg->msg.subtype, arg->msg.data)) {
        _coro_mco_create(arg);
        return;
    }
    coro_ctx *coctx = arg->task->arg;
    coro_sess *cosess = _coro_cosess_get(coctx, arg->msg.sess, arg->msg.mtype);
    if (NULL == cosess) {
        _coro_mco_create(arg);
        return;
    }
    mco_coro *coro = _coro_get_mco(arg->task, cosess);
    if (NULL == coro) {
        _coro_mco_create(arg);
    } else {
        _coro_mco_resume(coro, arg);
    }
}
// 处理连接关闭消息：排干所有等待该 skid 的挂起协程，再新建协程处理关闭事件
static void _coro_handle_closed(task_dispatch_arg *arg) {
    coro_ctx *coctx = arg->task->arg;
    coro_sess *cosess = _coro_cosess_get(coctx, arg->msg.skid, arg->msg.mtype);
    if (NULL != cosess) {
        mco_coro *coro;
        if (cosess->disposable) {
            /* disposable 只有一个协程，_get_mco 不消耗节点，不能循环 */
            coro = _coro_get_mco(arg->task, cosess);
            _coro_cosess_delete(coctx, arg->msg.skid); /* 先删再 resume，cosess 此后不再访问 */
            _coro_mco_resume(coro, arg);
            arg->msg.data = NULL; /* _coro_mco_resume 已清理，置 NULL 防后续重复释放 */
        } else {
            /* 预转移 qucoinfo 所有权到栈上，再提前删除 hashmap 条目。
             * _coro_mco_resume 期间用户代码可能调用 coro_connect 等触发 hashmap_set，
             * 进而导致 resize 释放旧 bucket，使 cosess 悬空。
             * qucoinfo.ptr 是独立堆分配，浅拷贝后不受 resize 影响。*/
            queue_ctx local_q = cosess->qucoinfo;
            queue_clear(&cosess->qucoinfo);   /* 防止 _coro_ctx_free 中 iter 重复 free ptr */
            _coro_cosess_delete(coctx, arg->msg.skid); /* cosess 此后可能悬空，不再访问 */
            coro_info *coinfo;
            while (NULL != (coinfo = queue_pop(&local_q))) {
                coro = coinfo->co;
                if (NULL != coinfo->te) {
                    heap_remove(&coctx->timeout_heap, &coinfo->te->hnode);
                    FREE(coinfo->te);
                }
                _coro_mco_resume(coro, arg);
                arg->msg.data = NULL;
            }
            queue_free(&local_q);
        }
    } else {
        /* zombie 清理：non-disposable 路径 coinfo 被 _coro_get_mco 全部消费或
         * _coro_timeout_monitor del_at 后队列变空，entry 暂留 mapco；CLOSE 路径
         * 在此补清。mapco elfree=NULL，hashmap_delete 不触发释放回调，
         * 必须显式 queue_free 否则 ptr 数组泄漏。*/
        coro_sess key;
        key.sess = arg->msg.skid;
        coro_sess *cofind = (coro_sess *)hashmap_get(coctx->mapco, &key);
        if (NULL != cofind && !cofind->disposable) {
            queue_free(&cofind->qucoinfo);
            hashmap_delete(coctx->mapco, &key);
        }
    }
    _coro_mco_create(arg);
}
// 处理 UDP 数据接收消息：sess==0 新建协程，否则唤醒对应一次性挂起协程
static void _coro_handle_recvfrom(task_dispatch_arg *arg) {
    if (0 == arg->msg.sess) {
        _coro_mco_create(arg);
        return;
    }
    _coro_handle_disposable(arg);
}
// 定期（每 1 秒）扫描超时堆，唤醒所有已到期的挂起协程并注入超时消息
static void _coro_timeout_monitor(task_ctx *task, uint64_t sess) {
    (void)sess;
    coro_ctx *coctx = task->arg;
    uint64_t now = timer_cur_ms(&coctx->timer);
    /* 只有存在挂起协程且堆非空才需要检查 */
    if (coctx->nyield > 0 && NULL != coctx->timeout_heap.root) {
        task_dispatch_arg arg = { 0 };
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
                LOG_INFO("task %s message type %d session %"PRIu64" timeout.",
                         _NAME_OR(task->name), coinfo->mtype, te->sess);
                hashmap_delete(coctx->mapco, &key);
            } else {
                /* 队列按 push 序排列，但 te 在堆中按 timeout 排序：
                 * 若两次 push 的 timeout 不同，先到期的 te 对应的 coinfo
                 * 不一定是队首，需按 coinfo->te 精确定位。 */
                uint32_t qsize = queue_size(&cosess->qucoinfo);
                uint32_t idx;
                coro_info *probe;
                coinfo = NULL;
                for (idx = 0; idx < qsize; idx++) {
                    probe = queue_at(&cosess->qucoinfo, idx);
                    if (NULL != probe && probe->te == te) {
                        coinfo = probe;
                        break;
                    }
                }
                if (NULL == coinfo) {
                    FREE(te);
                    continue;
                }
                coinfo->te = NULL;
                coro = coinfo->co;
                LOG_INFO("task %s message type %d session %"PRIu64" timeout.",
                         _NAME_OR(task->name), coinfo->mtype, te->sess);
                queue_del_at(&cosess->qucoinfo, idx);
                /* del_at 后若队列为空，coro_sess 条目暂留 mapco：
                 * _coro_cosess_get 对空队列返回 NULL，dispatch 函数随即调用 _co_create，
                 * 消息不会丢失；条目在连接关闭时由 _coro_handle_closed 统一删除。*/
            }
            arg.msg.sess = te->sess;
            FREE(te);
            _coro_mco_resume(coro, &arg);
        }
    }
    if (now - coctx->shrink_ms >= SHRINK_TIME) {
        coctx->shrink_ms = now;
        pool_shrink(&coctx->copool, SHRINK_NKEEP(pool_size(&coctx->copool)), SHRINK_BUSY);
    }
    task_timeout(task, 0, 1 * 1000, _coro_timeout_monitor);
}
// 协程任务的消息分发总入口，根据消息类型路由到对应的处理函数
static void _coro_handle_startup(task_dispatch_arg *arg) {
    task_timeout(arg->task, 0, 1 * 1000, _coro_timeout_monitor);
    _coro_mco_create(arg);
}
static void _coro_handle_closing(task_dispatch_arg *arg) {
    _coro_mco_create(arg);
    if (((coro_ctx *)arg->task->arg)->nyield > 0) {
        LOG_WARN("task %s yield %d.", _NAME_OR(arg->task->name), ((coro_ctx *)arg->task->arg)->nyield);
    }
}
typedef void (*_coro_msg_handler_t)(task_dispatch_arg *arg);
static const _coro_msg_handler_t _coro_msg_handlers[MSG_TYPE_ALL] = {
    [MSG_TYPE_STARTUP]      = _coro_handle_startup,
    [MSG_TYPE_CLOSING]      = _coro_handle_closing,
    [MSG_TYPE_TIMEOUT]      = _coro_handle_timeout,
    [MSG_TYPE_ACCEPT]       = _coro_mco_create,
    [MSG_TYPE_CONNECT]      = _coro_handle_non_disposable, // 连接建立
    [MSG_TYPE_SSLEXCHANGED] = _coro_handle_non_disposable, // SSL 握手
    [MSG_TYPE_HANDSHAKED]   = _coro_handle_non_disposable, // 应用层握手
    [MSG_TYPE_RECV]         = _coro_handle_recved,
    [MSG_TYPE_SEND]         = _coro_mco_create,
    [MSG_TYPE_CLOSE]        = _coro_handle_closed,
    [MSG_TYPE_RECVFROM]     = _coro_handle_recvfrom,
    [MSG_TYPE_REQUEST]      = _coro_mco_create,
    [MSG_TYPE_RESPONSE]     = _coro_handle_disposable,    // 跨 task 响应
    // fork 与 REQUEST 同模式：每条 fork 消息总是新建协程，无 sess 唤醒路径
    [MSG_TYPE_FORK]         = _coro_mco_create,
};
static void _coro_message_dispatch(task_dispatch_arg *arg) {
    if (arg->msg.mtype > MSG_TYPE_NONE
        && arg->msg.mtype < MSG_TYPE_ALL
        && NULL != _coro_msg_handlers[arg->msg.mtype]) {
        _coro_msg_handlers[arg->msg.mtype](arg);
    }
}
task_ctx *coro_task_register(loader_ctx *loader, const char *name, size_t quecap,
                             _task_startup_cb _startup, _task_closing_cb _closing,
                             free_cb _argfree, void *arg) {
    coro_ctx *coctx = _coro_ctx_init(_argfree, arg);
    task_ctx *task = task_new(loader, name, quecap, _coro_message_dispatch, _coro_ctx_free, coctx);
    task->type = TASK_MCO;
    if (ERR_OK != task_register(task, _startup, _closing)) {
        task_free(task);
        return NULL;
    }
    return task;
}
void *coro_get_arg(task_ctx *task) {
    if (NULL == task->arg) {
        return NULL;
    }
    return ((coro_ctx *)task->arg)->arg;
}
int32_t coro_sync(task_ctx *task, SOCKET fd, uint64_t skid) {
    return ev_ud_sess(&task->loader->netev, fd, skid, skid);
}
// 挂起当前协程并等待下一条匹配消息
// 返回指向分发参数中 msg 的指针，在下次 _coro_wait 或 _coro_mco_resume 返回前有效
static inline message_ctx *_coro_wait(task_ctx *task, int32_t disposable, uint64_t sess, msg_type mtype, uint32_t ms) {
    coro_ctx *coctx = task->arg;
    _coro_cosess_set(task, disposable, coctx->curco, sess, mtype, ms);
    ++coctx->nyield;
    mco_result rtn = mco_yield(coctx->curco);
    --coctx->nyield;
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
    // 弹出 _coro_mco_resume 推入的 8 字节消息指针，避免拷贝整个 message_ctx
    message_ctx *msg;
    rtn = mco_pop(coctx->curco, &msg, sizeof(msg));
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
    /* 所有消息类型均保证 msg.sess 与注册 key 一致
     * （CONNECT/SSL/CLOSE 系 skid，TIMEOUT 系 te->sess，RESPONSE/RECV 系传入 sess），
     * dispatch 函数以相同 key 查找协程后调用 _co_resume；若此断言触发，说明 dispatch 逻辑有 bug。*/
    ASSERTAB(sess == msg->sess, "different session");
    return msg;
}
void coro_sleep(task_ctx *task, uint32_t ms) {
    if (0 == ms) {
        return;
    }
    uint64_t sess = createid();
    task_timeout(task, sess, ms, NULL);
    _coro_wait(task, 1, sess, MSG_TYPE_TIMEOUT, 0);
}
void *coro_request(task_ctx *dst, task_ctx *src,
                   subtype_t rtype, void *data, size_t size, int32_t copy,
                   int32_t *erro, size_t *lens) {
    uint64_t sess = createid();
    task_request(dst, src, rtype, sess, data, size, copy);
    message_ctx *msg = _coro_wait(src, 1, sess, MSG_TYPE_RESPONSE, task_get_request_timeout(src));
    if (MSG_TYPE_TIMEOUT == msg->mtype) {
        *erro = ERR_FAILED;
        LOG_WARN("dst %s src %s request type %d timeout, session %"PRIu64".", _NAME_OR(dst->name), _NAME_OR(src->name), rtype, sess);
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
        ev_close(&task->loader->netev, fd, skid, 1);
        LOG_WARN("task %s, ssl exchange timeout, skid %"PRIu64".", _NAME_OR(task->name), skid);
        return ERR_FAILED;
    }
    if (MSG_TYPE_CLOSE == msg->mtype) {
        return ERR_FAILED;
    }
    return ERR_OK;
}
int32_t coro_ssl_exchange(task_ctx *task, SOCKET fd, uint64_t skid,
                          int32_t client, struct evssl_ctx *evssl) {
    if (ERR_OK != ev_ssl(&task->loader->netev, fd, skid, client, evssl)) {
        return ERR_FAILED;
    }
    return _wait_ssl_exchanged(task, fd, skid);
}
void *coro_handshaked(task_ctx *task, SOCKET fd, uint64_t skid, int32_t *err, size_t *size) {
    if (INVALID_SOCK == fd) {
        *err = ERR_FAILED;
        return NULL;
    }
    message_ctx *msg = _coro_wait(task, 0, skid, MSG_TYPE_HANDSHAKED, task_get_netread_timeout(task));
    if (MSG_TYPE_TIMEOUT == msg->mtype) {
        *err = ERR_FAILED;
        ev_close(&task->loader->netev, fd, skid, 1);
        LOG_WARN("task: %s, handshake timeout, skid %"PRIu64".", _NAME_OR(task->name), skid);
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
int32_t coro_connect(task_ctx *task, pack_type pktype,
                     struct evssl_ctx *evssl, const char *ip, uint16_t port,
                     int32_t netev, void *extra,
                     SOCKET *fd, uint64_t *skid) {
    if (ERR_OK != task_connect(task, pktype, evssl, ip, port, netev, extra, fd, skid)) {
        LOG_WARN("task: %s, connect %s:%d error.", _NAME_OR(task->name), ip, port);
        return ERR_FAILED;
    }
    message_ctx *msg = _coro_wait(task, 0, *skid, MSG_TYPE_CONNECT, task_get_connect_timeout(task));
    if (MSG_TYPE_TIMEOUT == msg->mtype) {
        ev_close(&task->loader->netev, *fd, *skid, 1);
        LOG_WARN("task: %s, connect %s:%d timeout.", _NAME_OR(task->name), ip, port);
        return ERR_FAILED;
    }
    if (ERR_OK != msg->erro) {
        LOG_WARN("task: %s, connect %s:%d error.", _NAME_OR(task->name), ip, port);
        return ERR_FAILED;
    }
    if (NULL != evssl) {
        if (ERR_OK != _wait_ssl_exchanged(task, *fd, *skid)) {
            return ERR_FAILED;
        }
    }
    return coro_sync(task, *fd, *skid);
}
void coro_close(task_ctx *task, SOCKET fd, uint64_t skid, int32_t immed) {
    if (INVALID_SOCK == fd) {
        return;
    }
    ev_close(&task->loader->netev, fd, skid, immed);
    _coro_wait(task, 0, skid, MSG_TYPE_CLOSE, task_get_netread_timeout(task));
}
// 等待指定连接的下一条接收消息，超时或连接关闭时返回 NULL
// 返回的指针在下次 _coro_wait 调用前有效
static message_ctx *_coro_wait_recved(task_ctx *task, SOCKET fd, uint64_t skid) {
    message_ctx *msg = _coro_wait(task, 0, skid, MSG_TYPE_RECV, task_get_netread_timeout(task));
    if (MSG_TYPE_TIMEOUT == msg->mtype) {
        ev_close(&task->loader->netev, fd, skid, 1);
        LOG_WARN("task %s, recve timeout, skid %"PRIu64".", _NAME_OR(task->name), skid);
        return NULL;
    }
    if (MSG_TYPE_CLOSE == msg->mtype) {
        return NULL;
    }
    return msg;
}
void *coro_send(task_ctx *task, SOCKET fd, uint64_t skid,
                void *data, size_t len, size_t *size, int32_t copy) {
    if (ERR_OK != ev_send(&task->loader->netev, fd, skid, data, len, copy)) {
        return NULL;
    }
    message_ctx *msg = _coro_wait_recved(task, fd, skid);
    if (NULL == msg) {
        return NULL;
    }
    SET_PTR(size, msg->size);
    return msg->data;
}
void *coro_slice(task_ctx *task, SOCKET fd, uint64_t skid, size_t *size, int32_t *end) {
    if (INVALID_SOCK == fd) {
        return NULL;
    }
    message_ctx *msg = _coro_wait_recved(task, fd, skid);
    if (NULL == msg) {
        return NULL;
    }
    *end = (PROT_SLICE_END == msg->slice) ? 1 : 0;
    SET_PTR(size, msg->size);
    return msg->data;
}
void *coro_sendto(task_ctx *task, SOCKET fd, uint64_t skid,
                  const char *ip, const uint16_t port,
                  void *data, size_t len, size_t *size, int32_t copy) {
    if (ERR_OK != coro_sync(task, fd, skid)) {
        if (!copy) {
            FREE(data);
        }
        return NULL;
    }
    if (ERR_OK != ev_sendto(&task->loader->netev, fd, skid, ip, port, data, len, copy)) {
        LOG_WARN("task %s, sendto error, skid %"PRIu64".", _NAME_OR(task->name), skid);
        (void)ev_ud_sess(&task->loader->netev, fd, skid, 0);
        return NULL;
    }
    message_ctx *msg = _coro_wait(task, 1, skid, MSG_TYPE_RECVFROM, task_get_netread_timeout(task));
    if (MSG_TYPE_TIMEOUT == msg->mtype) {
        (void)ev_ud_sess(&task->loader->netev, fd, skid, 0);
        LOG_WARN("task %s, sendto timeout, skid %"PRIu64".", _NAME_OR(task->name), skid);
        return NULL;
    }
    SET_PTR(size, msg->size);
    return ((char *)msg->data) + sizeof(netaddr_ctx);
}
// fork_wait 内部 stub：跑用户 func 后递减 barrier，归零时同步 curco 并 mco_resume 唤醒 waiter
static void _coro_fork_wait_stub(task_ctx *task, void *arg) {
    fork_wait_slot *slot = (fork_wait_slot *)arg;
    slot->func(task, slot->arg);
    fork_barrier *b = slot->barrier;
    FREE(slot);
    if (0 == --b->pending) {
        // 与 _coro_mco_resume 同模式：mco_resume 前必须把 coctx->curco 同步为 waiter，
        // 否则 waiter 醒来后 _coro_wait/_coro_get_mco 会用错协程标识进 cosess
        coro_ctx *coctx = (coro_ctx *)task->arg;
        mco_coro *waiter = b->waiter;// b 在 W 协程栈内，mco_destroy(W) 后释放整块，须先缓存 waiter 防 SIGBUS
        coctx->curco = waiter;
        mco_result rtn = mco_resume(waiter);
        ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
        if (MCO_DEAD == mco_status(waiter)) {
            mco_destroy(waiter);// 池满导致 _coro_mco_cb 返回，协程已死亡，须在此释放
        }
    }
}
void coro_fork(task_ctx *task,
               void (*func)(task_ctx *task, void *arg),
               void *arg) {
    // 把 (func, arg) 包装成 MSG_TYPE_FORK 自发消息推入 task->qumsg，
    // 由 loader 调度后 _coro_msg_handlers[FORK]=_coro_mco_create 起新协程跑 _handle_fork → func。
    // 多走一遍 fsqu 队列（几百 ns）换来：复用现有调度链 + 自动进 dispatch_cpu_ns[FORK] 桶统计 + 监控覆盖
    fork_item *item;
    MALLOC(item, sizeof(*item));
    item->func = func;
    item->arg = arg;
    message_ctx msg = { 0 };
    msg.mtype = MSG_TYPE_FORK;
    msg.data = item;
    _task_message_push(task, &msg);
}
int32_t coro_fork_wait(task_ctx *task,
                       int32_t n,
                       void (*funcs[])(task_ctx *task, void *arg),
                       void *args[]) {
    if (n <= 0) {
        return ERR_OK;
    }
    coro_ctx *coctx = (coro_ctx *)task->arg;
    if (NULL == coctx->curco) {
        // 不在协程上下文调用：mco_yield 会失败，提前拒绝
        LOG_WARN("task %s, coro_fork_wait called outside coroutine context.", _NAME_OR(task->name));
        return ERR_FAILED;
    }
    // barrier 栈分配：fork_wait_slot 在 stub 内 FREE 时还能通过 slot->barrier 访问；
    // 本函数在 mco_yield 期间栈帧仍在内存（minicoro 协程独立栈），地址有效
    fork_barrier barrier;
    barrier.pending = n;
    barrier.waiter = coctx->curco;
    fork_wait_slot *slot;
    for (int32_t i = 0; i < n; i++) {
        MALLOC(slot, sizeof(*slot));
        slot->func = funcs[i];
        slot->arg = args[i];
        slot->barrier = &barrier;
        coro_fork(task, _coro_fork_wait_stub, slot);
    }
    // 让出当前协程，等最后完成的子协程 mco_resume 唤醒
    mco_result rtn = mco_yield(coctx->curco);
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
    return ERR_OK;
}
coro_serial_ctx *coro_serial_new(task_ctx *task) {
    coro_serial_ctx *s;
    CALLOC(s, 1, sizeof(coro_serial_ctx));
    s->task = task;
    return s;
}
void coro_serial_free(coro_serial_ctx *serial) {
    // 销毁前调用方应保证无 in-flight：current/ref/waiters 全清空
    ASSERTAB(NULL == serial->current && 0 == serial->ref && NULL == serial->head,
             "coro_serial_free with active holders or waiters");
    FREE(serial);
}
// 临界区出口：ref 归 0 时取队头 waiter，先同步 current/ref 再 mco_resume；与 fork_wait_stub 同模式
static void _coro_serial_release(coro_serial_ctx *serial) {
    serial->ref--;
    if (0 != serial->ref) {
        return;
    }
    serial_node *nxt = serial->head;
    if (NULL == nxt) {
        serial->current = NULL;
        return;
    }
    serial->head = nxt->next;
    if (NULL == serial->head) {
        serial->tail = NULL;
    }
    // 唤醒前先设置 current/ref，nxt 唤醒后读取看到一致状态
    mco_coro *wco = nxt->co;
    serial->current = wco;
    serial->ref = 1;
    coro_ctx *coctx = (coro_ctx *)serial->task->arg;
    coctx->curco = wco;
    mco_result rtn = mco_resume(wco);
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
    FREE(nxt);
    if (MCO_DEAD == mco_status(wco)) {// 池满导致 _coro_mco_cb 返回，协程已死亡，须在此释放
        mco_destroy(wco);
    }
}
int32_t coro_serial_call(coro_serial_ctx *serial,
                    void (*func)(task_ctx *task, void *arg),
                    void *arg) {
    coro_ctx *coctx = (coro_ctx *)serial->task->arg;
    if (NULL == coctx->curco) {
        // 非协程上下文
        LOG_WARN("task %s, coro_serial_call called outside coroutine context.", _NAME_OR(serial->task->name));
        return ERR_FAILED;
    }
    // 缓存 self 到局部变量：mco_yield 期间 coctx->curco 被 _coro_serial_release
    // 改写为下一个被唤醒的协程；本协程被再次唤醒时 coctx->curco 会被还原指回 self,
    mco_coro *self = coctx->curco;
    if (NULL != serial->current && serial->current != self) {
        // ── 跨协程路径：锁被其他协程持有，需排队等待 ─────────────────────
        // 1) MALLOC 新 waiter 节点，单向链表 tail 入队保证 FIFO 顺序
        // 2) mco_yield(self) 挂起当前协程，控制权交回 task 消息循环
        // 3) 唤醒由前一个持锁协程在 _coro_serial_release 内完成：
        //    - FREE(node) → current=self → ref=1 → coctx->curco=self → mco_resume(self)
        // 4) 所以本路径不重复 current/ref 赋值，唤醒方已代劳；node 也已 FREE
        // 频繁 MALLOC/FREE：若 cs 高频可改 intrusive 栈节点，目前按简单实现走
        serial_node *node;
        MALLOC(node, sizeof(*node));
        node->co = self;
        node->next = NULL;
        if (NULL != serial->tail) {
            serial->tail->next = node;
        } else {
            serial->head = node;
        }
        serial->tail = node;
        mco_result rtn = mco_yield(self);
        ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
        // 唤醒后状态：serial->current==self, serial->ref==1, node 已 FREE
    } else {
        // ── 无锁或同协程嵌套路径 ─────────────────────────────────────
        // current==NULL：占据锁，current=self, ref 从 0 → 1
        // current==self：同协程嵌套调用（如 cs 内再 cs），仅 ref++ 不死锁,
        //                 由 _coro_serial_release 内 ref 计数管理出口
        if (NULL == serial->current) {
            serial->current = self;
        }
        serial->ref++;
    }
    // 临界区主体：func 内任意 yield（coro_sleep / coro_send / coro_request 等）期间，
    // 锁仍由 self 持有（serial->current 不变），其他协程进 cs 走"跨协程路径"挂起。
    // C 无 xpcall：func 内 abort/segfault 直接终止进程，本函数不兜底（与 coro_fork 同约定）
    func(serial->task, arg);
    // 出口：ref--；归 0 时取队头 waiter 唤醒下一位，未归 0（嵌套层）保留 current 给外层
    _coro_serial_release(serial);
    // release 内若唤醒了 waiter 且 waiter 在 func 内 yield，控制权回到此处时
    // coctx->curco 仍是 waiter（stale，已挂起非 RUNNING）；本协程返回上层前必须还原为
    // self，否则上层下次 coro_*（sleep/send/request 等）通过 coctx->curco 调
    // mco_yield 会读到 stale waiter，触发 MCO_NOT_RUNNING abort
    coctx->curco = self;
    return ERR_OK;
}
// 把一条挂起协程信息追加到 binary；C 协程无栈回溯,仅 sess / mtype / 挂起时长
static void _coro_dump_one(binary_ctx *bw, uint64_t sess, const coro_info *ci, uint64_t now) {
    if (NULL == ci->co) {
        return;
    }
    binary_set_va(bw, "sess=%" PRIu64 " mtype=%s age=%" PRIu64 "ms\n",
        sess, _message_str(ci->mtype), now - ci->since);
}
char *coro_dump(task_ctx *task, size_t *size) {
    if (TASK_MCO != task_get_type(task)
        || NULL == task->arg) {
        SET_PTR(size, 0);
        return NULL;
    }
    coro_ctx *coctx = (coro_ctx *)task->arg;
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    coro_info *ci;
    coro_sess *corosess;
    size_t iter = 0;
    int32_t total = 0;
    uint32_t n, i;
    uint64_t now = timer_cur_ms(&coctx->timer);
    while (hashmap_iter(coctx->mapco, &iter, (void **)&corosess)) {
        if (corosess->disposable) {
            if (NULL != corosess->coinfo.co) {
                _coro_dump_one(&bw, corosess->sess, &corosess->coinfo, now);
                total++;
            }
        } else {
            n = queue_size(&corosess->qucoinfo);
            for (i = 0; i < n; i++) {
                ci = queue_at(&corosess->qucoinfo, i);
                if (NULL != ci && NULL != ci->co) {
                    _coro_dump_one(&bw, corosess->sess, ci, now);
                    total++;
                }
            }
        }
    }
    binary_set_va(&bw, "%d suspended coro(s).", total);
    SET_PTR(size, bw.offset);
    return bw.data;
}
