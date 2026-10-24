#include "srey/loader.h"
#include "containers/hashmap.h"
#include "srey/task.h"
#include "coro/coro.h"

#define TASK_MSG_BATCH   32 // 单次批量 pop 消息的最大条数,也是 worker 与跑绑定 task 的 net 线程栈上那个数组的长度
                            // (按值存 message_ctx);调大摊薄出队开销,代价是两处栈占用等比涨
#define CLOSING_WARN_MS  15000// 关闭期每隔这么久把还没退的 task 打一遍
#define MONITOR_INTERVAL_MS 5000// 监控线程每隔这么久查一次有没有 task 卡在一条消息上
#define WORKER_IDLE_SPIN_MIN_US 1 // worker 睡前空转时长的下限(微秒);到活间隔长于唤醒代价时只转这么久,规则见 waiter.h 的 spinner_ctx
#define WORKER_IDLE_SPIN_MAX_US 50 // 空转时长的上限(微秒)

typedef struct _task_each_arg {
    task_each_cb cb;
    void *arg;
}_task_each_arg;

loader_ctx *g_loader; // 全局 loader 单例，程序入口把 loader_init 的返回值存进来
// 前向声明：绑定 task 在 net 线程上跑一轮也走它，而它末尾的重调度又要用到 _loader_task_schedule
static void _loader_task_run(loader_ctx *loader, int32_t weight, int32_t onworker,
    worker_version *version, task_dispatch_arg *runarg, message_ctx *msgbatch);

// 哈希表元素析构回调：通过任务名指针反推 task_ctx 并释放
static void _loader_task_free(void *item) {
    task_free(UPCAST(*((name_t **)item), task_ctx, handle));
}
// 分布式读锁的 slot 注册:失败不致命(退化成抢 fallback 共享锁),但槽位是手算的,
// 算漏了只能靠这条日志看出来。两把锁各自的容量表达式见 loader_init
static void _loader_slot_reg(rwlock_distr_ctx *lck, const char *which) {
    if (ERR_OK != rwlock_distr_register(lck)) {
        LOG_WARN("%s rwlock slot exhausted, this thread falls back to the shared lock.", which);
    }
}
// 线程 init / exit 钩子,thread_creat_hooks 在业务回调前后各调一次。
// 只管 loader 自己的线程级状态(slot、coro 缓存);与 loader 无关的线程级缓存(如 buffer 备用节点)
// 挂 main 注册的 thread_global_hooks,对所有线程生效。base 只给时间轮线程用,
// 不跑 Lua 故不要 lckcache slot
static void _loader_hook_init_base(void *udata, void *assist) {
    (void)udata;
    _loader_slot_reg(&((loader_ctx *)assist)->lckmaptasks, "maptasks");
}
static void _loader_hook_exit_base(void *udata, void *assist) {
    (void)udata;
    coro_thread_cleanup();
    rwlock_distr_unregister(&((loader_ctx *)assist)->lckmaptasks);
}
// worker 钩子:base 之外多一把 lckcache slot(加载脚本与 require 访问字节码缓存);退出顺序与进入相反。
// net 线程也挂它：上面可能跑绑定的 Lua task 与协程。ev_init 给 Windows 的 AcceptEx 线程的也是这一组
static void _loader_hook_init_worker(void *udata, void *assist) {
    _loader_hook_init_base(udata, assist);
#if WITH_LUA && ENABLE_LUA_BYTECACHE
    _loader_slot_reg(&((loader_ctx *)assist)->lckcache, "bytecache");
#endif
}
static void _loader_hook_exit_worker(void *udata, void *assist) {
#if WITH_LUA && ENABLE_LUA_BYTECACHE
    rwlock_distr_unregister(&((loader_ctx *)assist)->lckcache);
#endif
    _loader_hook_exit_base(udata, assist);
}
// 唤醒所有处于等待状态的工作线程（用于 loader_free 时通知退出）
static void _loader_worker_wakeup_all(loader_ctx *loader) {
    worker_ctx *worker;
    for (uint16_t i = 0; i < loader->nworker; i++) {
        worker = &loader->worker[i];
        if (WAITER_AWAKE != waiter_state_get(&worker->waiter)) {
            waiter_wake(&worker->waiter, NULL);
        }
    }
}
// 从 start 起走 k 步的环形下标：start 与 k 都小于 n，故和 < 2n，减一次即等价于取模，
// 省掉每轮一次硬件除法。累加必须用 uint32_t——先截成 uint16_t 再减的话，
// nworker > 32768 时和会绕过 65535，那一下截断后面任何判断都纠正不回来
static inline uint32_t _loader_ring_next(uint16_t start, uint16_t k, uint16_t n) {
    uint32_t i = (uint32_t)start + k;
    return i >= n ? i - n : i;
}
// 选一个 worker 接这个 task：RR 选起点扫一遍，正空转等活的最优（投完它自己就看见，
// 省一次 futex 唤醒），其次是睡着的，全忙则退回 RR 起点（自调节交给 work-stealing）
static inline uint16_t _loader_pick_worker(loader_ctx *loader) {
    if (1 == loader->nworker) {
        return 0;
    }
    uint16_t start = (uint16_t)(ATOMIC64_ADD_RELAXED(&loader->index, 1) % loader->nworker);
    uint16_t idx, hit = loader->nworker;
    for (uint16_t i = 0; i < loader->nworker; i++) {
        // 从 start 起点的环形迭代器：不固定从 0 开始，既避免总命中索引最小的空闲 worker，
        // 也保证全员忙时 fallback 与 RR 公平性一致
        idx = (uint16_t)_loader_ring_next(start, i, loader->nworker);
        if (ATOMIC_GET_RELAXED(&loader->worker[idx].spinning) > 0) {
            return idx;
        }
        if (loader->nworker == hit
            && WAITER_AWAKE != waiter_state_hint(&loader->worker[idx].waiter)) {
            hit = idx;
        }
    }
    return (loader->nworker == hit) ? start : hit;
}
// 把任务投递到某个工作线程队列并在必要时唤醒该线程。
// 入队前 incref：在途期间引用由队列持有，与 _loader_worker_loop 跑完的 ungrab 配对
static inline void _loader_worker_wakeup(loader_ctx *loader, task_ctx *task) {
    worker_ctx *worker = &loader->worker[_loader_pick_worker(loader)];
    task_incref(task);
    taskq_push(&worker->qutasks, &task);
    // 先入队再读睡眠态(规则见 waiter.h)，同时投来的只让抢到的那个叫。
    // 带上信号时刻，worker 醒来据此量唤醒代价
    if (WAITER_SLEEP == waiter_state_get(&worker->waiter)
        && waiter_mark_waking(&worker->waiter, WAITER_SLEEP)) {
        waiter_wake(&worker->waiter, &loader->timer);
    }
}
static void _loader_evtask_free(void *arg) {
    task_ungrab((task_ctx *)arg);
}
#if 0 != EVTASK_SLOW_MS
// 绑定 task 在第 idx 个 net 线程上从 t0(timer_cur_ms)跑到现在，超阈值告警。整轮与收包当场处理共用
static inline void _loader_evslow_check(loader_ctx *loader, task_ctx *task, int32_t idx, uint64_t t0) {
    uint64_t cost = timer_cur_ms(&loader->timer) - t0;
    if (tda_check(&loader->evslow[idx], (size_t)cost)) {
        LOG_WARN("task %s ran %" PRIu64 "ms in one round on net thread %d, every connection there waited.",
                 _NAME_OR(task->name), cost, idx);
    }
}
#endif
// 绑定 task 在它的 net 线程上跑一轮：weight 取 0 吃掉当时全部积压，跑时新进的由 _loader_task_run 末尾经 ev_defer_exec 重投，
// 何时再跑按它的规则(本轮最多再跑一趟)
static void _loader_evtask_run(void *arg) {
    task_ctx *task = (task_ctx *)arg;
    loader_ctx *loader = task->loader;
    int32_t idx = ev_cur_index(&loader->netev);
    task_dispatch_arg runarg;
    message_ctx msgbatch[TASK_MSG_BATCH];
#if 0 != EVTASK_SLOW_MS
    uint64_t t0;
#endif
    ASSERTAB(idx >= 0, "evtask must run on a net thread.");// 否则会写到某个 worker 的 monitor 槽
    runarg.task = task;
#if 0 != EVTASK_SLOW_MS
    t0 = timer_cur_ms(&loader->timer);
#endif
    _loader_task_run(loader, 0, 0, &loader->monitor.version[loader->nworker + idx], &runarg, msgbatch);
#if 0 != EVTASK_SLOW_MS
    _loader_evslow_check(loader, task, idx, t0);
#endif
    task_ungrab(task);
}
// 调度一个刚抢到 global 的 task：绑了 net 线程就投给那条线程，否则照旧给 worker。
// evbind 只在抢到 global 之后读，改绑随时可调，最多影响下一轮
static inline void _loader_task_schedule(loader_ctx *loader, task_ctx *task) {
    int32_t bind = (int32_t)ATOMIC_GET_RELAXED(&task->evbind);
    if (0 != bind) {
        task_incref(task);// 在途引用由投递持有，与 _loader_evtask_run / _loader_evtask_free 配对
        ev_defer_exec(&loader->netev, bind - 1, _loader_evtask_run, _loader_evtask_free, task);
        return;
    }
    _loader_worker_wakeup(loader, task);
}
// 触发调度：队列非空而尚未被调度时交给 worker(绑定的交给它的 net 线程)
void _task_message_active(task_ctx *task) {
    // CAS 0→1：只有首个生产者负责调度，避免重复唤醒
    if (ATOMIC_CAS(&task->global, 0, 1)) {
        _loader_task_schedule(task->loader, task);
    }
}
// 入队并触发调度；非网络生产者（超时、请求、响应、广播）都走这个
void _task_message_post(task_ctx *task, message_ctx *msg) {
    msgq_push(&task->qumsg, msg);
    _task_message_active(task);
}
// 找出积压任务最多的 worker 索引，用于任务窃取；队列全空时返回 -1。
// 积压数只拿来挑偷谁，用不拿锁的 size_fast，免得每次空闲都把别人的队列锁挨个抢一遍
static inline int32_t _loader_max_task_index(loader_ctx *loader, uint16_t exclude) {
    uint16_t index = 0;
    uint32_t max = 0;
    uint32_t count;
    uint16_t start = (uint16_t)((exclude + 1) % loader->nworker);
    uint16_t i;
    for (uint16_t k = 0; k < loader->nworker; k++) {
        i = (uint16_t)_loader_ring_next(start, k, loader->nworker);
        if (i == exclude) {
            continue;
        }
        count = taskq_size_fast(&loader->worker[i].qutasks);
        if (count > max) {
            index = i;
            max = count;
        }
    }
    return 0 == max ? -1 : (int32_t)index;
}
// 从本地队列或其他 worker 队列（工作窃取）取出下一个待处理任务
// inflight 出参：没取到时回传本轮有没有撞上在途元素，调用方据此决定退避还是休眠
static inline task_ctx *_loader_task_get(loader_ctx *loader, worker_ctx *worker, int32_t *inflight) {
    task_ctx *task;
    int32_t rtn = taskq_pop(&worker->qutasks, &task);
    if (ERR_OK == rtn) {
        *inflight = 0;
        return task;
    }
    *inflight = (1 == rtn);
    // 本地队列为空：尝试从积压最多的 worker 偷一个任务
    int32_t index = _loader_max_task_index(loader, worker->index);
    if (-1 != index) {
        rtn = taskq_pop(&loader->worker[index].qutasks, &task);
        if (ERR_OK == rtn) {
            *inflight = 0;
            return task;
        }
        if (1 == rtn) {
            *inflight = 1;
        }
    }
    return NULL;
}
// 本轮该消费几条：weight 定基数（lens >> weight，-1 固定 1 条；worker 取自己的 weight，net 线程取 0），
// task.priority 再以基数的 1/8 为单位加成（每 +8 翻倍，每 +1 约 +12.5%，
// 0 走快路径），结果夹在 [1, lens]
static inline uint32_t _loader_msg_quota(int32_t weight, task_ctx *task, uint32_t lens) {
    uint32_t n_base = weight >= 0 ? (lens >> weight) : 1;
    atomic_t prio = ATOMIC_GET(&task->priority);
    uint32_t n = (0 == prio) ? n_base : n_base + (uint32_t)(((uint64_t)n_base * prio) >> 3);
    if (n > lens) {
        n = lens;
    }
    return (0 == n) ? 1 : n;
}
// 一轮跑完交出调度权：round_end、清 msgtype、global 1→0 后队列里还有就重投。worker、net 线程与内联直调共用
static inline void _loader_task_release(loader_ctx *loader, task_ctx *task, worker_version *version) {
    // 必须在 CAS global 1→0 之前：交出调度权后别的线程可能同时进来
    if (NULL != task->_round_end) {
        task->_round_end(task);
    }
    // 退出 dispatch 进入空闲，清 msgtype 让 monitor 区分"卡死"与"空闲"
    ATOMIC_SET_RELAXED(&version->msgtype, MSG_TYPE_NONE);
    // 无锁重调度：先将 global CAS 1→0（取消调度），再检查队列是否仍有消息。
    // 若有：尝试 CAS 0→1 重新调度；若 CAS 失败说明某生产者已抢先调度。
    // CAS 之后的判空也得是足序的(环形后端按 seq_cst 读、自旋后端拿锁，见 fsqu 的 empty)，和生产者"入队 → CAS global"才不会同时看漏。
    ATOMIC_CAS(&task->global, 1, 0);
    if (!msgq_empty(&task->qumsg)) {
        if (ATOMIC_CAS(&task->global, 0, 1)) {
            _loader_task_schedule(loader, task);
        }
    }
}
// 分发统计的计时起点；没开统计恒为 0
static inline uint64_t _loader_stat_now(void) {
#if ENABLE_DISPATCH_STAT
    return timer_thread_cpu_ns();
#else
    return 0;
#endif
}
// 分发 runarg->msg 这一条：先记 monitor 的版本号与消息类型，再调分发函数。
// t0 是本条的计时起点，返回结束时刻给同批下一条当起点(省一次取时钟)；没开统计时恒为 0
static inline uint64_t _loader_dispatch_one(task_ctx *task, worker_version *version,
    task_dispatch_arg *runarg, uint64_t t0) {
    msg_type mtype = runarg->msg->mtype;
    ATOMIC_ADD_RELAXED(&version->ver, 1);
    ATOMIC_SET_RELAXED(&version->msgtype, mtype);
    task->_task_dispatch(runarg);
#if ENABLE_DISPATCH_STAT
    uint64_t t1 = _loader_stat_now();
    task->dispatch_cpu_ns[mtype] += t1 - t0;
    ++task->nmsg[mtype];
    return t1;
#else
    (void)t0;
    return 0;
#endif
}
// 从任务消息队列批量取出消息并依次分发，处理完成后重调度或清除调度标志。
static void _loader_task_run(loader_ctx *loader, int32_t weight, int32_t onworker,
    worker_version *version, task_dispatch_arg *runarg, message_ctx *msgbatch) {
    task_ctx *task = runarg->task;
    uint32_t lens = msgq_size(&task->qumsg);
    if (tda_check(&task->tda, lens)) {
        LOG_WARN("task %s overload, message queue length %u.", _NAME_OR(task->name), lens);
    }
    uint32_t n = _loader_msg_quota(weight, task, lens);
    uint32_t want, got, k, processed = 0;
    uint32_t spins = 0;
    uint32_t tries = 0;
    uint64_t t0;
    // version 是每个线程独占的单写者字段, monitor 每 5s 才读一次, 用 RELAXED 换掉不必要的 seq_cst 屏障
    ATOMIC64_SET_RELAXED(&version->handle, task->handle);
    // task->global CAS 保证同 task 同一时刻仅一个线程调度，qumsg 是单消费者，走 pop_sc_batch
    while (processed < n) {
        want = n - processed;
        if (want > TASK_MSG_BATCH) {
            want = TASK_MSG_BATCH;
        }
        got = msgq_pop_sc_batch(&task->qumsg, msgbatch, want);
        if (0 == got) {
            // onworker 非 0 表示在 worker 上跑：第一批就撞上在途元素(生产者已占槽、还没写完)时照 taskq 的做法退避重试，
            // 不立刻交还调度权让别的 worker 空跑；net 线程上不等，等了会卡住该线程上所有连接
            if (0 != onworker
                && 0 == processed
                && tries < 2 * SPIN_YIELD_CNT
                && !msgq_empty(&task->qumsg)) {
                tries++;
                spin_backoff(&spins);
                continue;
            }
            break;
        }
        t0 = _loader_stat_now();
        for (k = 0; k < got; k++) {
            runarg->msg = &msgbatch[k];
            t0 = _loader_dispatch_one(task, version, runarg, t0);
        }
        processed += got;
    }
    _loader_task_release(loader, task, version);
}
int32_t _loader_task_try_run(task_ctx *task, message_ctx *msg) {
    loader_ctx *loader = task->loader;
    int32_t idx = ev_cur_index(&loader->netev);
    worker_version *version;
    task_dispatch_arg runarg;
#if 0 != EVTASK_SLOW_MS
    uint64_t t0;
#endif
    // 它在本线程上还有没跑完的消息(含同一连接先推的)就得排在后面。判空只需看见本线程自己推的，
    // 别的线程推的与这条谁先谁后本来就不保证，故用不拿锁的 empty_fast
    if (idx < 0
        || ATOMIC_GET_RELAXED(&task->evbind) != (atomic_t)(idx + 1)
        || !msgq_empty_fast(&task->qumsg)
        || !ATOMIC_CAS(&task->global, 0, 1)) {
        return ERR_FAILED;
    }
    version = &loader->monitor.version[loader->nworker + idx];
    ATOMIC64_SET_RELAXED(&version->handle, task->handle);
    runarg.task = task;
    runarg.msg = msg;
#if 0 != EVTASK_SLOW_MS
    t0 = timer_cur_ms(&loader->timer);
#endif
    (void)_loader_dispatch_one(task, version, &runarg, _loader_stat_now());
    _loader_task_release(loader, task, version);
#if 0 != EVTASK_SLOW_MS
    _loader_evslow_check(loader, task, idx, t0);
#endif
    return ERR_OK;
}
// 空转时判活：挂上 spinning 后生产者会把 task 直接投给我，不必再发 futex 唤醒——那笔唤醒开销是
// task 层相对裸 event 层的大头。只看自己的队列，扫别人的会抢他们的锁
static int32_t _loader_worker_ready(void *arg) {
    return !taskq_empty_fast(&((worker_ctx *)arg)->qutasks);
}
// 挂起等唤醒，握手见 waiter.h。复查到已有活或已停就不睡了。
// 返回叫醒这次睡眠的首个信号的发出时刻(纳秒),近似活真正到来的时刻;0 表示没测到(没睡、虚假唤醒或关停时的叫醒)
static inline uint64_t _loader_worker_sleep(loader_ctx *loader, worker_ctx *worker) {
    waiter_prepare(&worker->waiter, WAITER_SLEEP);
    if (!taskq_empty(&worker->qutasks)
        || 0 != ATOMIC_GET(&loader->stop)) {
        waiter_cancel(&worker->waiter);
        return 0;
    }
    return waiter_commit(&worker->waiter, 0);
}
// 工作线程主循环：持续从队列取任务并分发消息，队列空时阻塞等待唤醒
static void _loader_worker_loop(void *arg) {
    task_ctx *task;
    worker_ctx *worker = (worker_ctx *)arg;
    loader_ctx *loader = worker->loader;
    worker_version *version = &loader->monitor.version[worker->index];
    task_dispatch_arg runarg;
    message_ctx msgbatch[TASK_MSG_BATCH];
    int32_t inflight = 0;
    uint32_t spins = 0;
    spinner_ctx spinner;
    spinner_init(&spinner, &loader->timer, WORKER_IDLE_SPIN_MIN_US, WORKER_IDLE_SPIN_MAX_US);
    while (0 == ATOMIC_GET_RELAXED(&loader->stop)) {
        // 从队列取一任务
        task = _loader_task_get(loader, worker, &inflight);
        if (NULL != task) {
            spins = 0;
            spinner_busy(&spinner);
            runarg.task = task;
            // 执行
            _loader_task_run(loader, worker->weight, 1, version, &runarg, msgbatch);
            task_ungrab(task);
            continue;
        }
        // 队列是否还有尚未发布的数据
        if (0 != inflight) {
            spin_backoff(&spins);
            continue;
        }
        spins = 0;
        // 睡前先空转等一会儿，转多久见 spinner_ctx
        if (0 != spinner_spin(&spinner, &worker->spinning, _loader_worker_ready, worker)) {
            continue;
        }
        // 挂起等唤醒
        spinner_wake(&spinner, _loader_worker_sleep(loader, worker));
    }
    LOG_INFO("worker thread %d exited.", worker->index);
}
// 检查各工作线程与 net 线程是否卡死（消息版本号未变化且仍有消息在处理）。
// 版本槽前 nworker 个归 worker，其后 nnet 个归跑绑定 task 的 net 线程
static void _loader_monitor_check(loader_ctx *loader) {
    int32_t ver;
    worker_version *version;
    name_t handle;
    task_ctx *task;
    uint32_t n = (uint32_t)loader->nworker + loader->nnet;
    for (uint32_t i = 0; i < n; i++) {
        version = &loader->monitor.version[i];
        ver = (int32_t)ATOMIC_GET(&version->ver);
        if (version->ckver == ver
            && MSG_TYPE_NONE != ATOMIC_GET(&version->msgtype)) {
            handle = ATOMIC64_GET(&version->handle);
            task = task_grab(loader, handle);
            LOG_WARN("task: %s message type: %d, maybe in an endless loop.",
                task ? _NAME_OR(task->name) : "?", ATOMIC_GET(&version->msgtype));
            if (NULL != task) {
                task_ungrab(task);
            }
        } else {
            version->ckver = ver;
        }
    }
}
// 监控线程主循环：每 MONITOR_INTERVAL_MS 调用 _loader_monitor_check 检测卡死的工作线程
static void _loader_monitor_loop(void *arg) {
    loader_ctx *loader = (loader_ctx *)arg;
    uint64_t next = timer_cur_ms(&loader->timer) + MONITOR_INTERVAL_MS;
    for (;;) {
        // 睡前复查 stop(握手见 waiter.h)，loader_free 置位后叫醒，不会白等满间隔
        waiter_prepare(&loader->monitor.waiter, WAITER_SLEEP);
        if (0 != ATOMIC_GET(&loader->monitor.stop)) {
            waiter_cancel(&loader->monitor.waiter);
            break;
        }
        // 必须等满间隔再查：提前醒会缩短判定窗口，把正常在跑的消息误报成卡死。被叫醒只会是要停，回去复查
        if (ERR_OK == waiter_commit_until(&loader->monitor.waiter, &loader->timer, next)) {
            continue;
        }
        if (0 != ATOMIC_GET(&loader->monitor.stop)) {
            break;
        }
        _loader_monitor_check(loader);
        next = timer_cur_ms(&loader->timer) + MONITOR_INTERVAL_MS;
    }
    LOG_INFO("%s", "worker monitor thread exited.");
}
loader_ctx *loader_init(uint16_t nnet, uint16_t nworker, uint32_t twcap) {
    loader_ctx *loader;
    CALLOC(loader, 1, sizeof(loader_ctx));
    prots_init(_task_net_emit());
#if WITH_SSL
    evssl_init();
    evssl_pool_init();
#endif
    loader->nworker = 0 == nworker ? procscnt_avail() : nworker;
    loader->nnet = 0 == nnet ? (uint16_t)procscnt_avail() : nnet;
    timer_init(&loader->timer);
    CALLOC(loader->worker, 1, sizeof(worker_ctx) * loader->nworker);
    CALLOC(loader->monitor.version, 1, sizeof(worker_version) * ((size_t)loader->nworker + loader->nnet));
    waiter_init(&loader->monitor.waiter);
    waiter_init(&loader->closing_waiter);
    // 槽位要覆盖全部注册方而不只是 worker：net 线程 nnet 个、时间轮 1 个、Windows 的 AcceptEx
    // 线程最多 2 个(iocp.c 的 nacpex)，它们挂的 hooks_worker / hooks_base 都注册这把锁。少算了就有线程 register
    // 失败、此后每次 task_grab 都退化去抢 fallback 那把共享读写锁，分布式读锁白建
    uint32_t nreg = (uint32_t)loader->nworker + loader->nnet + 3;
    rwlock_distr_init(&loader->lckmaptasks, nreg);
#if WITH_LUA && ENABLE_LUA_BYTECACHE
    rwlock_distr_init(&loader->lckcache, (uint32_t)loader->nworker + loader->nnet + 3);
#endif
    const thread_hooks hooks_worker = {
        _loader_hook_init_worker, _loader_hook_exit_worker, loader
    };
    const thread_hooks hooks_base = {
        _loader_hook_init_base, _loader_hook_exit_base, loader
    };
    CALLOC(loader->evslow, loader->nnet, sizeof(tda_ctx));
    for (uint16_t k = 0; k < loader->nnet; k++) {
        tda_init(&loader->evslow[k], EVTASK_SLOW_MS);
    }
    loader->maptasks = task_map_new(ONEK, _loader_task_free);
    loader->mapnames = tname_map_new(ONEK, NULL);
    loader->monitor.thread_monitor = thread_creat(_loader_monitor_loop, loader);
    // 每轮处理消息数 = lens >> weight（-1 是特例，固定 1 条），故 weight 越大越保守：
    //   -1: 1 条    0: 全量    1: lens/2    2: lens/4    3: lens/8
    int32_t weights[] = {
        3, 1, 2, -1, 0, 0, 0, 0,
        1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 1, 1, 1, 1,
        2, 2, 2, 2, 3, 3, 3, 3
    };
    worker_ctx *worker;
    uint16_t i, wn = ARRAY_SIZE(weights);
    // 先把所有 worker 的字段初始化完(taskq_init/waiter_init)
    // 再启动 worker 线程;否则 worker[0] 启动后会跨 worker 遍历 worker[1..N-1].qutasks
    // 而后者的 taskq_init 还在主线程进行中(TSan 报 taskq_size 的 race)
    for (i = 0; i < loader->nworker; i++) {
        worker = &loader->worker[i];
        worker->index = i;
        worker->weight = weights[i % wn];
        worker->loader = loader;
        taskq_init(&worker->qutasks, ONEK);
        waiter_init(&worker->waiter);
    }
    // pthread_create 的 release barrier 保证 worker 线程能看到第一轮循环的所有写入
    for (i = 0; i < loader->nworker; i++) {
        worker = &loader->worker[i];
        worker->thread_worker = thread_creat_hooks(_loader_worker_loop, hooks_worker.init, hooks_worker.exit, worker, hooks_worker.assist);
    }
    tw_init(&loader->tw, twcap, &hooks_base);
    ev_init(&loader->netev, loader->nnet, &hooks_worker);
    return loader;
}
uint32_t loader_nnet(loader_ctx *loader) {
    return loader->nnet;
}
#if WITH_LUA && ENABLE_LUA_BYTECACHE
rwlock_distr_ctx *loader_lckcache(loader_ctx *loader) {
    return &loader->lckcache;
}
#endif
// task_map_scan 回调：向每个任务推送 MSG_TYPE_CLOSING 消息（仅推一次）
static int32_t _loader_closing_push(name_t *const *item, void *udata) {
    task_ctx *task = UPCAST(*item, task_ctx, handle);
    if (ATOMIC_CAS(&task->closing, 0, 1)) {
        _task_message_post(task, udata);
    }
    return 1;/* 非 0 = 继续扫 */
}
// task_map_scan 回调：打印仍未退出的任务警告（关闭超时时使用）
static int32_t _loader_closing_timeout(name_t *const *item, void *udata) {
    (void)udata;
    task_ctx *task = UPCAST(*item, task_ctx, handle);
    LOG_WARN("task %s close timeout, ref %d.", _NAME_OR(task->name), ATOMIC_GET(&task->ref));
    return 1;/* 非 0 = 继续扫 */
}
// 广播关闭消息给所有任务，并等待所有任务退出、net 线程上推迟的释放全部做完(之后 ev_free 才能停 net 线程)。
// 每 CLOSING_WARN_MS 毫秒打一次仍在的 task
static void _loader_task_closing(loader_ctx *loader) {
    message_ctx closing = { 0 };
    closing.mtype = MSG_TYPE_CLOSING;
    rwlock_distr_rdlock(&loader->lckmaptasks);
    // 在持锁期间置位 closing，与 task_register 的写锁互斥：先注册的被本次扫描覆盖，
    // 后注册的由 task_register 自己看到 closing=1 追加 CLOSING，两侧都不会漏。
    // 用足序：与推迟释放那边"减 nfreeing 再查 closing"握手，见 _task_free_deferred
    ATOMIC_SET(&loader->closing, 1);
    task_map_scan(loader->maptasks, _loader_closing_push, &closing);
    rwlock_distr_runlock(&loader->lckmaptasks);
    // 查计数夹在 waiter_prepare 与 cancel / commit_until 之间(握手见 waiter.h)，发布方摘掉最后一个再叫醒
    // 不会落在"查到非 0"与"开始等"之间。发布方见 task_ungrab 与 _task_free_deferred。
    // 先查表后查 nfreeing：推迟释放的计数在摘表的写锁里加，看到表空就一定看得到它
    uint64_t next_warn = timer_cur_ms(&loader->timer) + CLOSING_WARN_MS;
    size_t n;
    for (;;) {
        waiter_prepare(&loader->closing_waiter, WAITER_SLEEP);
        rwlock_distr_rdlock(&loader->lckmaptasks);
        n = task_map_size(loader->maptasks);
        rwlock_distr_runlock(&loader->lckmaptasks);
        if (0 == n
            && 0 == ATOMIC_GET_SEQCST(&loader->nfreeing)) {
            waiter_cancel(&loader->closing_waiter);
            break;
        }
        // 被叫醒就回去重查计数；到点(已过点则不睡)把还没退的 task 打一遍
        if (1 == waiter_commit_until(&loader->closing_waiter, &loader->timer, next_warn)) {
            next_warn = timer_cur_ms(&loader->timer) + CLOSING_WARN_MS;
            rwlock_distr_rdlock(&loader->lckmaptasks);
            task_map_scan(loader->maptasks, _loader_closing_timeout, NULL);
            rwlock_distr_runlock(&loader->lckmaptasks);
            if (0 != ATOMIC_GET(&loader->nfreeing)) {
                LOG_WARN("%d task(s) removed from the table but not freed yet.", (int32_t)ATOMIC_GET(&loader->nfreeing));
            }
        }
    }
}
static int32_t _loader_task_each_scan(name_t *const *item, void *udata) {
    _task_each_arg *w = (_task_each_arg *)udata;
    task_ctx *task = UPCAST(*item, task_ctx, handle);
    w->cb(task->name, task->handle, w->arg);
    return 1;/* 非 0 = 继续扫 */
}
void loader_task_each(loader_ctx *loader, task_each_cb cb, void *arg) {
    _task_each_arg wrap = { cb, arg };
    rwlock_distr_rdlock(&loader->lckmaptasks);
    task_map_scan(loader->maptasks, _loader_task_each_scan, &wrap);
    rwlock_distr_runlock(&loader->lckmaptasks);
}
void loader_free(loader_ctx *loader) {
    _loader_task_closing(loader);
    ATOMIC_SET_SEQCST(&loader->stop, 1);
    worker_ctx *worker;
    _loader_worker_wakeup_all(loader);
    for (uint16_t i = 0; i < loader->nworker; i++) {
        worker = &loader->worker[i];
        thread_join(worker->thread_worker);
    }
    ATOMIC_SET_RELAXED(&loader->monitor.stop, 1);
    waiter_wake(&loader->monitor.waiter, NULL);
    thread_join(loader->monitor.thread_monitor);
    waiter_free(&loader->monitor.waiter);
    ev_free(&loader->netev);
    tw_free(&loader->tw);
    waiter_free(&loader->closing_waiter);
    for (uint16_t i = 0; i < loader->nworker; i++) {
        worker = &loader->worker[i];
        taskq_free(&worker->qutasks);
        waiter_free(&worker->waiter);
    }
    task_map_free(loader->maptasks);
    tname_map_free(loader->mapnames);
    rwlock_distr_free(&loader->lckmaptasks);
#if WITH_LUA && ENABLE_LUA_BYTECACHE
    rwlock_distr_free(&loader->lckcache);
#endif
    prots_free();
#if WITH_SSL
    evssl_pool_free();
#endif
    FREE(loader->worker);
    FREE(loader->monitor.version);
    FREE(loader->evslow);
    FREE(loader);
}
void loader_on_exit(loader_ctx *loader, loader_exit_cb cb, void *arg) {
    loader->exit_arg = arg;
    loader->exit_cb = cb;
}
void loader_exit(loader_ctx *loader, int32_t code) {
    if (NULL == loader->exit_cb) {
        LOG_WARN("exit requested with code %d, but no exit hook registered.", code);
        return;
    }
    loader->exit_cb(code, loader->exit_arg);
}
