#include "srey/coro.h"
#include "protocol/prots.h"
#include "containers/hashmap.h"
#include "containers/heap.h"
#include "utils/timer.h"
#include "utils/binary.h"
#include "utils/pool.h"
#include "containers/slist.h"
#if defined(MCO_USE_VMEM_ALLOCATOR)
    #error "MCO_USE_VMEM_ALLOCATOR skips the C-allocator block, silently reverting coroutine stacks to mmap/VirtualAlloc"
#endif
// 协程栈改走框架分配器以计入 MEMORY_CHECK；minicoro 依赖零初始化的栈，必须用 _calloc
#define MCO_ALLOC(size) _calloc(1, size)
#define MCO_DEALLOC(ptr, size) _free(ptr)
#define MINICORO_IMPL
#include "srey/minicoro.h"

#define COROPOOL_CAP 128
#define NODEPOOL_CAP ONEK
#define COROPOOL_MIN_KEEP 4

typedef void (*_coro_msg_handler_t)(task_dispatch_arg *arg);

// 超时堆节点：嵌入最小堆，存储过期时间和关联 session
typedef struct timeout_entry {
    heap_node hnode;     // 必须在首位，供 UPCAST 使用
    uint64_t timeout;   // 到期时间戳（毫秒）
    uint64_t sess;      // 关联的 session ID
} timeout_entry;
// 从堆节点指针还原 timeout_entry 指针
#define _TE_FROM_HNODE(n) UPCAST(n, timeout_entry, hnode)
// 单个挂起协程的等待信息
// 到期时间不在这里存：权威副本在 te->timeout（超时堆按它排序与判定），
// 这边再留一份就是只写不读的死字段
typedef struct coro_info {
    list_node node;    // 挂载到 coro_sess.waiters
    mco_coro *co;      // 挂起的协程对象
    uint64_t since;    // 挂起起始时刻（毫秒），用于 debug dump 计算挂起时长
    timeout_entry *te; // 非 NULL 表示已注册到超时堆
    msg_type mtype;    // 期望唤醒的消息类型
}coro_info;
// session 到挂起协程的映射节点
typedef struct coro_sess {
    int32_t keep;       // waiters 摘空后是否保留本条目：0 立即删除 mapco 条目；1 保留（TCP/UDP 同一 skid 高频复用，免去反复 hashmap 删除+插入），仅 _coro_handle_closed 会强制清零并真正删除
    uint64_t sess;      // session ID（一次性请求或 skid）
    list_ctx waiters;   // 挂起协程链表（元素 coro_info，严格按 FIFO 顺序等待/唤醒：仅队头 mtype 匹配才摘除）
}coro_sess;
// fork 任务载荷：coro_fork / coro_fork_wait 均建此结构追加到 coctx->fork_pending，
// drain 时由 _coro_fork_run 跑 fkcb 后归还 fork_item_pool；fwctx=NULL 即 coro_fork(fire-and-forget) 退化态
typedef struct fork_item {
    list_node node;               // 挂 coctx->fork_pending
    fork_serial_cb fkcb;          // 用户函数
    void *arg;                    // 用户参数（生命周期由调用方管理）
    struct fork_wait_ctx *fwctx;  // NULL=coro_fork；非NULL=coro_fork_wait 成员，归零唤醒其 waiter
} fork_item;
// fork_wait 屏障：栈分配于 coro_fork_wait 内，子协程跑完 stub 递减 pending；
// 归零时唤醒 waiter（栈生命周期到 coro_fork_wait return 才结束，覆盖 yield 期间）；
// yield 期间挂入 coctx->fork_waited 链表，task 关闭时由 _coro_ctx_free 兜底 destroy
typedef struct fork_wait_ctx {
    list_node node;             // coctx->fork_waited 侵入式链表节点（slist，UPCAST 复原外层）
    int32_t waited;             // 未完成的 fork 子协程数；_coro_fork_run 跑完递减 1，归零唤醒 waiter
    mco_coro *waiter;           // 等待归零的父协程；waited=0 时 mco_resume 唤醒
    uint64_t since;             // 挂起时刻(ms)，coro_dump 据此报"等了多久"，字段与用法同 serial_node
} fork_wait_ctx;
// serial waiter 链表节点：cs 挂起协程的 FIFO 元素，进队时 pool_pop、出队由前一个协程的 coro_serial_leave pool_push
typedef struct serial_node {
    list_node node;           // 侵入式 FIFO 链表节点（slist，UPCAST 复原外层），须在首位
    int32_t aborted;          // 1 = 被 coro_serial_free 唤醒，锁未交接给本协程，须失败返回
    mco_coro *co;             // 等待中的协程
    uint64_t since;           // 入队时刻(ms)，coro_dump 据此报"排了多久"
} serial_node;
struct coro_serial_ctx {
    list_node node;        // 挂 coctx->serials（侵入式，须在首位）；仅供 coro_dump 遍历，
                           // 摘挂点严格对齐三处生死：coro_serial_new 挂、两处 FREE(serial) 前摘
    int32_t ref;           // 嵌套深度（同协程多次进入累加）
    int32_t closed;        // 1 = 已关闭，此后任何 enter 一律失败，不再有人能拿到锁
    task_ctx *task;        // 所属 task；resume 时同步 coctx->curco 需要
    mco_coro *current;     // 当前持锁协程；NULL 表示无锁
    list_ctx waiters;      // 挂起 waiter 的 FIFO（元素 serial_node，UPCAST 复原）
};
// 协程任务的运行时上下文，挂在 task->arg
typedef struct coro_ctx {
    int32_t nyield;              // 当前挂起（yield）中的协程数量
    mco_coro *curco;             // 正在运行的协程指针
    struct hashmap *mapco;       // sess → coro_sess 哈希映射
    void *arg;                   // 用户自定义数据
    free_cb _arg_free;           // 用户数据释放回调
    uint64_t shrink_ms;          // 上次协程池收缩的时间戳(ms)，按 SHRINK_TIME 门控
    list_ctx fork_pending;       // 待起协程的 fork_item FIFO（slist，task-local 无界无锁）；每次 dispatch 末尾 drain 到空
    list_ctx fork_waited;        // 挂起的 fork_wait 父协程链表（slist，元素 fork_wait_ctx）；task 关闭时由 _coro_ctx_free 兜底 destroy
    list_ctx serials;            // 活跃的命令串行化执行器链表（slist，元素 coro_serial_ctx）；供 coro_dump 遍历，
                                 // 正常由 *_quit 释放，task 销毁时 _coro_ctx_free 兜底
    pool_ctx copool;             // 空闲协程对象池（元素 mco_coro *，含负载趋势）
    pool_ctx te_pool;            // 空闲 timeout_entry 对象池，容量 NODEPOOL_CAP，不参与周期性收缩
    pool_ctx coinfo_pool;        // 空闲 coro_info 节点池，容量 NODEPOOL_CAP，不参与周期性收缩
    pool_ctx fork_item_pool;     // 空闲 fork_item 节点池，容量 NODEPOOL_CAP，不参与周期性收缩
    pool_ctx serial_node_pool;   // 空闲 serial_node 节点池，容量 NODEPOOL_CAP，不参与周期性收缩
    timer_ctx timer;             // 用于获取当前毫秒时间戳
    heap_ctx timeout_heap;       // 按到期时间排序的最小堆,O(1) 检查最早超时
}coro_ctx;

static mco_desc _coro_desc; // 全局协程描述符，由 coro_desc_init 初始化

static void _coro_fork_run(task_ctx *task, fork_item *item);
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
    coinfo->co = coro;
    coinfo->mtype = mtype;
    coinfo->te = ms > 0 ? _coro_te_insert(coctx, now + ms, sess) : NULL;
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
        task_incref(arg.task); // 保证回调在 yield 后 task 不会被释放
        if (MSG_TYPE_FORK == arg.msg.mtype) {
            _coro_fork_run(arg.task, (fork_item *)arg.msg.data);// fork 走 coro 本地 runner，不绕 task.c
        } else {
            _message_run(arg.task, &arg.msg);
        }
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
    pool_cbs _coro_pool_cbs = { _coro_new, _coro_free, NULL, NULL };
    coro_ctx *coctx;
    CALLOC(coctx, 1, sizeof(coro_ctx));
    coctx->arg = arg;
    coctx->_arg_free = _argfree;
    pool_init(&coctx->copool, 0, COROPOOL_CAP, COROPOOL_MIN_KEEP, 0, &_coro_pool_cbs);
    pool_init(&coctx->te_pool, sizeof(timeout_entry), NODEPOOL_CAP, 0, 0, NULL);
    pool_init(&coctx->coinfo_pool, sizeof(coro_info), NODEPOOL_CAP, 0, 0, NULL);
    pool_init(&coctx->fork_item_pool, sizeof(fork_item), NODEPOOL_CAP, 0, 0, NULL);
    pool_init(&coctx->serial_node_pool, sizeof(serial_node), NODEPOOL_CAP, 0, 0, NULL);
    timer_init(&coctx->timer);
    coctx->shrink_ms = timer_cur_ms(&coctx->timer);
    coctx->mapco = hashmap_new(sizeof(coro_sess), ONEK, 0, 0,
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
    pool_free(&coctx->fork_item_pool);
    pool_free(&coctx->serial_node_pool);
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
                _coro_free(ci->co);// 走同一个销毁点，失败有日志
            }
            FREE(ci);
        }
    }
    hashmap_free(coctx->mapco);
    fork_wait_ctx *fw;
    list_foreach_safe(&coctx->fork_waited, ln, tmp) {
        fw = UPCAST(ln, fork_wait_ctx, node);
        _coro_free(fw->waiter);
    }
    // fork_pending 正常路径每次 dispatch 末尾已 drain 空，此处兜底清未起的 item（不跑 fkcb）
    fork_item *fi;
    list_foreach_safe(&coctx->fork_pending, fln, ftmp) {
        fi = UPCAST(fln, fork_item, node);
        FREE(fi);
    }
    //释放用户数据
    if (NULL != coctx->_arg_free
        && NULL != coctx->arg) {
        coctx->_arg_free(coctx->arg);
    }
    // 业务不显式 quit(靠进程退出回收)时在此兜底,须排在 _arg_free 之后:析构里的 *_quit / *_ping
    // 头一件事就是读 X->serial。不必唤醒排队者:挂起的协程持着 task ref,ref 未归零进不来本函数
    coro_serial_ctx *serial;
    list_foreach_safe(&coctx->serials, sln, stmp) {
        serial = UPCAST(sln, coro_serial_ctx, node);
        FREE(serial);
    }
    FREE(coctx);
}
// 从协程对象池取出可用协程，池为空时新建并首次 resume 到第一个 yield 点
static mco_coro *_coro_pool_get(task_ctx *task) {
    coro_ctx *coctx = task->arg;
    return (mco_coro *)pool_pop(&coctx->copool, NULL, 0);
}
// 切到另一个协程跑,回来再把 curco 指回调用者。curco 是"当前在跑的协程"这一唯一标识:
// 被唤醒者醒来后靠它进 cosess、调 mco_yield,调用者拿回控制权后同样靠它。
// 漏还原会把 curco 留在已挂起(甚至已随池收缩销毁)的协程上——调用者下一次 coro_sleep / coro_send
// 对着它 mco_yield 撞 MCO_NOT_RUNNING;顶层漏清则 coro_fork / coro_fork_wait / coro_serial_enter
// 三处"不在协程里就拒绝"的守卫从第一条消息起永远不成立。
// 顶层调用的"原值"就是 NULL,同样由本函数还原,不必各写一份
static mco_result _coro_resume_switch(coro_ctx *coctx, mco_coro *co) {
    mco_coro *self = coctx->curco;
    coctx->curco = co;
    mco_result rtn = mco_resume(co);
    coctx->curco = self;
    return rtn;
}
// 唤醒尾部:切过去、断言、回收死协程。五个唤醒点共用,别再各写一份——
// curco 漏还原是上面那个 abort,MCO_DEAD 漏回收是无声的协程栈泄漏(只在池满时发生)。
// co 必须是调用方先缓存好的指针:被唤醒者返回后它栈上的对象即失效,不能再从那些对象里取 co
static inline void _coro_resume_reap(coro_ctx *coctx, mco_coro *co) {
    mco_result rtn = _coro_resume_switch(coctx, co);
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
    if (MCO_DEAD == mco_status(co)) {
        _coro_free(co);// 池满导致 _coro_mco_cb 返回,协程已死亡,须在此释放
    }
}
// 从对象池取出协程并推入分发参数，开始执行新的消息处理流程。
// 五个调用点(消息分发表各项与 _coro_drain_forks)全在顶层,curco 恒为 NULL
static void _coro_mco_create(task_dispatch_arg *arg) {
    coro_ctx *coctx = arg->task->arg;
    mco_coro *co = _coro_pool_get(arg->task);
    // 推入 8 字节指针而非整个结构体，由 _coro_mco_cb 在 resume 后自行复制
    mco_result rtn = mco_push(co, &arg, sizeof(arg));
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
    _coro_resume_reap(coctx, co);
}
// 唤醒已挂起的协程，推入消息指针后 resume，返回后清理消息资源
static void _coro_mco_resume(mco_coro *coro, task_dispatch_arg *arg) {
    coro_ctx *coctx = arg->task->arg;
    // 推入 8 字节消息指针，避免拷贝整个 message_ctx
    message_ctx *msgptr = &arg->msg;
    mco_result rtn = mco_push(coro, &msgptr, sizeof(msgptr));
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
    _coro_resume_reap(coctx, coro);
    _message_clean(&arg->msg);
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
// CONNECT / SSLEXCHANGED / HANDSHAKED / RECVFROM / RESPONSE 共用：找不到等待者静默新建协程，不告警。
// 语义差异只在分发表那几行的注释里，函数体没有可写的区别，故不再各留一个同体空壳
static void _coro_handle_miss_create(task_dispatch_arg *arg) {
    _coro_dispatch(arg, 1, 0);
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
    // NEVERCONN 的合成 CLOSE 只为唤醒上面那批等待方，不触发 on_close 观察者（见 close_type）
    if (CLOSE_TYPE_NEVERCONN != arg->msg.erro) {
        _coro_mco_create(arg);
    }
    /* resume 期间协程可能重新在同一 sess 上注册等待（追加到 cofind->waiters），
     * 也可能因其它 sess 的插入触发 hashmap resize 导致 cofind 悬空，须重新查询而非复用旧指针 */
    cofind = (coro_sess *)hashmap_get(coctx->mapco, &key);
    if (NULL != cofind && list_empty(&cofind->waiters)) {
        _coro_cosess_delete(coctx, arg->msg.sess);
    }
}
// 定期（每 1 秒）扫描超时堆，唤醒所有已到期的挂起协程并注入超时消息
static void _coro_timeout_monitor(task_ctx *task, uint64_t sess) {
    (void)sess;
    coro_ctx *coctx = task->arg;
    uint64_t now = timer_cur_ms(&coctx->timer);
    /* 堆空即无到期条目;堆非空必有挂起协程(插堆与 ++nyield 之间没有 yield 点),不必再判 nyield */
    if (NULL != coctx->timeout_heap.root) {
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
            /* 超时路径无视 keep：keep 是为活连接上的请求-响应循环省掉建删条目的开销，超时本就罕见；
             * 留着的话，该 skid 的 CLOSE 已被消费过时条目再没有任何路径能删掉 */
            if (list_empty(&cosess->waiters)) {
                _coro_cosess_delete(coctx, te->sess);
            }
            arg.msg.sess = te->sess;
            pool_push(&coctx->te_pool, te, 0);
            _coro_mco_resume(coro, &arg);
        }
    }
    if (now - coctx->shrink_ms >= SHRINK_TIME) {
        coctx->shrink_ms = now;
        pool_shrink(&coctx->copool);
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
    coro_ctx *coctx = (coro_ctx *)arg->task->arg;
    if (coctx->nyield > 0) {
        LOG_WARN("task %s yield %d.", _NAME_OR(arg->task->name), coctx->nyield);
    }
}
static const _coro_msg_handler_t _coro_msg_handlers[MSG_TYPE_ALL] = {
    [MSG_TYPE_STARTUP]      = _coro_handle_startup,// 新建
    [MSG_TYPE_CLOSING]      = _coro_handle_closing,// 新建
    [MSG_TYPE_TIMEOUT]      = _coro_handle_timeout,// 新建或唤醒
    [MSG_TYPE_ACCEPT]       = _coro_mco_create,// 新建
    [MSG_TYPE_CONNECT]      = _coro_handle_miss_create, // 连接建立；未找到静默新建
    [MSG_TYPE_SSLEXCHANGED] = _coro_handle_miss_create, // SSL 握手；未找到静默新建
    [MSG_TYPE_HANDSHAKED]   = _coro_handle_miss_create, // 应用层握手；未找到静默新建
    [MSG_TYPE_RECV]         = _coro_handle_recved,// sess==0 或协议不允许 创建；未找到新建，否则唤醒
    [MSG_TYPE_SEND]         = _coro_mco_create,// 新建
    [MSG_TYPE_CLOSE]        = _coro_handle_closed,// sess直接赋值skid,尝试唤醒所有
    [MSG_TYPE_RECVFROM]     = _coro_handle_miss_create,// sess 0新建；未找到静默新建，不告警(UDP 不保证顺序与送达，迟到/孤儿包是常态)
    [MSG_TYPE_REQUEST]      = _coro_mco_create,// 新建
    [MSG_TYPE_RESPONSE]     = _coro_handle_miss_create,// 未找到静默新建，不告警(task_multi_request 广播的 N 个响应本就没有等待者；
                                                       // 真孤儿由请求方的 coro_request 超时告警报出)
};
// 消费 fork_pending 全部待起 fork（嵌套 fork 追加到尾，持续消费到空）；起协程走 _coro_fork_run
static void _coro_drain_forks(task_ctx *task) {
    coro_ctx *coctx = (coro_ctx *)task->arg;
    if (list_empty(&coctx->fork_pending)) {
        return;
    }
    list_node *ln;
    task_dispatch_arg farg = { 0 };
    farg.task = task;
    farg.msg.mtype = MSG_TYPE_FORK;
    while (NULL != (ln = list_pop_head(&coctx->fork_pending))) {
        farg.msg.data = UPCAST(ln, fork_item, node);
        _coro_mco_create(&farg);
    }
}
static void _coro_message_dispatch(task_dispatch_arg *arg) {
    if (arg->msg.mtype > MSG_TYPE_NONE
        && arg->msg.mtype < MSG_TYPE_ALL
        && NULL != _coro_msg_handlers[arg->msg.mtype]) {
        _coro_msg_handlers[arg->msg.mtype](arg);
    }
    _coro_drain_forks(arg->task);
}
task_ctx *coro_task_register(loader_ctx *loader, const char *name, uint32_t quecap,
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
    // 判型与 coro_dump 同口径:光判 NULL 挡不住"类型不对但非空"——TASK_LUA 的 task->arg 是
    // ltask_ctx *、带 arg 的 TASK_NORMAL 是业务自己的指针，按 coro_ctx * 解引用就是读错偏移
    if (TASK_MCO != task_get_type(task)
        || NULL == task->arg) {
        return NULL;
    }
    return ((coro_ctx *)task->arg)->arg;
}
int32_t coro_incoro(task_ctx *task) {
    // 判型同 coro_get_arg。收 NULL 是有意的:调用方常在"连接还没建起来"的清理路径上问,
    // 那时手里的 ctx->task 可能还没填
    if (NULL == task
        || TASK_MCO != task_get_type(task)
        || NULL == task->arg) {
        return 0;
    }
    return NULL != ((coro_ctx *)task->arg)->curco;
}
int32_t coro_sync(task_ctx *task, SOCKET fd, uint64_t skid) {
    return ev_ud_sess(&task->loader->netev, fd, skid, skid);
}
// 挂起当前协程并等待下一条匹配消息
// 返回指向分发参数中 msg 的指针，在下次 _coro_wait 或 _coro_mco_resume 返回前有效
message_ctx *_coro_wait(task_ctx *task, uint64_t sess, msg_type mtype, uint32_t ms) {
    coro_ctx *coctx = task->arg;
    ASSERTAB(NULL != coctx->curco, "coro api called outside a coroutine.");
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
// 等一条指定类型的消息:超时则关连接并告警,连接已关则静默,两种都返 NULL。
// 四个等待点(ssl exchange / handshake / connect / recv)只差 mtype、超时值与告警里的动作名,
// tag 仅进日志。返回的指针在本协程下次 _coro_wait 前有效。
// CLOSE 分支有意不告警:对端关连接是正常事件,而调用方是每命令一轮的循环,一条连接断掉能刷出几十条
static message_ctx *_coro_wait_msg(task_ctx *task, SOCKET fd, uint64_t skid,
                                   msg_type mtype, uint32_t ms, const char *tag) {
    // 连接已 teardown 就别挂上去:等不到唤醒,只会挂满超时再对 INVALID_SOCK 调一次 ev_close、
    // 打一条假的 timeout 日志。四个 coro_* 入口都经本函数,守卫收在这里一处
    if (INVALID_SOCK == fd) {
        return NULL;
    }
    message_ctx *msg = _coro_wait(task, skid, mtype, ms);
    if (MSG_TYPE_TIMEOUT == msg->mtype) {
        ev_close(&task->loader->netev, fd, skid);
        LOG_WARN("task %s, %s timeout, skid %"PRIu64".", _NAME_OR(task->name), tag, skid);
        return NULL;
    }
    if (MSG_TYPE_CLOSE == msg->mtype) {
        return NULL;
    }
    return msg;
}
// 等待 SSL 交换完成消息，失败的处理见 _coro_wait_msg
static int32_t _wait_ssl_exchanged(task_ctx *task, SOCKET fd, uint64_t skid) {
    return NULL == _coro_wait_msg(task, fd, skid, MSG_TYPE_SSLEXCHANGED,
                                  task_get_netread_timeout(task), "ssl exchange")
           ? ERR_FAILED : ERR_OK;
}
int32_t coro_ssl_exchange(task_ctx *task, SOCKET fd, uint64_t skid,
                          int32_t client, struct evssl_ctx *evssl) {
    if (ERR_OK != ev_ssl(&task->loader->netev, fd, skid, client, evssl)) {
        return ERR_FAILED;
    }
    return _wait_ssl_exchanged(task, fd, skid);
}
void *coro_handshaked(task_ctx *task, SOCKET fd, uint64_t skid, int32_t *err, size_t *size) {
    message_ctx *msg = _coro_wait_msg(task, fd, skid, MSG_TYPE_HANDSHAKED,
                                      task_get_netread_timeout(task), "handshake");
    if (NULL == msg) {
        *err = ERR_FAILED;
        return NULL;
    }
    *err = msg->erro;
    SET_PTR(size, msg->size);
    return msg->data;
}
int32_t coro_wait_connect(task_ctx *task, SOCKET fd, uint64_t skid, struct evssl_ctx *evssl) {
    message_ctx *msg = _coro_wait_msg(task, fd, skid, MSG_TYPE_CONNECT,
                                      task_get_connect_timeout(task), "connect");
    if (NULL == msg) {
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
void coro_close(task_ctx *task, SOCKET fd, uint64_t skid) {
    if (INVALID_SOCK == fd) {
        return;
    }
    ev_close(&task->loader->netev, fd, skid);
    _coro_wait(task, skid, MSG_TYPE_CLOSE, task_get_netread_timeout(task));
}
// 等待指定连接的下一条接收消息，失败的处理与指针有效期见 _coro_wait_msg
static message_ctx *_coro_wait_recved(task_ctx *task, SOCKET fd, uint64_t skid) {
    return _coro_wait_msg(task, fd, skid, MSG_TYPE_RECV, task_get_netread_timeout(task), "netread");
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
// 只收不发:一次请求产生多个响应时续读后续包(如 MySQL 多结果集)。
// 与 coro_send 一样,返回的指针只在本协程下次挂起前有效
void *coro_recv(task_ctx *task, SOCKET fd, uint64_t skid, size_t *size) {
    message_ctx *msg = _coro_wait_recved(task, fd, skid);
    if (NULL == msg) {
        return NULL;
    }
    SET_PTR(size, msg->size);
    return msg->data;
}
void *coro_slice(task_ctx *task, SOCKET fd, uint64_t skid, size_t *size, int32_t *end) {
    *end = 0;// 任何失败路径都不再往下写,统一在此归零,保证"返回 NULL 时 end 为 0"
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
// fork 子协程体：跑用户函数后 FREE item；属 fork_wait 的（fwctx!=NULL）递减 waited，归零同步 curco 唤醒 waiter
static void _coro_fork_run(task_ctx *task, fork_item *item) {
    coro_ctx *coctx = (coro_ctx *)task->arg;
    item->fkcb(task, item->arg);
    fork_wait_ctx *fw = item->fwctx;// 先缓存：fw 在 waiter 协程栈内，mco_destroy(waiter) 后整块释放
    pool_push(&coctx->fork_item_pool, item, 0);
    if (NULL != fw && 0 == --fw->waited) {
        // waiter 缓存到局部：resume 后 coro_fork_wait 返回，其栈上的 fw 随即失效
        mco_coro *waiter = fw->waiter;
        _coro_resume_reap(coctx, waiter);
    }
}
// 建 fork_item 追加到 fork_pending（task-local 无界无锁）；fwctx=NULL 即 coro_fork 退化态
static inline void _coro_fork_enqueue(coro_ctx *coctx, fork_serial_cb fkcb, void *arg, fork_wait_ctx *fwctx) {
    fork_item *item = (fork_item *)pool_pop(&coctx->fork_item_pool, NULL, 0);
    item->fkcb = fkcb;
    item->arg = arg;
    item->fwctx = fwctx;
    list_push_tail(&coctx->fork_pending, &item->node);
}
void coro_fork(task_ctx *task, fork_serial_cb func, void *arg) {
    coro_ctx *coctx = (coro_ctx *)task->arg;
    if (NULL == coctx->curco) {
        // 与 coro_fork_wait 一致：fork_pending 无锁，仅允许在本 task 协程上下文内调用
        LOG_WARN("task %s, coro_fork called outside coroutine context.", _NAME_OR(task->name));
        return;
    }
    _coro_fork_enqueue(coctx, func, arg, NULL);
}
int32_t coro_fork_wait(task_ctx *task, int32_t n, fork_serial_cb funcs[], void *args[]) {
    if (n <= 0) {
        return ERR_OK;
    }
    coro_ctx *coctx = (coro_ctx *)task->arg;
    if (NULL == coctx->curco) {
        // 不在协程上下文调用：mco_yield 会失败，提前拒绝
        LOG_WARN("task %s, coro_fork_wait called outside coroutine context.", _NAME_OR(task->name));
        return ERR_FAILED;
    }
    // fw 栈分配：mco_yield 期间父协程独立栈仍在，_coro_fork_run 读 fw 合法
    fork_wait_ctx fw;
    fw.waited = n;
    fw.waiter = coctx->curco;
    fw.since = timer_cur_ms(&coctx->timer);
    for (int32_t i = 0; i < n; i++) {
        _coro_fork_enqueue(coctx, funcs[i], args[i], &fw);
    }
    list_push_head(&coctx->fork_waited, &fw.node);// 入队
    ++coctx->nyield;
    mco_result rtn = mco_yield(coctx->curco);
    --coctx->nyield;
    // 并发 fork_wait 完成顺序非 LIFO，移除的可能非队头，按节点解链（勿改 pop_head）
    list_remove(&coctx->fork_waited, &fw.node);
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
    return ERR_OK;
}
coro_serial_ctx *coro_serial_new(task_ctx *task) {
    // 判型同 coro_get_arg:光判 NULL 挡不住"类型不对但非空",按 coro_ctx * 挂进
    // serials 就是往错误偏移写链表节点
    if (TASK_MCO != task_get_type(task)
        || NULL == task->arg) {
        return NULL;
    }
    coro_serial_ctx *s;
    CALLOC(s, 1, sizeof(coro_serial_ctx));
    s->task = task;
    list_push_head(&((coro_ctx *)task->arg)->serials, &s->node);
    return s;
}
void coro_serial_free(coro_serial_ctx *serial) {
    // closed 先于 drain 置位,一举两用:
    // 一是挡住排队者的错误路径再调 enter(拿到 ERR_FAILED,不会又排进已在清空的队列);
    // 二是挡住它们重入本函数——看到 closed 直接返回,FREE 由外层这次完成,不会双重释放
    if (0 != serial->closed) {
        return;
    }
    serial->closed = 1;
    // 逐个唤醒排队者并标记 aborted：它们醒来即失败返回,锁不交接给任何一个
    coro_ctx *coctx = (coro_ctx *)serial->task->arg;
    list_node *ln;
    serial_node *nd;
    mco_coro *wco;
    while (NULL != (ln = list_pop_head(&serial->waiters))) {
        nd = UPCAST(ln, serial_node, node);
        nd->aborted = 1;
        wco = nd->co;
        _coro_resume_reap(coctx, wco);
        pool_push(&coctx->serial_node_pool, nd, 0);
    }
    // 排队者能唤醒,持锁者不能——锁抢不走,它还在临界区里跑,后面还要 leave。
    // 所以有持锁者时本函数不释放,只把 closed 留在那儿当交接凭据:
    // 队列已排空且 closed 之后再没人能入队,那位最后一次 leave 走的必是"队列空"分支,由它关灯
    if (NULL == serial->current) {
        list_remove(&coctx->serials, &serial->node);
        FREE(serial);
    }
}
int32_t coro_serial_enter(coro_serial_ctx *serial) {
    coro_ctx *coctx = (coro_ctx *)serial->task->arg;
    if (NULL == coctx->curco) {
        // 非协程上下文
        LOG_WARN("task %s, coro_serial_enter called outside coroutine context.", _NAME_OR(serial->task->name));
        return ERR_FAILED;
    }
    if (0 != serial->closed) {
        // 已关闭：连锁都不再发放，免得关闭流程等一个永远不会归还的持有者
        return ERR_FAILED;
    }
    // 缓存 self 到局部变量：mco_yield 期间 coctx->curco 被 coro_serial_leave
    // 改写为下一个被唤醒的协程；本协程被再次唤醒时 coctx->curco 会被还原指回 self,
    mco_coro *self = coctx->curco;
    if (NULL != serial->current && serial->current != self) {
        // ── 跨协程路径：锁被其他协程持有，需排队等待 ─────────────────────
        // 本路径不自行赋 current/ref：唤醒方 coro_serial_leave 已代劳；nd 也由唤醒方在
        // mco_resume 返回后归还池，临界区内它只被唤醒方的栈局部变量持有
        serial_node *nd = (serial_node *)pool_pop(&coctx->serial_node_pool, NULL, 0);
        nd->co = self;
        nd->aborted = 0;
        nd->since = timer_cur_ms(&coctx->timer);
        list_push_tail(&serial->waiters, &nd->node);
        // 排队中的协程也算"挂起没退",与 _coro_wait / coro_fork_wait 同口径计入 nyield,
        // 否则 task 关闭时看到 nyield==0 就静默通过,拿不到"有协程卡在临界区队列上"这条线索
        ++coctx->nyield;
        mco_result rtn = mco_yield(self);
        --coctx->nyield;
        ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
        // 醒来有两种来源,必须靠自己那个节点上的 aborted 区分,不能读 serial->closed——
        // 正常交接与"交接后别人才关闭"这两种情形下 closed 都可能为 1,
        // 那时锁其实已经在自己手上,误判成失败返回就把锁永久漏掉了
        if (0 != nd->aborted) {
            return ERR_FAILED;
        }
        // 唤醒后状态：serial->current==self, serial->ref==1；nd 尚未归还池(见上)
    } else {
        // ── 无锁或同协程嵌套路径 ─────────────────────────────────────
        // current==NULL：占锁,ref 0→1；current==self：同协程嵌套(如 cs 内再 cs),仅 ref++
        // 不死锁,出口由 coro_serial_leave 的 ref 计数管理
        if (NULL == serial->current) {
            serial->current = self;
        }
        serial->ref++;
    }
    return ERR_OK;
}
void coro_serial_leave(coro_serial_ctx *serial) {
    serial->ref--;
    // 负数说明有人没持锁就调了 leave。放着不管的话 ref 再也回不到 0,
    // 这个 serial 既不会交接也不会释放,是一条不报错的死路
    ASSERTAB(serial->ref >= 0, "coro_serial_leave without a matching enter");
    if (0 != serial->ref) {// 嵌套层，current 留给外层
        return;
    }
    list_node *ln = list_pop_head(&serial->waiters);
    if (NULL == ln) {
        serial->current = NULL;
        // coro_serial_free 撞上本协程持锁,把释放推给了这里(见该函数末尾)
        if (0 != serial->closed) {
            list_remove(&((coro_ctx *)serial->task->arg)->serials, &serial->node);
            FREE(serial);
        }
        return;
    }
    serial_node *nxt = UPCAST(ln, serial_node, node);
    // 唤醒前先设置 current/ref，nxt 唤醒后读取看到一致状态。
    // 这里就地 mco_resume 是安全的：minicoro 切栈，一串不 yield 的等待者链式唤醒是 N 个协程
    // 各挂一帧在各自栈上，OS 线程栈不增长
    mco_coro *wco = nxt->co;
    serial->current = wco;
    serial->ref = 1;
    coro_ctx *coctx = (coro_ctx *)serial->task->arg;
    // curco 的还原目标恒为调用方自己那个协程,不必外传
    // 下面这行之后不许再碰 serial:锁已交给 wco,它在自己的临界区里可以 coro_serial_free
    // （标记后由它那次 leave 释放）,回到这里时对象可能已经没了。此后只用 coctx / nxt / wco
    _coro_resume_reap(coctx, wco);
    pool_push(&coctx->serial_node_pool, nxt, 0);
}
int32_t coro_serial_call(coro_serial_ctx *serial, fork_serial_cb func, void *arg) {
    if (ERR_OK != coro_serial_enter(serial)) {
        return ERR_FAILED;
    }
    // 临界区主体：func 内任意 yield（coro_sleep / coro_send / coro_request 等）期间，
    // 锁仍由 self 持有（serial->current 不变），其他协程进 cs 走"跨协程路径"挂起。
    // C 无 xpcall：func 内 abort/segfault 直接终止进程，本函数不兜底（与 coro_fork 同约定）
    if (NULL != func) {
        func(serial->task, arg);
    }
    coro_serial_leave(serial);
    return ERR_OK;
}
// 把一条挂起协程信息追加到 binary；C 协程无栈回溯,仅 sess / mtype / 挂起时长
static void _coro_dump_one(binary_ctx *bw, uint64_t sess, const coro_info *ci, uint64_t now) {
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
    int32_t nfork = 0;
    fork_wait_ctx *fw;
    list_foreach(&coctx->fork_waited, fit) {
        fw = UPCAST(fit, fork_wait_ctx, node);
        binary_set_va(&bw, "fork_wait pending=%d age=%" PRIu64 "ms\n", fw->waited, now - fw->since);
        nfork++;
    }
    int32_t nserial = 0;
    coro_serial_ctx *sl;
    serial_node *nd;
    list_foreach(&coctx->serials, sit) {
        sl = UPCAST(sit, coro_serial_ctx, node);
        list_foreach(&sl->waiters, wit) {
            nd = UPCAST(wit, serial_node, node);
            binary_set_va(&bw, "serial=%p held=%d age=%" PRIu64 "ms\n",
                (void *)sl, NULL != sl->current, now - nd->since);
            nserial++;
        }
    }
    // sessions 是 mapco 的条目数，与 suspended（挂起协程数）不是一回事：keep 的条目摘空 waiters
    // 后仍留着复用。sessions 只增不减、suspended 长期为 0，就是 keep 条目泄漏
    binary_set_va(&bw, "%d suspended, %d sessions, %d fork_wait, %d serial, %d yield total.",
                  total, (int32_t)hashmap_count(coctx->mapco), nfork, nserial, coctx->nyield);
    SET_PTR(size, bw.offset);
    return bw.data;
}
