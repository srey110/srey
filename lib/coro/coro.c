#include "coro/coro.h"
#include "containers/hashmap.h"
#include "containers/heap.h"
#include "containers/slist.h"
#include "utils/utils.h"
#include "utils/timer.h"
#include "utils/binary.h"
#include "utils/pool.h"
#if defined(MCO_USE_VMEM_ALLOCATOR)
    #error "MCO_USE_VMEM_ALLOCATOR skips the C-allocator block, silently reverting coroutine stacks to mmap/VirtualAlloc"
#endif
// 协程栈改走框架分配器以计入 MEMORY_CHECK，不必清零。
// Win32 走 MCO_USE_FIBERS，栈由 CreateFiberEx 给，同样不计入——有意保留，上面那道 #error 只挡 VMEM
#define MCO_ALLOC(size) _malloc(size)
#define MCO_DEALLOC(ptr, size) _free(ptr)
// 全项目只推/弹一个 8 字节指针；这个值设多大，每个协程就白占多大（谁都不清零它）
#define MCO_DEFAULT_STORAGE_SIZE 16
// minicoro 只认 NDEBUG 判调试期，而 mk.sh 从不定义它，得手动关掉它自带的 assert/puts。
// 断言映射到恒开的 ASSERTAB：切换路径上的双 resume / 陈旧 curco 探针不能没有。
// MCO_LOG 维持静默：它每处都紧跟一个 return，调用方的 ASSERTAB 拿 mco_result_description
// 打的是同一句话，而栈溢出那两处已经没有栈够它跑
#define MCO_NO_DEBUG
#define MCO_ASSERT(c) ASSERTAB(c, #c)
#define MINICORO_IMPL
#include "coro/minicoro.h"

#define COROPOOL_CAP 128
// 协程栈上下界。ASan 插桩把每帧撑大数倍，56KB 必爆栈，故两头一起抬；非 ASan 的下界取 minicoro
// 默认值（它只把不足 32KB 的静默抬到 32KB，比默认还小，调小反更易爆栈）。
// 上界按每个调度器最多 COROPOOL_CAP 个协程折算，再大单个调度器的常驻栈就上百 MB。
// 两者必须留出差距：相等会让栈大小配置在该构建上彻底失效，怎么配都是同一个值
#if ENABLED_ASAN
    #define COROSTACK_MIN (1024 * 1024)
    #define COROSTACK_MAX (16 * 1024 * 1024)
#else
    #define COROSTACK_MIN MCO_DEFAULT_STACK_SIZE
    #define COROSTACK_MAX (1024 * 1024)
#endif
#define NODEPOOL_CAP ONEK
#define MAPCO_INIT_CAP 32 // mapco 建表容量,同时是缩容地板,见 coro_new
#define COROPOOL_MIN_KEEP 4

// 单个挂起协程的等待信息。超时堆节点直接嵌在这里:一个等待者一个对象,
// 到期时由堆顶直接还原出它,不必再拿堆节点去 waiters 里线性找
typedef struct coro_info {
    int32_t timed;     // 非 0 表示已挂在 co->timeout_heap 上
    uint32_t hidx;     // 在 co->timeout_heap 中的下标，由堆维护；timed 为 0 时无意义
    int32_t tag;       // 期望的唤醒标签
    list_node node;    // 挂载到 coro_sess.waiters
    mco_coro *co;      // 挂起的协程对象
    uint64_t since;    // 挂起起始时刻（毫秒），用于 debug dump 计算挂起时长
    uint64_t timeout;  // 到期时间戳（毫秒），超时堆按它排序
    uint64_t sess;     // 反查 mapco 用：coro_map 按值存 coro_sess 且 resize 会搬，不能存其指针
}coro_info;
// 超时堆：按 timeout 排序的最小堆，堆顶即最早到期的等待者
#define _CORO_TIMEOUT_LT(lhs, rhs) ((lhs)->timeout < (rhs)->timeout)
HEAP_DECL(coro_heap, coro_info, hidx, _CORO_TIMEOUT_LT)
// session 到挂起协程的映射节点
typedef struct coro_sess {
    int32_t keep;       // waiters 摘空后是否保留本条目：0 立即删除 mapco 条目；1 保留（同一 sess 高频复用，免去反复 coro_map 删除+插入），仅 coro_wake_all 会强制清零并真正删除
    uint64_t sess;      // session ID
    list_ctx waiters;   // 挂起协程链表（元素 coro_info，严格按 FIFO 顺序等待/唤醒：仅队头 tag 匹配才摘除）
}coro_sess;
// fork 任务载荷：coro_fork / coro_fork_wait 均建此结构追加到 co->fork_pending，
// drain 时由 _coro_fork_run 跑 fn 后归还 fork_item_pool；fwctx=NULL 即 coro_fork(fire-and-forget) 退化态
typedef struct fork_item {
    list_node node;               // 挂 co->fork_pending
    coro_fn fn;                   // 用户函数
    void *arg;                    // 用户参数（生命周期由调用方管理）
    struct fork_wait_ctx *fwctx;  // NULL=coro_fork；非NULL=coro_fork_wait 成员，归零唤醒其 waiter
} fork_item;
// fork_wait 屏障：栈分配于 coro_fork_wait 内，子协程跑完 stub 递减 pending；
// 归零时唤醒 waiter（栈生命周期到 coro_fork_wait return 才结束，覆盖 yield 期间）；
// yield 期间挂入 co->fork_waited 链表，醒来后由 coro_fork_wait 自己摘除
typedef struct fork_wait_ctx {
    list_node node;             // co->fork_waited 侵入式链表节点（slist，UPCAST 复原外层）
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
    list_node node;        // 挂 co->serials（侵入式，须在首位）；仅供 coro_dump 遍历，
                           // 摘挂点严格对齐三处生死：coro_serial_new 挂、两处 FREE(serial) 前摘
    int32_t ref;           // 嵌套深度（同协程多次进入累加）
    int32_t closed;        // 1 = 已关闭，此后任何 enter 一律失败，不再有人能拿到锁
    uint64_t since;        // current 转为非 NULL 的时刻(ms)，coro_dump 据此报"持锁多久"；
                           // current 为 NULL 时是上一任的残值，不可读
    coro_ctx *co;          // 所属调度器；resume 时同步 co->curco 需要
    mco_coro *current;     // 当前持锁协程；NULL 表示无锁
    list_ctx waiters;      // 挂起 waiter 的 FIFO（元素 serial_node，UPCAST 复原）
};
// mapco 的哈希单点:表里的 bucket->hash 由它产生,三处 *_with_hash 的调用点也必须用它算。
// 调用点不得绕过它直接调 hash_u64,否则写进去的与查的对不上,条目静默查不到(不崩、不报错)
#define _CORO_SESS_HASH_OF(sess) hash_u64(sess)
// 按 sess 散列与升序比较
#define _CORO_SESS_HASH(e) _CORO_SESS_HASH_OF((e)->sess)
#define _CORO_SESS_CMP(a, b) (((a)->sess < (b)->sess) ? -1 : ((a)->sess > (b)->sess) ? 1 : 0)
HASHMAP_DECL(coro_map, coro_sess, _CORO_SESS_HASH, _CORO_SESS_CMP)
// 调度器
struct coro_ctx {
    int32_t nyield;              // 当前挂起（yield）中的协程数量
    mco_coro *curco;             // 正在运行的协程指针
    coro_map *mapco;             // sess → coro_sess 哈希映射
    void *owner;                 // 用本调度器的一方，原样交给钩子与 coro_fn
    uint64_t shrink_ms;          // 上次协程池收缩的时间戳(ms)，按 SHRINK_TIME 门控
    coro_hooks hooks;            // owner 钩子；before_run / after_run 没给时补成空函数，热路径不必判空
    list_ctx fork_pending;       // 待起协程的 fork_item FIFO（slist，无界无锁）；由 coro_fork_drain 跑到空
    list_ctx fork_waited;        // 挂起的 fork_wait 父协程链表（slist，元素 fork_wait_ctx）；节点是父协程栈上的对象，醒来即摘
    list_ctx serials;            // 活跃的命令串行化执行器链表（slist，元素 coro_serial_ctx）；供 coro_dump 遍历，
                                 // 正常由使用方释放，coro_free 兜底
    pool_ctx copool;             // 空闲协程对象池（元素 mco_coro *，含负载趋势）
    pool_ctx coinfo_pool;        // 空闲 coro_info 节点池，容量 NODEPOOL_CAP，不参与周期性收缩
    pool_ctx fork_item_pool;     // 空闲 fork_item 节点池，容量 NODEPOOL_CAP，不参与周期性收缩
    pool_ctx serial_node_pool;   // 空闲 serial_node 节点池，容量 NODEPOOL_CAP，不参与周期性收缩
    timer_ctx timer;             // 用于获取当前毫秒时间戳
    coro_heap timeout_heap;      // 按到期时间排序的最小堆,O(1) 检查最早超时
    mco_desc desc;               // 本调度器的协程描述：入口、栈大小；user_data 指回本调度器
};
// 交给新协程的一条活，放在调用方栈上：fork 非 NULL 跑 fork，否则跑 hooks.run(owner, payload)。
// 协程只拿到它的指针，开跑第一件事就是整份拷走——调用方栈在协程第一次挂起后即失效
typedef struct coro_entry {
    fork_item *fork;
    void *payload;
}coro_entry;

static inline void _coro_fork_run(coro_ctx *co, fork_item *item);
// before_run / after_run 没给时的占位
static void _coro_hook_nop(void *owner) {
    (void)owner;
}
// 将挂起的协程注册到 mapco；keep 只在新建条目时生效
static inline void _coro_cosess_set(coro_ctx *co, mco_coro *coro, uint64_t sess, int32_t tag, int32_t keep, uint32_t ms) {
    uint64_t now = timer_cur_ms(&co->timer);
    coro_info *coinfo = (coro_info *)pool_pop(&co->coinfo_pool, NULL, 0);
    coinfo->since = now;
    coinfo->co = coro;
    coinfo->tag = tag;
    coinfo->sess = sess;
    coinfo->timed = (ms > 0);
    if (0 != coinfo->timed) {
        coinfo->timeout = now + ms;
        coro_heap_insert(&co->timeout_heap, coinfo);
    }
    // 走 get_set:一趟探测覆盖"已有就追加、没有就新建"两种情况。keep 只在新建时生效,
    // 命中已有条目时返回的是表内那一份,这里填的 keep 不会覆盖它
    coro_sess cosess;
    cosess.sess = sess;
    cosess.keep = keep;
    list_init(&cosess.waiters);
    coro_sess *cur = coro_map_get_set(co->mapco, &cosess, NULL);
    list_push_tail(&cur->waiters, &coinfo->node);
}
// 从 mapco 中删除指定 sess 的记录。hash 由调用方用 _CORO_SESS_HASH_OF 算好传进来:
// 三个调用点都是刚 get 过同一个 sess,不必再算一遍
static inline void _coro_cosess_delete(coro_ctx *co, uint64_t sess, uint64_t hash) {
    coro_sess key;
    key.sess = sess;
    coro_map_delete_with_hash(co->mapco, &key, hash);
}
// 从 mapco 查找匹配 sess 的挂起协程节点，仅检测队头：tag 匹配才摘除返回，
// 队头不匹配（含 keep 保留的空条目）视为无等待者，不越过队头继续查找（保持严格 FIFO）；
// 摘除后链表为空且 !keep 时才删除 mapco 条目
static inline coro_info *_coro_cosess_get(coro_ctx *co, uint64_t sess, int32_t tag) {
    coro_sess key;
    key.sess = sess;
    uint64_t hash = _CORO_SESS_HASH_OF(sess);
    coro_sess *cofind = coro_map_get_with_hash(co->mapco, &key, hash);
    if (NULL == cofind || list_empty(&cofind->waiters)) {
        return NULL;
    }
    coro_info *coinfo = UPCAST(cofind->waiters.head, coro_info, node);
    if (tag != coinfo->tag) {
        return NULL;
    }
    list_remove(&cofind->waiters, &coinfo->node);
    if (list_empty(&cofind->waiters) && !cofind->keep) {
        _coro_cosess_delete(co, sess, hash);
    }
    return coinfo;
}
// 从 coinfo 取出协程对象，清理其超时堆节点（如果有），并归还 coinfo 节点到对象池
static inline mco_coro *_coro_take_mco(coro_ctx *co, coro_info *coinfo) {
    mco_coro *c = coinfo->co;
    if (0 != coinfo->timed) {
        coro_heap_remove(&co->timeout_heap, coinfo);
        coinfo->timed = 0;
    }
    pool_push(&co->coinfo_pool, coinfo, 0);
    return c;
}
// 协程主循环：每次被 resume 弹出一条活，跑完先回池再 after_run。
// after_run 里不得让本调度器被释放：本协程返回后，唤醒方还要还原 curco、回收死协程
static void _coro_mco_cb(mco_coro *coro) {
    coro_ctx *co = (coro_ctx *)mco_get_user_data(coro);// 一个协程只属于一个调度器，循环外取一次
    mco_result rtn;
    coro_entry *ep;
    coro_entry e;
    for (;;) {
        rtn = mco_yield(coro);
        ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
        rtn = mco_pop(coro, &ep, sizeof(ep));
        ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
        e = *ep;
        co->hooks.before_run(co->owner);
        if (NULL != e.fork) {
            _coro_fork_run(co, e.fork);
        } else {
            co->hooks.run(co->owner, e.payload);
        }
        if (ERR_OK != pool_push(&co->copool, coro, POOL_OP_NOFREE)) {
            co->hooks.after_run(co->owner);
            break; // 池满时跳出循环，让函数自然返回使协程进入 MCO_DEAD 状态
        }
        co->hooks.after_run(co->owner);
    }
}
void coro_thread_cleanup(void) {
    mco_thread_cleanup();
}
size_t coro_stack_fit(size_t stack_size) {
    if (stack_size < COROSTACK_MIN) {
        return COROSTACK_MIN;
    }
    if (stack_size > COROSTACK_MAX) {
        return COROSTACK_MAX;
    }
    return stack_size;
}
// 对象池 _elnew：新建协程并首次 resume 到第一个 yield 点；args 是 pool_pop 传进来的调度器
static void *_coro_new(void *args) {
    mco_coro *c;
    mco_result rtn = mco_create(&c, &((coro_ctx *)args)->desc);
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
    rtn = mco_resume(c);
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
    return c;
}
// 对象池 _elfree：销毁协程对象
static void _coro_free(void *c) {
    mco_result rtn = mco_destroy((mco_coro *)c);
    if (MCO_SUCCESS != rtn) {
        LOG_WARN("%s", mco_result_description(rtn));
    }
}
coro_ctx *coro_new(const coro_hooks *hooks, void *owner, size_t stack_size) {
    ASSERTAB(NULL != hooks && NULL != hooks->run, ERRSTR_NULLP);
    pool_cbs cbs = { _coro_new, _coro_free, NULL, NULL };
    coro_ctx *co;
    CALLOC(co, 1, sizeof(coro_ctx));
    co->owner = owner;
    co->hooks = *hooks;
    if (NULL == co->hooks.before_run) {
        co->hooks.before_run = _coro_hook_nop;
    }
    if (NULL == co->hooks.after_run) {
        co->hooks.after_run = _coro_hook_nop;
    }
    co->desc = mco_desc_init(_coro_mco_cb, coro_stack_fit(stack_size));
    co->desc.user_data = co;// 协程经 mco_get_user_data 找回所属调度器
    pool_init(&co->copool, 0, COROPOOL_CAP, COROPOOL_MIN_KEEP, 0, &cbs);
    pool_init(&co->coinfo_pool, sizeof(coro_info), NODEPOOL_CAP, 0, 0, NULL);
    pool_init(&co->fork_item_pool, sizeof(fork_item), NODEPOOL_CAP, 0, 0, NULL);
    pool_init(&co->serial_node_pool, sizeof(serial_node), NODEPOOL_CAP, 0, 0, NULL);
    timer_init(&co->timer);
    co->shrink_ms = timer_cur_ms(&co->timer);
    // 建表容量取小:cap 同时是缩容地板,给大了永不回缩
    co->mapco = coro_map_new(MAPCO_INIT_CAP, NULL);
    coro_heap_init(&co->timeout_heap, 0);
    return co;
}
// 正常用法下 mapco 的 waiters(连同超时堆)与 fork_waited 到这里恒为空:挂起的协程持着 owner 引用,进不来本函数,
// 不空就是用法错,只打 WARN 不兜底;mapco 本身可能还留着 keep 空条目,无持有物,交给 coro_map_free 收。
// serials 不同——coro_serial_ctx 不持引用,下面那圈兜底 FREE 是承重的
void coro_free(coro_ctx *co) {
    if (co->nyield > 0) {
        LOG_WARN("coro_free with %d suspended coroutines, they leak.", co->nyield);
    }
    pool_free(&co->copool);
    pool_free(&co->coinfo_pool);
    pool_free(&co->fork_item_pool);
    pool_free(&co->serial_node_pool);
    coro_heap_free(&co->timeout_heap);
    coro_map_free(co->mapco);
    // fork_pending 正常路径每次分发末尾已 drain 空，此处兜底清未起的 item（不跑 fn）
    fork_item *fi;
    list_foreach_safe(&co->fork_pending, fln, ftmp) {
        fi = UPCAST(fln, fork_item, node);
        FREE(fi);
    }
    coro_serial_ctx *serial;
    list_foreach_safe(&co->serials, sln, stmp) {
        serial = UPCAST(sln, coro_serial_ctx, node);
        FREE(serial);
    }
    FREE(co);
}
void *coro_owner(coro_ctx *co) {
    return co->owner;
}
int32_t coro_running(coro_ctx *co) {
    return NULL != co->curco;
}
int32_t coro_suspended(coro_ctx *co) {
    return co->nyield;
}
// 切到另一个协程跑,回来再把 curco 指回调用者。curco 是"当前在跑的协程"这一唯一标识:
// 被唤醒者醒来后靠它进 cosess、调 mco_yield,调用者拿回控制权后同样靠它。
// 漏还原会把 curco 留在已挂起(甚至已随池收缩销毁)的协程上——调用者下一次挂起
// 对着它 mco_yield 撞 MCO_STACK_OVERFLOW abort(minicoro 先判栈范围后判状态,别照字面调栈大小);
// 顶层漏清则 coro_fork / coro_fork_wait / coro_serial_enter 三处"不在协程里就拒绝"的守卫从第一条消息起永不成立。
// 顶层调用的"原值"就是 NULL,同样由本函数还原,不必各写一份
static inline mco_result _coro_resume_switch(coro_ctx *co, mco_coro *c) {
    mco_coro *self = co->curco;
    co->curco = c;
    mco_result rtn = mco_resume(c);
    co->curco = self;
    return rtn;
}
// 唤醒尾部:切过去、断言、回收死协程。所有唤醒点共用,别再各写一份——
// curco 漏还原是上面那个 abort,MCO_DEAD 漏回收是无声的协程栈泄漏(只在池满时发生)。
// c 必须是调用方先缓存好的指针:被唤醒者返回后它栈上的对象即失效,不能再从那些对象里取 c
static inline void _coro_resume_reap(coro_ctx *co, mco_coro *c) {
    mco_result rtn = _coro_resume_switch(co, c);
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
    if (MCO_DEAD == mco_status(c)) {
        _coro_free(c);// 池满导致 _coro_mco_cb 返回,协程已死亡,须在此释放
    }
}
// 取一个协程，压入条目指针(8 字节，由 _coro_mco_cb 弹出后自行拷贝)，跑到它第一次挂起或结束
static inline void _coro_start(coro_ctx *co, coro_entry *e) {
    mco_coro *c = (mco_coro *)pool_pop(&co->copool, co, 0);
    mco_result rtn = mco_push(c, &e, sizeof(e));
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
    _coro_resume_reap(co, c);
}
// 唤醒已挂起的协程：推入唤醒数据指针后 resume
static inline void _coro_mco_resume(coro_ctx *co, mco_coro *c, void *payload) {
    mco_result rtn = mco_push(c, &payload, sizeof(payload));
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
    _coro_resume_reap(co, c);
}
void coro_spawn(coro_ctx *co, void *payload) {
    coro_entry e = { NULL, payload };
    _coro_start(co, &e);
}
void *coro_wait(coro_ctx *co, uint64_t sess, int32_t tag, int32_t keep, uint32_t ms) {
    ASSERTAB(NULL != co->curco, "coro api called outside a coroutine.");
    _coro_cosess_set(co, co->curco, sess, tag, keep, ms);
    ++co->nyield;
    mco_result rtn = mco_yield(co->curco);
    --co->nyield;
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
    // 弹出唤醒方推入的 8 字节指针
    void *payload;
    rtn = mco_pop(co->curco, &payload, sizeof(payload));
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
    return payload;
}
int32_t coro_wake(coro_ctx *co, uint64_t sess, int32_t tag, void *payload) {
    coro_info *coinfo = _coro_cosess_get(co, sess, tag);
    if (NULL == coinfo) {
        return ERR_FAILED;
    }
    _coro_mco_resume(co, _coro_take_mco(co, coinfo), payload);
    return ERR_OK;
}
// 进入时探测一次 mapco，把该 sess 下全部挂起等待者转移到本地链表再消费：
// resume 期间在同一 sess 上重新注册的等待不会被本轮看到，而是追加回同一个条目
void coro_wake_all(coro_ctx *co, uint64_t sess, void *payload, void *spawn) {
    coro_sess key;
    key.sess = sess;
    uint64_t hash = _CORO_SESS_HASH_OF(sess);
    coro_sess *cofind = coro_map_get_with_hash(co->mapco, &key, hash);
    if (NULL != cofind) {
        cofind->keep = 0;// 让此后注册的等待者摘空时能删掉条目
        list_ctx local = cofind->waiters;
        list_init(&cofind->waiters);
        list_node *node;
        coro_info *coinfo;
        while (NULL != (node = list_pop_head(&local))) {
            coinfo = UPCAST(node, coro_info, node);
            _coro_mco_resume(co, _coro_take_mco(co, coinfo), payload);
        }
    }
    if (NULL != spawn) {
        coro_spawn(co, spawn);
    }
    /* resume 期间协程可能重新在同一 sess 上注册等待（追加到 cofind->waiters），
     * 也可能因其它 sess 的插入触发 coro_map resize 导致 cofind 悬空，须重新查询而非复用旧指针 */
    cofind = coro_map_get_with_hash(co->mapco, &key, hash);
    if (NULL != cofind && list_empty(&cofind->waiters)) {
        _coro_cosess_delete(co, sess, hash);
    }
}
// 超时绝大多数等不到触发就被正常唤醒摘走，故放调度器私有的堆：插删不用锁，由 owner 定时来扫
void coro_expire(coro_ctx *co, coro_timeout_cb mk, void *ud) {
    uint64_t now = timer_cur_ms(&co->timer);
    coro_sess key, *cosess;
    uint64_t hash, tsess;
    int32_t tag;
    mco_coro *c;
    coro_info *coinfo;
    while (NULL != (coinfo = coro_heap_min(&co->timeout_heap))) {
        if (coinfo->timeout > now) {
            break; /* 堆顶是最早到期的：它都没到期，后面全没到期 */
        }
        /* 堆节点就嵌在 coinfo 里，堆顶直接还原出到期的那个等待者，
           不必再拿堆节点去 waiters 里按指针线性找 */
        coro_heap_dequeue(&co->timeout_heap);
        coinfo->timed = 0;
        tsess = coinfo->sess;
        tag = coinfo->tag;
        key.sess = tsess;
        hash = _CORO_SESS_HASH_OF(tsess);
        cosess = coro_map_get_with_hash(co->mapco, &key, hash);
        ASSERTAB(NULL != cosess, "timed waiter without session");
        list_remove(&cosess->waiters, &coinfo->node);
        c = coinfo->co;
        pool_push(&co->coinfo_pool, coinfo, 0);
        /* 超时路径无视 keep：keep 是为活连接上的请求-响应循环省掉建删条目的开销，超时本就罕见；
         * 留着的话，该 sess 的广播已被消费过时条目再没有任何路径能删掉 */
        if (list_empty(&cosess->waiters)) {
            _coro_cosess_delete(co, tsess, hash);
        }
        _coro_mco_resume(co, c, NULL == mk ? NULL : mk(ud, tsess, tag));
    }
    if (pool_shrink_due(&co->shrink_ms, now)) {
        pool_shrink(&co->copool);
    }
}
// fork 子协程体：跑用户函数后归还 item；属 fork_wait 的（fwctx!=NULL）递减 waited，归零唤醒 waiter
static inline void _coro_fork_run(coro_ctx *co, fork_item *item) {
    item->fn(co->owner, item->arg);
    fork_wait_ctx *fw = item->fwctx;// 先缓存：fw 在 waiter 协程栈内，waiter 醒来返回后即失效
    pool_push(&co->fork_item_pool, item, 0);
    if (NULL != fw && 0 == --fw->waited) {
        // waiter 缓存到局部：resume 后 coro_fork_wait 返回，其栈上的 fw 随即失效
        mco_coro *waiter = fw->waiter;
        _coro_resume_reap(co, waiter);
    }
}
// 建 fork_item 追加到 fork_pending（无界无锁）；fwctx=NULL 即 coro_fork 退化态
static inline void _coro_fork_enqueue(coro_ctx *co, coro_fn fn, void *arg, fork_wait_ctx *fwctx) {
    fork_item *item = (fork_item *)pool_pop(&co->fork_item_pool, NULL, 0);
    item->fn = fn;
    item->arg = arg;
    item->fwctx = fwctx;
    list_push_tail(&co->fork_pending, &item->node);
}
void coro_fork(coro_ctx *co, coro_fn fn, void *arg) {
    if (NULL == co->curco) {
        // 与 coro_fork_wait 一致：fork_pending 无锁，仅允许在本调度器的协程内调用
        LOG_WARN("coro_fork called outside coroutine context.");
        return;
    }
    _coro_fork_enqueue(co, fn, arg, NULL);
}
int32_t coro_fork_wait(coro_ctx *co, coro_fn fns[], void *args[], int32_t n) {
    if (n <= 0) {
        return ERR_OK;
    }
    if (NULL == co->curco) {
        // 不在协程上下文调用：mco_yield 会失败，提前拒绝
        LOG_WARN("coro_fork_wait called outside coroutine context.");
        return ERR_FAILED;
    }
    // fw 栈分配：mco_yield 期间父协程独立栈仍在，_coro_fork_run 读 fw 合法
    fork_wait_ctx fw;
    fw.waited = n;
    fw.waiter = co->curco;
    fw.since = timer_cur_ms(&co->timer);
    for (int32_t i = 0; i < n; i++) {
        _coro_fork_enqueue(co, fns[i], args[i], &fw);
    }
    list_push_head(&co->fork_waited, &fw.node);// 入队
    ++co->nyield;
    mco_result rtn = mco_yield(co->curco);
    --co->nyield;
    // 并发 fork_wait 完成顺序非 LIFO，移除的可能非队头，按节点解链（勿改 pop_head）
    list_remove(&co->fork_waited, &fw.node);
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
    return ERR_OK;
}
// e 复用安全：每个协程开跑就把条目拷走了
void coro_fork_drain(coro_ctx *co) {
    list_node *ln;
    coro_entry e = { NULL, NULL };
    while (NULL != (ln = list_pop_head(&co->fork_pending))) {
        e.fork = UPCAST(ln, fork_item, node);
        _coro_start(co, &e);
    }
}
coro_serial_ctx *coro_serial_new(coro_ctx *co) {
    coro_serial_ctx *s;
    CALLOC(s, 1, sizeof(coro_serial_ctx));
    s->co = co;
    list_push_head(&co->serials, &s->node);
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
    coro_ctx *co = serial->co;
    list_node *ln;
    serial_node *nd;
    mco_coro *wco;
    while (NULL != (ln = list_pop_head(&serial->waiters))) {
        nd = UPCAST(ln, serial_node, node);
        nd->aborted = 1;
        wco = nd->co;
        _coro_resume_reap(co, wco);
        pool_push(&co->serial_node_pool, nd, 0);
    }
    // 排队者能唤醒,持锁者不能——锁抢不走,它还在临界区里跑,后面还要 leave。
    // 所以有持锁者时本函数不释放,只把 closed 留在那儿当交接凭据:
    // 队列已排空且 closed 之后再没人能入队,那位最后一次 leave 走的必是"队列空"分支,由它关灯
    if (NULL == serial->current) {
        list_remove(&co->serials, &serial->node);
        FREE(serial);
    }
}
int32_t coro_serial_enter(coro_serial_ctx *serial) {
    coro_ctx *co = serial->co;
    if (NULL == co->curco) {
        // 非协程上下文
        LOG_WARN("coro_serial_enter called outside coroutine context.");
        return ERR_FAILED;
    }
    if (0 != serial->closed) {
        // 已关闭：连锁都不再发放，免得关闭流程等一个永远不会归还的持有者
        return ERR_FAILED;
    }
    // 缓存 self 到局部变量：mco_yield 期间 co->curco 被 coro_serial_leave
    // 改写为下一个被唤醒的协程；本协程被再次唤醒时 co->curco 会被还原指回 self,
    mco_coro *self = co->curco;
    if (NULL != serial->current && serial->current != self) {
        // ── 跨协程路径：锁被其他协程持有，需排队等待 ─────────────────────
        // 本路径不自行赋 current/ref：唤醒方 coro_serial_leave 已代劳；nd 也由唤醒方在
        // mco_resume 返回后归还池，临界区内它只被唤醒方的栈局部变量持有
        serial_node *nd = (serial_node *)pool_pop(&co->serial_node_pool, NULL, 0);
        nd->co = self;
        nd->aborted = 0;
        nd->since = timer_cur_ms(&co->timer);
        list_push_tail(&serial->waiters, &nd->node);
        // 排队中的协程也算"挂起没退",与 coro_wait / coro_fork_wait 同口径计入 nyield,
        // 否则 owner 关闭时看到挂起数为 0 就静默通过,拿不到"有协程卡在临界区队列上"这条线索
        ++co->nyield;
        mco_result rtn = mco_yield(self);
        --co->nyield;
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
            serial->since = timer_cur_ms(&co->timer);
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
    coro_ctx *co = serial->co;
    list_node *ln = list_pop_head(&serial->waiters);
    if (NULL == ln) {
        serial->current = NULL;
        // coro_serial_free 撞上本协程持锁,把释放推给了这里(见该函数末尾)
        if (0 != serial->closed) {
            list_remove(&co->serials, &serial->node);
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
    serial->since = timer_cur_ms(&co->timer);
    // curco 的还原目标恒为调用方自己那个协程,不必外传
    // 下面这行之后不许再碰 serial:锁已交给 wco,它在自己的临界区里可以 coro_serial_free
    // （标记后由它那次 leave 释放）,回到这里时对象可能已经没了。此后只用 co / nxt / wco
    _coro_resume_reap(co, wco);
    pool_push(&co->serial_node_pool, nxt, 0);
}
int32_t coro_serial_call(coro_serial_ctx *serial, coro_fn fn, void *arg) {
    if (ERR_OK != coro_serial_enter(serial)) {
        return ERR_FAILED;
    }
    // 临界区主体：fn 内任意挂起期间，锁仍由 self 持有（serial->current 不变），其他协程进 cs 走"跨协程路径"挂起。
    // C 无 xpcall：fn 内 abort/segfault 直接终止进程，本函数不兜底（与 coro_fork 同约定）
    if (NULL != fn) {
        fn(serial->co->owner, arg);
    }
    coro_serial_leave(serial);
    return ERR_OK;
}
// 把一条挂起协程信息追加到 binary；C 协程无栈回溯,仅 sess / 标签 / 挂起时长。字段名 mtype 保持不变，外部按它解析
static void _coro_dump_one(coro_ctx *co, binary_ctx *bw, uint64_t sess, const coro_info *ci, uint64_t now) {
    if (NULL != co->hooks.tagstr) {
        binary_set_va(bw, "sess=%" PRIu64 " co=%p mtype=%s age=%" PRIu64 "ms\n",
            sess, (void *)ci->co, co->hooks.tagstr(ci->tag), now - ci->since);
    } else {
        binary_set_va(bw, "sess=%" PRIu64 " co=%p mtype=%d age=%" PRIu64 "ms\n",
            sess, (void *)ci->co, ci->tag, now - ci->since);
    }
}
char *coro_dump(coro_ctx *co, size_t *size) {
    binary_ctx bw;
    binary_init_write(&bw, 0, 0);
    coro_sess *corosess;
    size_t iter = 0;
    int32_t total = 0;
    uint64_t now = timer_cur_ms(&co->timer);
    coro_info *ci;
    while (coro_map_iter(co->mapco, &iter, &corosess)) {
        list_foreach(&corosess->waiters, it) {
            ci = UPCAST(it, coro_info, node);
            _coro_dump_one(co, &bw, corosess->sess, ci, now);
            total++;
        }
    }
    int32_t nfork = 0;
    fork_wait_ctx *fw;
    list_foreach(&co->fork_waited, fit) {
        fw = UPCAST(fit, fork_wait_ctx, node);
        binary_set_va(&bw, "fork_wait pending=%d age=%" PRIu64 "ms\n", fw->waited, now - fw->since);
        nfork++;
    }
    int32_t nserial = 0;
    int32_t nwait;
    uint64_t oldest, hold;
    coro_serial_ctx *sl;
    serial_node *nd;
    list_foreach(&co->serials, sit) {
        sl = UPCAST(sit, coro_serial_ctx, node);
        nwait = 0;
        oldest = now;
        list_foreach(&sl->waiters, wit) {
            nd = UPCAST(wit, serial_node, node);
            if (nd->since < oldest) {
                oldest = nd->since;
            }
            nwait++;
        }
        hold = NULL == sl->current ? 0 : now - sl->since;
        if (nwait > 0) {
            binary_set_va(&bw, "serial=%p co=%p held=%d hold=%" PRIu64 "ms waiters=%d age=%" PRIu64 "ms\n",
                (void *)sl, (void *)sl->current, NULL != sl->current, hold, nwait, now - oldest);
        } else if (NULL != sl->current) {
            binary_set_va(&bw, "serial=%p co=%p held=1 hold=%" PRIu64 "ms waiters=0\n",
                (void *)sl, (void *)sl->current, hold);
        }
        nserial += nwait;
    }
    // sessions 是 mapco 的条目数，与 suspended（挂起协程数）不是一回事：keep 的条目摘空 waiters
    // 后仍留着复用。sessions 只增不减、suspended 长期为 0，就是 keep 条目泄漏
    binary_set_va(&bw, "%d suspended, %d sessions, %d fork_wait, %d serial, %d yield total.",
                  total, (int32_t)coro_map_size(co->mapco), nfork, nserial, co->nyield);
    SET_PTR(size, bw.offset);
    return bw.data;
}
