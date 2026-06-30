#include "srey/loader.h"
#include "containers/hashmap.h"
#include "srey/task.h"
#include "utils/utils.h"

#define TASK_MSG_BATCH  128

typedef struct _task_each_arg {
    task_each_cb cb;
    void *arg;
}_task_each_arg;

loader_ctx *g_loader; // 全局 loader 单例，由 loader_init 创建

// 计算任务名在哈希表中的哈希值
static uint64_t _loader_task_hash(const void *item, uint64_t seed0, uint64_t seed1) {
    (void)seed0;
    (void)seed1;
    return hash_u64((uint64_t)(*(*(name_t **)item)));
}
// 比较两个任务名（用于哈希表碰撞解决）
// name_t 为 uint64_t，不能用减法（差值溢出导致符号翻转），
// 使用三路比较确保所有 uint64_t 值均正确排序。
static int _loader_task_compare(const void *a, const void *b, void *ud) {
    (void)ud;
    name_t va = *(*(name_t **)a);
    name_t vb = *(*(name_t **)b);
    return (va > vb) - (va < vb);
}
// 哈希表元素析构回调：通过任务名指针反推 task_ctx 并释放
static void _loader_task_free(void *item) {
    task_free(UPCAST(*((name_t **)item), task_ctx, handle));
}
// mapnames 哈希回调：按 name_handle_entry.name 字符串计算/比较；元素借用 task_ctx.name，无 free 回调
static uint64_t _loader_name_hash(const void *item, uint64_t seed0, uint64_t seed1) {
    (void)seed0;
    (void)seed1;
    const name_handle_entry *e = (const name_handle_entry *)item;
    return hash(e->name, strlen(e->name));
}
static int _loader_name_compare(const void *a, const void *b, void *ud) {
    (void)ud;
    return strcmp(((const name_handle_entry *)a)->name, ((const name_handle_entry *)b)->name);
}
// 基础 slot 注册:仅 lckmaptasks;net / acpex / tw 用(不跑 Lua,无需 lckcache slot)
static void _loader_slot_register_base(void *udata, void *assist) {
    (void)udata;
    rwlock_distr_register(&((loader_ctx *)assist)->lckmaptasks);
}
static void _loader_slot_unregister_base(void *udata, void *assist) {
    (void)udata;
    rwlock_distr_unregister(&((loader_ctx *)assist)->lckmaptasks);
}
// worker slot 注册:base + lckcache;仅 worker 加载脚本与 require 访问字节码缓存
static void _loader_slot_register_worker(void *udata, void *assist) {
    _loader_slot_register_base(udata, assist);
#if WITH_LUA && ENABLE_LUA_BYTECACHE
    rwlock_distr_register(&((loader_ctx *)assist)->lckcache);
#endif
}
static void _loader_slot_unregister_worker(void *udata, void *assist) {
#if WITH_LUA && ENABLE_LUA_BYTECACHE
    rwlock_distr_unregister(&((loader_ctx *)assist)->lckcache);
#endif
    _loader_slot_unregister_base(udata, assist);
}
// 找出积压任务最多的 worker 索引，用于任务窃取；队列全空时返回 -1
static int32_t _loader_max_task_index(loader_ctx *loader, uint16_t exclude) {
    uint16_t index = 0;
    uint32_t max = 0;
    uint32_t count;
    uint16_t start = (uint16_t)((exclude + 1) % loader->nworker);
    uint16_t i;
    for (uint16_t k = 0; k < loader->nworker; k++) {
        i = (uint16_t)((start + k) % loader->nworker);
        if (i == exclude) {
            continue;
        }
        count = fsqu_size(&loader->worker[i].qutasks);
        if (count > max) {
            index = i;
            max = count;
        }
    }
    return 0 == max ? -1 : (int32_t)index;
}
// 唤醒所有处于等待状态的工作线程（用于 loader_free 时通知退出）
static void _loader_worker_wakeup_all(loader_ctx *loader) {
    worker_ctx *worker;
    for (uint16_t i = 0; i < loader->nworker; i++) {
        worker = &loader->worker[i];
        if (ATOMIC_GET(&worker->waiting) > 0) {
            mutex_lock(&worker->mutex);
            cond_broadcast(&worker->cond);
            mutex_unlock(&worker->mutex);
        }
    }
}
// 将任务名投递到某个工作线程队列并在必要时唤醒该线程
static void _loader_worker_wakeup(loader_ctx *loader, name_t *task) {
    uint16_t target;
    if (1 == loader->nworker) {
        target = 0;
    } else {
        // RR 选起点；从起点扫一遍优先命中 waiting>0 的空闲 worker，
        // 全 0 时退化回 RR 起点（所有 worker 都在跑，自调节由忙 worker 主循环 work-stealing 完成）。
        uint16_t start = (uint16_t)(ATOMIC64_ADD(&loader->index, 1) % loader->nworker);
        uint16_t idx;
        target = start;
        for (uint16_t i = 0; i < loader->nworker; i++) {
            // (start + i) % nworker 形成"从 start 起点的环形迭代器"：
            // 用 start 而非固定从 0 开始，避免多个空闲 worker 时总命中索引最小的，
            // 也保证全员忙时 fallback 与 RR 公平性一致。
            idx = (uint16_t)((start + i) % loader->nworker);
            if (ATOMIC_GET(&loader->worker[idx].waiting) > 0) {
                target = idx;
                break;
            }
        }
    }
    worker_ctx *worker = &loader->worker[target];
    // 阻塞入队；队列满时自旋等待（正常负载下极少发生）
    fsqu_push(&worker->qutasks, task);
    // 必须先入队再读 waiting，与消费者"先写 waiting 再检查队列"形成对称屏障，
    // 确保两者至少有一方能观察到对方的写入，从而消除丢失唤醒窗口。
    // waiting == 0 时 worker 正在运行，无需 signal；仅在 > 0 时才获取 mutex 发信号。
    if (ATOMIC_GET(&worker->waiting) > 0) {
        mutex_lock(&worker->mutex);
        cond_signal(&worker->cond);
        mutex_unlock(&worker->mutex);
    }
}
// 将消息推入任务的无锁消息队列并在必要时唤醒工作线程
void _task_message_push(task_ctx *task, message_ctx *msg) {
    message_ctx *pmsg = (message_ctx *)pool_pop(&task->loader->msg_pool, NULL, 0);
    *pmsg = *msg;
    // 阻塞入队；队列满时自旋等待
    fsqu_push(&task->qumsg, &pmsg);
    // CAS 0→1：只有首个生产者负责调度，避免重复唤醒
    if (ATOMIC_CAS(&task->global, 0, 1)) {
        _loader_worker_wakeup(task->loader, &task->handle);
    }
}
// 从本地队列或其他 worker 队列（工作窃取）取出下一个待处理任务名
static name_t _loader_task_name_get(loader_ctx *loader, worker_ctx *worker) {
    name_t handle;
    if (ERR_OK == fsqu_pop(&worker->qutasks, &handle)) {
        return handle;
    }
    // 本地队列为空：尝试从积压最多的 worker 偷一个任务
    int32_t index = _loader_max_task_index(loader, worker->index);
    if (-1 != index
        && ERR_OK == fsqu_pop(&loader->worker[index].qutasks, &handle)) {
        return handle;
    }
    return INVALID_TNAME;
}
// 单次批量 pop 消息的最大条数（栈上数组上限）
// 从任务消息队列批量取出消息并依次分发，处理完成后重调度或清除调度标志
static void _loader_task_run(loader_ctx *loader, worker_ctx *worker,
    worker_version *version, task_dispatch_arg *runarg, message_ctx **msgbatch) {
    task_ctx *task = runarg->task;
    uint32_t lens = fsqu_size(&task->qumsg);
    if (tda_check(&task->tda, lens)) {
        LOG_WARN("task %s overload, message queue length %u.", _NAME_OR(task->name), lens);
    }
    // n_base: worker.weight 推导的基础消费数
    uint32_t n_base = worker->weight >= 0 ? (lens >> worker->weight) : 1;
    // task.priority 以 n_base/8 为单位加成:n = n_base * (1 + priority/8),
    // 每 +8 翻倍,每 +1 ≈ +12.5%;priority=0 快路径与历史等价
    atomic_t prio = ATOMIC_GET(&task->priority);
    uint32_t n = (0 == prio) ? n_base : n_base + (uint32_t)(((uint64_t)n_base * prio) >> 3);
    if (n > lens) {
        n = lens;
    }
    if (0 == n) {
        n = 1;
    }
    message_ctx *msg;
    uint32_t want, got, k, processed = 0;
#if ENABLE_DISPATCH_STAT
    uint64_t t0;
#endif
    ATOMIC64_SET(&version->handle, task->handle);
    // task->global CAS 保证同 task 同一时刻仅一个 worker 调度，qumsg 是单消费者，走 pop_sc_batch
    while (processed < n) {
        want = n - processed;
        if (want > TASK_MSG_BATCH) {
            want = TASK_MSG_BATCH;
        }
        got = fsqu_pop_sc_batch(&task->qumsg, msgbatch, want);
        if (0 == got) {
            break;
        }
        for (k = 0; k < got; k++) {
            msg = msgbatch[k];
            runarg->msg = *msg;
            pool_push(&loader->msg_pool, msg, 0);
            ATOMIC_ADD(&version->ver, 1);
            ATOMIC_SET(&version->msgtype, runarg->msg.mtype);
#if ENABLE_DISPATCH_STAT
            t0 = timer_thread_cpu_ns();
            task->_task_dispatch(runarg);
            task->dispatch_cpu_ns[runarg->msg.mtype] += timer_thread_cpu_ns() - t0;
            ++task->nmsg[runarg->msg.mtype];
#else
            task->_task_dispatch(runarg);
#endif
        }
        processed += got;
    }
    // worker 退出 dispatch 进入空闲，清 msgtype 让 monitor 区分"卡死"与"空闲"
    ATOMIC_SET(&version->msgtype, MSG_TYPE_NONE);
    // 无锁重调度：先将 global CAS 1→0（取消调度），再检查队列是否仍有消息。
    // 若有：尝试 CAS 0→1 重新调度；若 CAS 失败说明某生产者已抢先调度。
    // 两步均为 seq_cst 原子操作，保证不丢消息。
    ATOMIC_CAS(&task->global, 1, 0);
    if (fsqu_size(&task->qumsg) > 0) {
        if (ATOMIC_CAS(&task->global, 0, 1)) {
            _loader_worker_wakeup(loader, &task->handle);
        }
    }
}
// 工作线程主循环：持续从队列取任务并分发消息，队列空时阻塞等待唤醒
static void _loader_worker_loop(void *arg) {
    name_t handle;
    worker_ctx *worker = (worker_ctx *)arg;
    loader_ctx *loader = worker->loader;
    worker_version *version = &loader->monitor.version[worker->index];
    task_dispatch_arg runarg;
    message_ctx *msgbatch[TASK_MSG_BATCH];
    while (0 == ATOMIC_GET(&loader->stop)) {
        // 从队列取一任务
        handle = _loader_task_name_get(loader, worker);
        if (INVALID_TNAME != handle) {
            runarg.task = task_grab(loader, handle);
            if (NULL == runarg.task) {
                continue;
            }
            _loader_task_run(loader, worker, version, &runarg, msgbatch);
            task_ungrab(runarg.task);
            continue;
        }
        mutex_lock(&worker->mutex);
        ATOMIC_ADD(&worker->waiting, 1);
        // 丢失唤醒防护：先写 waiting，再检查队列。
        // 与生产者"先入队，再读 waiting"构成对称：
        //   若生产者在本线程写 waiting 之前读到 waiting==0 并跳过了 signal，
        //   则任务已在队列中，此处必然能看到非空，从而跳过 cond_wait。
        //   若生产者在本线程写 waiting 之后读 waiting，则会执行 signal，
        //   cond_wait 不会永久阻塞。
        if (fsqu_size(&worker->qutasks) > 0 || 0 != ATOMIC_GET(&loader->stop)) {
            ATOMIC_ADD(&worker->waiting, -1);
            mutex_unlock(&worker->mutex);
            continue;
        }
        cond_wait(&worker->cond, &worker->mutex);
        ATOMIC_ADD(&worker->waiting, -1);
        mutex_unlock(&worker->mutex);
    }
    LOG_INFO("worker thread %d exited.", worker->index);
}
// 检查各工作线程是否卡死（消息版本号未变化且仍有消息在处理）
static void _loader_monitor_check(loader_ctx *loader) {
    int32_t ver;
    worker_version *version;
    name_t handle;
    task_ctx *task;
    for (uint16_t i = 0; i < loader->nworker; i++) {
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
// 监控线程主循环：每 5 秒调用 _loader_monitor_check 检测卡死的工作线程
static void _loader_monitor_loop(void *arg) {
    loader_ctx *loader = (loader_ctx *)arg;
    timer_ctx timer;
    timer_init(&timer);
    uint64_t now, shrink_start = timer_cur_ms(&timer);
    while (0 == ATOMIC_GET(&loader->monitor.stop)) {
        mutex_lock(&loader->monitor.mutex);
        cond_timedwait(&loader->monitor.cond, &loader->monitor.mutex, 5000);
        mutex_unlock(&loader->monitor.mutex);
        if (0 != ATOMIC_GET(&loader->monitor.stop)) {
            break;
        }
        _loader_monitor_check(loader);
        // 空闲时按 SHRINK_TIME 门控回落消息池
        now = timer_cur_ms(&timer);
        if (now - shrink_start >= SHRINK_TIME) {
            shrink_start = now;
            pool_shrink(&loader->msg_pool, SHRINK_NKEEP(pool_size(&loader->msg_pool)), SHRINK_BUSY);
        }
    }
    LOG_INFO("%s", "worker monitor thread exited.");
}
loader_ctx *loader_init(uint16_t nnet, uint16_t nworker, uint32_t twcap) {
    loader_ctx *loader;
    CALLOC(loader, 1, sizeof(loader_ctx));
    prots_init(task_net_emit());
#if WITH_SSL
    evssl_init();
    evssl_pool_init();
#endif
    loader->nworker = 0 == nworker ? procscnt() : nworker;
    CALLOC(loader->worker, 1, sizeof(worker_ctx) * loader->nworker);
    CALLOC(loader->monitor.version, 1, sizeof(worker_version) * loader->nworker);
    mutex_init(&loader->monitor.mutex);
    cond_init(&loader->monitor.cond);
    pool_init(&loader->msg_pool, sizeof(message_ctx),
              (uint32_t)INIT_EVENTS_CNT * loader->nworker * 2, INIT_EVENTS_CNT, 1, NULL);
    rwlock_distr_init(&loader->lckmaptasks, (uint32_t)loader->nworker * 4);
#if WITH_LUA && ENABLE_LUA_BYTECACHE
    rwlock_distr_init(&loader->lckcache, (uint32_t)loader->nworker + 3);
#endif
    // worker 注册 lckmaptasks + lckcache;net / acpex / tw 只注册 lckmaptasks(不访问字节码缓存)
    const thread_hooks hooks_worker = {
        _loader_slot_register_worker, _loader_slot_unregister_worker, loader
    };
    const thread_hooks hooks_base = {
        _loader_slot_register_base, _loader_slot_unregister_base, loader
    };
    loader->maptasks = hashmap_new_with_allocator(_malloc, _realloc, _free,
                                                  sizeof(name_t *), ONEK, 0, 0,
                                                  _loader_task_hash, _loader_task_compare, _loader_task_free, NULL);
    loader->mapnames = hashmap_new_with_allocator(_malloc, _realloc, _free,
                                                  sizeof(name_handle_entry), ONEK, 0, 0,
                                                  _loader_name_hash, _loader_name_compare, NULL, NULL);
    loader->monitor.thread_monitor = thread_creat(_loader_monitor_loop, loader);
    // 每轮处理消息数 = lens >> weight（即 lens / 2^weight），等比递减：
    //   weight=-1: 1条        weight=2: lens/4
    //   weight=0:  全量       weight=3: lens/8
    //   weight=1:  lens/2
    // 32 槽分层：前 4 个保守、后 24 个激进，nworker ≥ 8 时避免后段退化循环。
    int32_t weights[] = {
        -1, -1, -1, -1, 0, 0, 0, 0,
        1, 1, 1, 1, 1, 1, 1, 1,
        2, 2, 2, 2, 2, 2, 2, 2,
        3, 3, 3, 3, 3, 3, 3, 3,
    };
    worker_ctx *worker;
    uint16_t i, wn = ARRAY_SIZE(weights);
    // 先把所有 worker 的字段初始化完(fsqu_init/mutex_init/cond_init/timer_init)
    // 再启动 worker 线程;否则 worker[0] 启动后会跨 worker 遍历 worker[1..N-1].qutasks
    // 而后者的 fsqu_init 还在主线程进行中(TSan 报 fsqu_size 的 race)
    for (i = 0; i < loader->nworker; i++) {
        worker = &loader->worker[i];
        worker->index = i;
        worker->weight = weights[i % wn];
        worker->loader = loader;
        fsqu_init(&worker->qutasks, sizeof(name_t), ONEK);
        mutex_init(&worker->mutex);
        cond_init(&worker->cond);
        timer_init(&worker->timer);
    }
    // pthread_create 的 release barrier 保证 worker 线程能看到第一轮循环的所有写入
    for (i = 0; i < loader->nworker; i++) {
        worker = &loader->worker[i];
        worker->thread_worker = thread_creat_hooks(_loader_worker_loop, hooks_worker.init, hooks_worker.exit, worker, hooks_worker.assist);
    }
    tw_init(&loader->tw, twcap, &hooks_base);
    ev_init(&loader->netev, nnet, &hooks_base);
    return loader;
}
#if WITH_LUA && ENABLE_LUA_BYTECACHE
rwlock_distr_ctx *loader_lckcache(loader_ctx *loader) {
    return &loader->lckcache;
}
#endif
// hashmap_scan 回调：向每个任务推送 MSG_TYPE_CLOSING 消息（仅推一次）
static bool _loader_closing_push(const void *item, void *udata) {
    task_ctx *task = UPCAST(*((name_t **)item), task_ctx, handle);
    if (ATOMIC_CAS(&task->closing, 0, 1)) {
        _task_message_push(task, udata);
    }
    return true;
}
// hashmap_scan 回调：打印仍未退出的任务警告（关闭超时时使用）
static bool _loader_closing_timeout(const void *item, void *udata) {
    (void)udata;
    task_ctx *task = UPCAST(*((name_t **)item), task_ctx, handle);
    LOG_WARN("task %s close timeout, ref %d.", _NAME_OR(task->name), ATOMIC_GET(&task->ref));
    return true;
}
// 广播关闭消息给所有任务，并等待所有任务退出（最长 15 秒超时告警）
static void _loader_task_closing(loader_ctx *loader) {
    message_ctx closing = { 0 };
    closing.mtype = MSG_TYPE_CLOSING;
    rwlock_distr_rdlock(&loader->lckmaptasks);
    // 在持锁期间置位 closing，与 task_register 的写锁互斥：
    // 若 task_register 先拿到写锁完成注册，本次扫描必然覆盖该新 task；
    // 若本扫描先拿到读锁并置位，task_register 随后在写锁内检测到 closing=1，
    // 会立即为新 task 追加 CLOSING 消息，无需业务代码感知。
    ATOMIC_SET(&loader->closing, 1);
    hashmap_scan(loader->maptasks, _loader_closing_push, &closing);
    rwlock_distr_runlock(&loader->lckmaptasks);
    size_t n;
    uint32_t time = 0;
    for (;;) {
        rwlock_distr_rdlock(&loader->lckmaptasks);
        n = hashmap_count(loader->maptasks);
        if (0 == n) {
            rwlock_distr_runlock(&loader->lckmaptasks);
            break;
        }
        if (time >= 15 * 1000) {
            time = 0;
            hashmap_scan(loader->maptasks, _loader_closing_timeout, NULL);
        }
        rwlock_distr_runlock(&loader->lckmaptasks);
        MSLEEP(50);
        time += 50;
    }
}
static bool _loader_task_each_scan(const void *item, void *udata) {
    _task_each_arg *w = (_task_each_arg *)udata;
    task_ctx *task = UPCAST(*((name_t **)item), task_ctx, handle);
    w->cb(task->name, task->handle, w->arg);
    return true;
}
void loader_task_each(loader_ctx *loader, task_each_cb cb, void *arg) {
    _task_each_arg wrap = { cb, arg };
    rwlock_distr_rdlock(&loader->lckmaptasks);
    hashmap_scan(loader->maptasks, _loader_task_each_scan, &wrap);
    rwlock_distr_runlock(&loader->lckmaptasks);
}
void loader_free(loader_ctx *loader) {
    _loader_task_closing(loader);
    ATOMIC_SET(&loader->stop, 1);
    worker_ctx *worker;
    _loader_worker_wakeup_all(loader);
    for (uint16_t i = 0; i < loader->nworker; i++) {
        worker = &loader->worker[i];
        thread_join(worker->thread_worker);
    }
    ATOMIC_SET(&loader->monitor.stop, 1);
    mutex_lock(&loader->monitor.mutex);
    cond_signal(&loader->monitor.cond);
    mutex_unlock(&loader->monitor.mutex);
    thread_join(loader->monitor.thread_monitor);
    mutex_free(&loader->monitor.mutex);
    cond_free(&loader->monitor.cond);
    ev_free(&loader->netev);
    tw_free(&loader->tw);
    for (uint16_t i = 0; i < loader->nworker; i++) {
        worker = &loader->worker[i];
        fsqu_free(&worker->qutasks);
        mutex_free(&worker->mutex);
        cond_free(&worker->cond);
    }
    hashmap_free(loader->maptasks);
    hashmap_free(loader->mapnames);
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
    pool_free(&loader->msg_pool);
    FREE(loader);
}
