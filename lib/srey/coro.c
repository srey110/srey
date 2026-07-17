#include "srey/coro.h"
#include "protocol/prots.h"
#include "containers/hashmap.h"
#include "containers/heap.h"
#include "utils/timer.h"
#include "utils/binary.h"
#include "utils/pool.h"
#include "containers/slist.h"
#define MINICORO_IMPL
#include "srey/minicoro.h"

#define COROPOOL_CAP      128
#define COROPOOL_MIN_KEEP 4
#define TEPOOL_CAP ONEK

// 超时堆节点：嵌入最小堆，存储过期时间和关联 session
typedef struct timeout_entry {
    heap_node hnode;     // 必须在首位，供 UPCAST 使用
    uint64_t timeout;   // 到期时间戳（毫秒）
    uint64_t sess;      // 关联的 session ID
} timeout_entry;
// 从堆节点指针还原 timeout_entry 指针
#define _TE_FROM_HNODE(n) UPCAST(n, timeout_entry, hnode)
// 单个挂起协程的等待信息
typedef struct coro_info {
    list_node node;   // 挂载到 coro_sess.waiters
    msg_type mtype;   // 期望唤醒的消息类型
    mco_coro *co;      // 挂起的协程对象
    uint64_t timeout; // 绝对到期时间（毫秒），0 表示无超时
    uint64_t since;   // 挂起起始时刻（毫秒），用于 debug dump 计算挂起时长
    timeout_entry *te;      // 非 NULL 表示已注册到超时堆
}coro_info;
// session 到挂起协程的映射节点
typedef struct coro_sess {
    int32_t keep;       // waiters 摘空后是否保留本条目：0 立即删除 mapco 条目；1 保留（TCP/UDP 同一 skid 高频复用，免去反复 hashmap 删除+插入），仅 _coro_handle_closed 会强制清零并真正删除
    uint64_t sess;      // session ID（一次性请求或 skid）
    list_ctx waiters;   // 挂起协程链表（元素 coro_info，严格按 FIFO 顺序等待/唤醒：仅队头 mtype 匹配才摘除）
}coro_sess;
// fork_wait 屏障：栈分配于 coro_fork_wait 内，子协程跑完 stub 递减 pending；
// 归零时唤醒 waiter（栈生命周期到 coro_fork_wait return 才结束，覆盖 yield 期间）；
// yield 期间挂入 coctx->fork_barriers 链表，task 关闭时由 _coro_ctx_free 兜底 destroy
typedef struct fork_barrier {
    list_node node;             // coctx->fork_barriers 侵入式链表节点（slist，UPCAST 复原外层）
    int32_t pending;            // 未完成的 fork 子协程数；stub 跑完递减 1，归零唤醒 waiter
    mco_coro *waiter;           // 等待 barrier 归零的父协程；pending=0 时 mco_resume 唤醒
} fork_barrier;
// 协程任务的运行时上下文，挂在 task->arg
typedef struct coro_ctx {
    int32_t nyield;              // 当前挂起（yield）中的协程数量
    mco_coro *curco;             // 正在运行的协程指针
    struct hashmap *mapco;       // sess → coro_sess 哈希映射
    void *arg;                   // 用户自定义数据
    free_cb _arg_free;           // 用户数据释放回调
    uint64_t shrink_ms;          // 上次协程池收缩的时间戳(ms)，按 SHRINK_TIME 门控
    list_ctx fork_barriers;      // 所有挂起的 fork_wait 屏障链表（slist）；task 关闭时由 _coro_ctx_free 兜底 destroy
    pool_ctx copool;             // 空闲协程对象池（元素 mco_coro *，含负载趋势）
    pool_ctx te_pool;            // 空闲 timeout_entry 对象池，容量 TEPOOL_CAP，不参与周期性收缩
    pool_ctx coinfo_pool;        // 空闲 coro_info 节点池，容量 TEPOOL_CAP，不参与周期性收缩
    timer_ctx timer;             // 用于获取当前毫秒时间戳
    heap_ctx timeout_heap;       // 按到期时间排序的最小堆,O(1) 检查最早超时
}coro_ctx;
// fork_wait 内部包装：每个 func 配一份 slot，stub 跑完后释放
typedef struct fork_wait_slot {
    void (*func)(task_ctx *task, void *arg);      // 业务回调函数
    void *arg;                                    // 透传给 func 的参数（生命周期由 coro_fork_wait 调用方管理）
    fork_barrier *barrier;                        // 所属 barrier 指针；stub 跑完递减 barrier->pending
} fork_wait_slot;
// serial waiter 链表节点：cs 挂起协程的 FIFO 元素，进队时 MALLOC、出队由前一个协程的 _coro_serial_release FREE
typedef struct serial_node {
    list_node node;           // 侵入式 FIFO 链表节点（slist，UPCAST 复原外层）
    mco_coro *co;             // 等待中的协程
} serial_node;
struct coro_serial_ctx {
    int32_t ref;           // 嵌套深度（同协程多次进入累加）
    task_ctx *task;        // 所属 task；resume 时同步 coctx->curco 需要
    mco_coro *current;     // 当前持锁协程；NULL 表示无锁
    list_ctx waiters;      // 挂起 waiter 的 FIFO（元素 serial_node，UPCAST 复原）
};

static mco_desc _coro_desc; // 全局协程描述符，由 coro_desc_init 初始化

// 最小堆比较函数：timeout 小的优先（堆顶是最早到期的）
static int _coro_timeout_cmp(const heap_node *lhs, const heap_node *rhs) {
    return _TE_FROM_HNODE(lhs)->timeout < _TE_FROM_HNODE(rhs)->timeout;
}
// 创建 timeout_entry 并插入超时堆，返回堆节点指针（用于后续删除）
static timeout_entry *_coro_te_insert(coro_ctx *coctx, uint64_t timeout, uint64_t sess) {
    timeout_entry *te = (timeout_entry *)pool_pop(&coctx->te_pool, NULL, 0);
    te->hnode.parent = te->hnode.left = te->hnode.right = NULL;
    te->timeout = timeout;
    te->sess = sess;
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
// 将挂起的协程注册到 mapco
// keep 0: 链表为空,主动从map移除节点,其他：不主动移除节点，在close消息后强制设置为0
static void _coro_cosess_set(task_ctx *task, mco_coro *coro, uint64_t sess, msg_type mtype, uint32_t ms) {
    coro_ctx *coctx = task->arg;
    uint64_t now = timer_cur_ms(&coctx->timer);
    coro_info *coinfo = (coro_info *)pool_pop(&coctx->coinfo_pool, NULL, 0);
    coinfo->since = now;
    coinfo->timeout = ms > 0 ? now + ms : 0;
    coinfo->co = coro;
    coinfo->mtype = mtype;
    coinfo->te = ms > 0 ? _coro_te_insert(coctx, coinfo->timeout, sess) : NULL;
    coro_sess key;
    key.sess = sess;
    coro_sess *cofind = (coro_sess *)hashmap_get(coctx->mapco, &key);
    if (NULL != cofind) {
        list_push_tail(&cofind->waiters, &coinfo->node);
    } else {
        coro_sess cosess;
        cosess.sess = sess;
        cosess.keep = _message_may_keep(mtype);
        list_init(&cosess.waiters);
        list_push_tail(&cosess.waiters, &coinfo->node);
        hashmap_set(coctx->mapco, &cosess);
    }
}
// 从 mapco 中删除指定 sess 的记录
static void _coro_cosess_delete(coro_ctx *coctx, uint64_t sess) {
    coro_sess key;
    key.sess = sess;
    hashmap_delete(coctx->mapco, &key);
}
// 从 mapco 查找匹配 sess 的挂起协程节点，仅检测队头：mtype 匹配才摘除返回，
// 队头不匹配（含 keep 保留的空条目）视为无等待者，不越过队头继续查找（保持严格 FIFO）；
// 摘除后链表为空且 !keep 时才删除 mapco 条目
static coro_info *_coro_cosess_get(coro_ctx *coctx, uint64_t sess, msg_type mtype) {
    coro_sess key;
    key.sess = sess;
    coro_sess *cofind = (coro_sess *)hashmap_get(coctx->mapco, &key);
    if (NULL == cofind || list_empty(&cofind->waiters)) {
        return NULL;
    }
    coro_info *coinfo = UPCAST(cofind->waiters.head, coro_info, node);
    if (mtype != coinfo->mtype) {
        return NULL;
    }
    list_remove(&cofind->waiters, &coinfo->node);
    if (list_empty(&cofind->waiters) && !cofind->keep) {
        _coro_cosess_delete(coctx, sess);
    }
    return coinfo;
}
// 从 coinfo 取出协程对象，清理其超时堆节点（如果有），并归还 coinfo 节点到对象池
static inline mco_coro *_coro_take_mco(coro_ctx *coctx, coro_info *coinfo) {
    mco_coro *co = coinfo->co;
    if (NULL != coinfo->te) {
        heap_remove(&coctx->timeout_heap, &coinfo->te->hnode);
        pool_push(&coctx->te_pool, coinfo->te, 0);
    }
    pool_push(&coctx->coinfo_pool, coinfo, 0);
    return co;
}
// 协程主循环：每次 resume 后弹出分发参数指针，执行消息处理，结束后归还协程到对象池
static void _coro_mco_cb(mco_coro *coro) {
    mco_result rtn;
    task_dispatch_arg *argp;
    task_dispatch_arg arg;
    coro_ctx *ctx;
    for (;;) {
        rtn = mco_yield(coro);
        ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
        // 弹出 8 字节指针并在协程栈上复制一份，保证 arg.fd/arg.skid 在整个生命期内有效
        rtn = mco_pop(coro, &argp, sizeof(argp));
        ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
        arg = *argp; // 在协程栈上保存一份副本
        task_incref(arg.task); // 保证 _message_run 在 yield 后 task 不会被释放
        _message_run(arg.task, &arg.msg);
        ctx = (coro_ctx *)arg.task->arg;
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
    pool_init(&coctx->te_pool, sizeof(timeout_entry), TEPOOL_CAP, 0, 0, NULL);
    pool_init(&coctx->coinfo_pool, sizeof(coro_info), TEPOOL_CAP, 0, 0, NULL);
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
    pool_free(&coctx->te_pool);
    pool_free(&coctx->coinfo_pool);
    /* 先释放超时堆（堆节点独立分配，不依赖 mapco） */
    timeout_entry *te;
    while (NULL != coctx->timeout_heap.root) {
        te = _TE_FROM_HNODE(coctx->timeout_heap.root);
        heap_dequeue(&coctx->timeout_heap);
        FREE(te);
    }
    size_t iter = 0;
    coro_sess *corosess;
    // 注意：上面已释放整个 timeout_heap，此处 coinfo->te 均为悬空指针，禁止解引用；
    // 仅销毁 coinfo->co 协程对象及 coinfo 节点本身即可（直接 FREE，同 te 一样不必归还对象池）
    coro_info *ci;
    while (hashmap_iter(coctx->mapco, &iter, (void **)&corosess)) {
        list_foreach_safe(&corosess->waiters, wit, wtmp) {
            ci = UPCAST(wit, coro_info, node);
            if (NULL != ci->co) {
                mco_destroy(ci->co);
            }
            FREE(ci);
        }
    }
    hashmap_free(coctx->mapco);
    fork_barrier *fb;
    list_foreach_safe(&coctx->fork_barriers, ln, tmp) {
        fb = UPCAST(ln, fork_barrier, node);
        mco_destroy(fb->waiter);
    }
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
// 统一唤醒尾部：找到匹配等待者则唤醒；否则 warn!=0 时先告警(未找到即逻辑异常，与是否新建协程无关)，
// 再按 miss_create 决定新建协程处理(!=0)还是丢弃(==0，TIMEOUT 专属：正常情况下已被正常路径消费)
static inline void _coro_dispatch(task_dispatch_arg *arg, int32_t miss_create, int32_t warn) {
    if (0 == arg->msg.sess) {
        _coro_mco_create(arg);
        return;
    }
    coro_ctx *coctx = arg->task->arg;
    coro_info *coinfo = _coro_cosess_get(coctx, arg->msg.sess, arg->msg.mtype);
    if (NULL == coinfo) {
        if (warn) {
            LOG_WARN("can't find session, maybe logic error. msg_type %d.", (int32_t)arg->msg.mtype);
        }
        if (!miss_create) {
            return;
        }
        _coro_mco_create(arg);
        return;
    }
    mco_coro *coro = _coro_take_mco(coctx, coinfo);
    _coro_mco_resume(coro, arg);
}
static void _coro_handle_timeout(task_dispatch_arg *arg) {
    _coro_dispatch(arg, 0, 1);
}
static void _coro_handle_connect(task_dispatch_arg *arg) {
    _coro_dispatch(arg, 1, 0);
}
static void _coro_handle_sslexchanged(task_dispatch_arg *arg) {
    _coro_dispatch(arg, 1, 0);
}
static void _coro_handle_handshaked(task_dispatch_arg *arg) {
    _coro_dispatch(arg, 1, 0);
}
static void _coro_handle_response(task_dispatch_arg *arg) {
    _coro_dispatch(arg, 1, 1);
}
// 处理数据接收消息：sess==0 或协议不允许 resume 则新建协程，否则唤醒等待的协程
static void _coro_handle_recved(task_dispatch_arg *arg) {
    if (0 == arg->msg.sess
        || ERR_OK != prots_may_resume(arg->msg.subtype, arg->msg.data)) {
        _coro_mco_create(arg);
        return;
    }
    _coro_dispatch(arg, 1, 0);
}
// 处理连接关闭消息：进入时探测一次 mapco，把该 sess(连接类即 skid)下全部挂起等待者转移到本地链表再消费，
// resume 期间业务代码在同一 sess 上重新注册的等待不会被本轮循环看到，而是追加回同一个 mapco 条目；
// 新建协程处理关闭事件后重新查询该条目，仍为空才删除，避免 resume 期间的重新注册被误删/重复插入
static void _coro_handle_closed(task_dispatch_arg *arg) {
    coro_ctx *coctx = arg->task->arg;
    coro_sess key;
    key.sess = arg->msg.sess;
    coro_sess *cofind = (coro_sess *)hashmap_get(coctx->mapco, &key);
    if (NULL != cofind) {
        cofind->keep = 0;// 连接已关闭，让后续注册的coro能主动移除
        list_ctx local = cofind->waiters;
        list_init(&cofind->waiters);
        list_node *node;
        coro_info *coinfo;
        mco_coro *coro;
        while (NULL != (node = list_pop_head(&local))) {
            coinfo = UPCAST(node, coro_info, node);
            coro = _coro_take_mco(coctx, coinfo);
            _coro_mco_resume(coro, arg);
        }
    }
    // erro!=ERR_OK 是 prots_net_connect 因连接失败补发的合成 CLOSE（见该函数），不触发 on_close 观察者
    if (ERR_OK == arg->msg.erro) {
        _coro_mco_create(arg);
    }
    /* resume 期间协程可能重新在同一 sess 上注册等待（追加到 cofind->waiters），
     * 也可能因其它 sess 的插入触发 hashmap resize 导致 cofind 悬空，须重新查询而非复用旧指针 */
    cofind = (coro_sess *)hashmap_get(coctx->mapco, &key);
    if (NULL != cofind && list_empty(&cofind->waiters)) {
        _coro_cosess_delete(coctx, arg->msg.sess);
    }
}
// 处理 UDP 数据接收消息：UDP 本身不保证顺序与送达，找不到等待者（迟到/孤儿包）是正常场景，不告警；
// sess==0 由 _coro_dispatch 内部自行新建协程处理
static void _coro_handle_recvfrom(task_dispatch_arg *arg) {
    _coro_dispatch(arg, 1, 0);
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
        timeout_entry *te;
        coro_sess key, *cosess;
        mco_coro *coro;
        coro_info *coinfo, *probe;
        while (NULL != coctx->timeout_heap.root) {
            te = _TE_FROM_HNODE(coctx->timeout_heap.root);
            if (te->timeout > now) {
                break; /* 最早的都没到期，无需继续 */
            }
            heap_dequeue(&coctx->timeout_heap);
            key.sess = te->sess;
            cosess = (coro_sess *)hashmap_get(coctx->mapco, &key);
            if (NULL == cosess) {
                /* 已被正常路径消费（_coro_cosess_get 已删堆节点），此处只需释放 te */
                pool_push(&coctx->te_pool, te, 0);
                continue;
            }
            /* 链表按 push 序排列，但 te 在堆中按 timeout 排序：
             * 若两次 push 的 timeout 不同，先到期的 te 对应的 coinfo
             * 不一定是队首，需按 coinfo->te 精确定位。 */
            coinfo = NULL;
            list_foreach(&cosess->waiters, it) {
                probe = UPCAST(it, coro_info, node);
                if (probe->te == te) {
                    coinfo = probe;
                    list_remove(&cosess->waiters, it);
                    break;
                }
            }
            if (NULL == coinfo) {
                pool_push(&coctx->te_pool, te, 0);
                continue;
            }
            coinfo->te = NULL; /* 堆节点已由 heap_dequeue 移除 */
            coro = coinfo->co;
            LOG_INFO("task %s message type %d session %"PRIu64" timeout.",
                     _NAME_OR(task->name), coinfo->mtype, te->sess);
            pool_push(&coctx->coinfo_pool, coinfo, 0);
            /* 摘除后链表为空且 !keep 时删除 mapco 条目，与 _coro_cosess_get 的清理时机保持一致 */
            if (list_empty(&cosess->waiters) && !cosess->keep) {
                _coro_cosess_delete(coctx, te->sess);
            }
            arg.msg.sess = te->sess;
            pool_push(&coctx->te_pool, te, 0);
            _coro_mco_resume(coro, &arg);
        }
    }
    if (now - coctx->shrink_ms >= SHRINK_TIME) {
        coctx->shrink_ms = now;
        pool_shrink(&coctx->copool, shrink_nkeep(pool_size(&coctx->copool)), SHRINK_BUSY);
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
    [MSG_TYPE_STARTUP]      = _coro_handle_startup,// 新建
    [MSG_TYPE_CLOSING]      = _coro_handle_closing,// 新建
    [MSG_TYPE_TIMEOUT]      = _coro_handle_timeout,// 新建或唤醒
    [MSG_TYPE_ACCEPT]       = _coro_mco_create,// 新建
    [MSG_TYPE_CONNECT]      = _coro_handle_connect, // 连接建立；未找到静默新建
    [MSG_TYPE_SSLEXCHANGED] = _coro_handle_sslexchanged, // SSL 握手；未找到静默新建
    [MSG_TYPE_HANDSHAKED]   = _coro_handle_handshaked, // 应用层握手；未找到静默新建
    [MSG_TYPE_RECV]         = _coro_handle_recved,// sess==0 或协议不允许 创建；未找到新建，否则唤醒
    [MSG_TYPE_SEND]         = _coro_mco_create,// 新建
    [MSG_TYPE_CLOSE]        = _coro_handle_closed,// sess直接赋值skid,尝试唤醒所有
    [MSG_TYPE_RECVFROM]     = _coro_handle_recvfrom,// sess 0新建；未找到静默新建，不告警
    [MSG_TYPE_REQUEST]      = _coro_mco_create,// 新建
    [MSG_TYPE_RESPONSE]     = _coro_handle_response,// 未找到告警后新建，否则唤醒
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
message_ctx *_coro_wait(task_ctx *task, uint64_t sess, msg_type mtype, uint32_t ms) {
    coro_ctx *coctx = task->arg;
    _coro_cosess_set(task, coctx->curco, sess, mtype, ms);
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
     * dispatch 函数以相同 key 查找协程后调用 _coro_mco_resume；若此断言触发，说明 dispatch 逻辑有 bug。*/
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
// 等待 SSL 交换完成消息，超时或连接关闭时关闭连接并返回 ERR_FAILED
static int32_t _wait_ssl_exchanged(task_ctx *task, SOCKET fd, uint64_t skid) {
    message_ctx *msg = _coro_wait(task, skid, MSG_TYPE_SSLEXCHANGED, task_get_netread_timeout(task));
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
    message_ctx *msg = _coro_wait(task, skid, MSG_TYPE_HANDSHAKED, task_get_netread_timeout(task));
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
int32_t coro_wait_connect(task_ctx *task, SOCKET fd, uint64_t skid, struct evssl_ctx *evssl) {
    if (INVALID_SOCK == fd) {
        return ERR_FAILED;
    }
    message_ctx *msg = _coro_wait(task, skid, MSG_TYPE_CONNECT, task_get_connect_timeout(task));
    if (MSG_TYPE_TIMEOUT == msg->mtype) {
        ev_close(&task->loader->netev, fd, skid, 1);
        LOG_WARN("task %s, connect timeout, skid %"PRIu64".", _NAME_OR(task->name), skid);
        return ERR_FAILED;
    }
    if (MSG_TYPE_CLOSE == msg->mtype) {
        return ERR_FAILED;
    }
    if (ERR_OK != msg->erro) {
        LOG_WARN("task %s, connect error, skid %"PRIu64".", _NAME_OR(task->name), skid);
        return ERR_FAILED;
    }
    if (NULL != evssl) {
        if (ERR_OK != _wait_ssl_exchanged(task, fd, skid)) {
            return ERR_FAILED;
        }
    }
    return ERR_OK;
}
int32_t coro_connect(task_ctx *task, pack_type pktype,
                     struct evssl_ctx *evssl, const char *ip, uint16_t port,
                     int32_t netev, void *extra,
                     SOCKET *fd, uint64_t *skid) {
    if (ERR_OK != task_connect(task, pktype, evssl, ip, port, netev, extra, 1, fd, skid)) {
        LOG_WARN("task: %s, connect %s:%d error.", _NAME_OR(task->name), ip, port);
        return ERR_FAILED;
    }
    return coro_wait_connect(task, *fd, *skid, evssl);
}
void coro_close(task_ctx *task, SOCKET fd, uint64_t skid, int32_t immed) {
    if (INVALID_SOCK == fd) {
        return;
    }
    ev_close(&task->loader->netev, fd, skid, immed);
    _coro_wait(task, skid, MSG_TYPE_CLOSE, task_get_netread_timeout(task));
}
// 等待指定连接的下一条接收消息，超时或连接关闭时返回 NULL
// 返回的指针在下次 _coro_wait 调用前有效
static message_ctx *_coro_wait_recved(task_ctx *task, SOCKET fd, uint64_t skid) {
    message_ctx *msg = _coro_wait(task, skid, MSG_TYPE_RECV, task_get_netread_timeout(task));
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
    // 非分片完整消息(slice==0)也视为末片,通用客户端 while(!end) 循环不会误判还有后续分片而挂到超时
    *end = (PROT_SLICE_END == msg->slice || 0 == msg->slice) ? 1 : 0;
    SET_PTR(size, msg->size);
    return msg->data;
}
// 同步收发前须由调用方显式 coro_sync 一次(与 TCP 服务端 accept 连接同约定,见文件头注释)；
// 之后同一 skid 上可连续多次调用；并发多次调用（不等上一次响应返回）时两次响应按到达顺序 FIFO
// 匹配给两次调用，若网络乱序仍可能与发送顺序不一致——UDP 协议本身无法避免的限制
void *coro_sendto(task_ctx *task, SOCKET fd, uint64_t skid,
                  const char *ip, const uint16_t port,
                  void *data, size_t len, size_t *size, int32_t copy) {
    if (ERR_OK != ev_sendto(&task->loader->netev, fd, skid, ip, port, data, len, copy)) {
        LOG_WARN("task %s, sendto error, skid %"PRIu64".", _NAME_OR(task->name), skid);
        return NULL;
    }
    message_ctx *msg = _coro_wait(task, skid, MSG_TYPE_RECVFROM, task_get_netread_timeout(task));
    if (MSG_TYPE_TIMEOUT == msg->mtype) {
        LOG_WARN("task %s, sendto timeout, skid %"PRIu64".", _NAME_OR(task->name), skid);
        return NULL;
    }
    if (MSG_TYPE_CLOSE == msg->mtype) {
        return NULL;
    }
    recvfrom_ctx *rfmsg = msg->data;
    SET_PTR(size, rfmsg->len);
    return rfmsg->data;
}
// fork_wait 内部 stub：跑用户 func 后递减 barrier，归零时同步 curco 并 mco_resume 唤醒 waiter
static void _coro_fork_wait_stub(task_ctx *task, void *arg) {
    fork_wait_slot *slot = (fork_wait_slot *)arg;
    slot->func(task, slot->arg);
    fork_barrier *b = slot->barrier;
    FREE(slot);
    if (0 == --b->pending) {
        // 与 _coro_mco_resume 同模式：mco_resume 前必须把 coctx->curco 同步为 waiter，
        // 否则 waiter 醒来后 _coro_wait/_coro_cosess_set 会用错协程标识进 cosess
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
    list_push_head(&coctx->fork_barriers, &barrier.node);// 入队
    ++coctx->nyield;
    mco_result rtn = mco_yield(coctx->curco);
    --coctx->nyield;
    // 并发 fork_wait 完成顺序非 LIFO，移除的可能非队头，按节点解链（勿改 pop_head）
    list_remove(&coctx->fork_barriers, &barrier.node);
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
    ASSERTAB(NULL == serial->current && 0 == serial->ref && list_empty(&serial->waiters),
             "coro_serial_free with active holders or waiters");
    FREE(serial);
}
// 临界区出口：ref 归 0 时取队头 waiter，先同步 current/ref 再 mco_resume；与 fork_wait_stub 同模式
static void _coro_serial_release(coro_serial_ctx *serial) {
    serial->ref--;
    if (0 != serial->ref) {
        return;
    }
    list_node *ln = list_pop_head(&serial->waiters);
    if (NULL == ln) {
        serial->current = NULL;
        return;
    }
    serial_node *nxt = UPCAST(ln, serial_node, node);
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
        // 1) MALLOC 新 waiter 节点，list_push_tail 入队保证 FIFO 顺序
        // 2) mco_yield(self) 挂起当前协程，控制权交回 task 消息循环
        // 3) 唤醒由前一个持锁协程在 _coro_serial_release 内完成：
        //    - FREE(nd) → current=self → ref=1 → coctx->curco=self → mco_resume(self)
        // 4) 所以本路径不重复 current/ref 赋值，唤醒方已代劳；nd 也已 FREE
        // 频繁 MALLOC/FREE：若 cs 高频可改栈分配 waiter 节点，目前按简单实现走
        serial_node *nd;
        MALLOC(nd, sizeof(*nd));
        nd->co = self;
        list_push_tail(&serial->waiters, &nd->node);
        mco_result rtn = mco_yield(self);
        ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
        // 唤醒后状态：serial->current==self, serial->ref==1, nd 已 FREE
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
    coro_sess *corosess;
    size_t iter = 0;
    int32_t total = 0;
    uint64_t now = timer_cur_ms(&coctx->timer);
    coro_info *ci;
    while (hashmap_iter(coctx->mapco, &iter, (void **)&corosess)) {
        list_foreach(&corosess->waiters, it) {
            ci = UPCAST(it, coro_info, node);
            if (NULL != ci->co) {
                _coro_dump_one(&bw, corosess->sess, ci, now);
                total++;
            }
        }
    }
    binary_set_va(&bw, "%d suspended coro(s).", total);
    SET_PTR(size, bw.offset);
    return bw.data;
}
