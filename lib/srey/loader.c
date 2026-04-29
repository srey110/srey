#include "srey/loader.h"
#include "containers/hashmap.h"
#include "srey/task.h"

loader_ctx *g_loader; // 全局 loader 单例，由 loader_init 创建

// 计算任务名在哈希表中的哈希值
static uint64_t _map_task_hash(const void *item, uint64_t seed0, uint64_t seed1) {
    (void)seed0;
    (void)seed1;
    return hash((const char *)(*(name_t **)item), sizeof(name_t));
}
// 比较两个任务名（用于哈希表碰撞解决）
static int _map_task_compare(const void *a, const void *b, void *ud) {
    (void)ud;
    return *(*(name_t **)a) - *(*(name_t **)b);
}
// 哈希表元素析构回调：通过任务名指针反推 task_ctx 并释放
static void _map_task_free(void *item) {
    task_free(UPCAST(*((name_t **)item), task_ctx, name));
}
// 找出积压任务最多的 worker 索引，用于任务窃取；队列全空时返回 -1
static int32_t _max_task_index(loader_ctx *loader) {
    uint16_t index = 0;
    uint32_t max = mspc_size(&loader->worker[0].qutasks);
    uint32_t count;
    for (uint16_t i = 1; i < loader->nworker; i++) {
        count = mspc_size(&loader->worker[i].qutasks);
        if (count > max) {
            index = i;
            max = count;
        }
    }
    return 0 == max ? -1 : (int32_t)index;
}
// 唤醒所有处于等待状态的工作线程（用于 loader_free 时通知退出）
static void _worker_wakeup_all(loader_ctx *loader) {
    worker_ctx *worker;
    for (uint16_t i = 0; i < loader->nworker; i++) {
        worker = &loader->worker[i];
        if (ATOMIC_GET(&worker->waiting) > 0) {
            mutex_lock(&worker->mutex);
            cond_signal(&worker->cond);
            mutex_unlock(&worker->mutex);
        }
    }
}
// 将任务名投递到某个工作线程队列并在必要时唤醒该线程
static void _worker_wakeup(loader_ctx *loader, name_t *task) {
    // 单工作线程时跳过原子轮询直接选第 0 个
    worker_ctx *worker = (1 == loader->nworker)
        ? &loader->worker[0]
        : &loader->worker[ATOMIC64_ADD(&loader->index, 1) % loader->nworker];
    // 无锁入队；队列满时自旋等待（正常负载下极少发生）
    while (ERR_FAILED == mspc_push(&worker->qutasks, (void *)(uintptr_t)*task)) {
        CPU_PAUSE();
    }
    // 快速路径：无工作线程休眠时跳过 mutex
    if (ATOMIC_GET(&worker->waiting) > 0) {
        mutex_lock(&worker->mutex);
        cond_signal(&worker->cond);
        mutex_unlock(&worker->mutex);
    }
}
// 将消息推入任务的无锁消息队列并在必要时唤醒工作线程
void _task_message_push(task_ctx *task, message_ctx *msg) {
    // 堆分配 message_ctx 包装体，由消费方负责释放
    message_ctx *pmsg;
    MALLOC(pmsg, sizeof(message_ctx));
    *pmsg = *msg;
    // 无锁入队；队列满时自旋等待
    while (ERR_FAILED == mspc_push(&task->qumsg, pmsg)) {
        CPU_PAUSE();
    }
    // CAS 0→1：只有首个生产者负责调度，避免重复唤醒
    if (ATOMIC_CAS(&task->global, 0, 1)) {
        _worker_wakeup(task->loader, &task->name);
    }
}
// 从本地队列或其他 worker 队列（工作窃取）取出下一个待处理任务名
static name_t _task_name_get(loader_ctx *loader, worker_ctx *worker) {
    void *p = mspc_pop(&worker->qutasks);
    if (NULL != p) {
        return (name_t)(uintptr_t)p;
    }
    // 本地队列为空：尝试从积压最多的 worker 偷一个任务
    int32_t index = _max_task_index(loader);
    if (-1 != index) {
        p = mspc_pop(&loader->worker[index].qutasks);
        if (NULL != p) {
            return (name_t)(uintptr_t)p;
        }
    }
    return INVALID_TNAME;
}
// 单次批量 pop 消息的最大条数（栈上数组上限）
#define TASK_MSG_BATCH  64
// 从任务消息队列批量取出消息并依次分发，处理完成后重调度或清除调度标志
static void _task_run(loader_ctx *loader, worker_ctx *worker,
    worker_version *version, task_dispatch_arg *runarg) {
    task_ctx *task = runarg->task;
    uint32_t lens = mspc_size(&task->qumsg);
    if (lens > task->overload) {
        task->overload *= 2;
        LOG_WARN("task %d may overload, message queue length %d.", task->name, lens);
    }
    uint32_t n = worker->weight >= 0 ? (lens >> worker->weight) : 1;
    if (0 == n) {
        n = 1;
    }
    // 无锁批量 pop：逐条取出后复制到栈上数组，立即释放堆包装体，再批量 dispatch
    message_ctx *ptrs[TASK_MSG_BATCH];
    message_ctx  batch[TASK_MSG_BATCH];
    uint32_t want, got, i;
    uint32_t processed = 0;
    version->name = task->name;
    while (processed < n) {
        want = n - processed;
        if (want > TASK_MSG_BATCH) {
            want = TASK_MSG_BATCH;
        }
        got = 0;
        while (got < want) {
            message_ctx *p = (message_ctx *)mspc_pop(&task->qumsg);
            if (NULL == p) {
                break;
            }
            ptrs[got] = p;
            batch[got] = *p;
            got++;
        }
        if (0 == got) {
            break;
        }
        for (i = 0; i < got; i++) {
            FREE(ptrs[i]);
            runarg->msg = batch[i];
            ++version->ver;
            version->msgtype = runarg->msg.mtype;
            task->_task_dispatch(runarg);
            version->msgtype = MSG_TYPE_NONE;
        }
        processed += got;
        if (got < want) {
            break; // 队列已耗尽，提前退出
        }
    }
    // 无锁重调度：先将 global CAS 1→0（取消调度），再检查队列是否仍有消息。
    // 若有：尝试 CAS 0→1 重新调度；若 CAS 失败说明某生产者已抢先调度。
    // 两步均为 seq_cst 原子操作，保证不丢消息。
    ATOMIC_CAS(&task->global, 1, 0);
    if (mspc_size(&task->qumsg) > 0) {
        if (ATOMIC_CAS(&task->global, 0, 1)) {
            _worker_wakeup(loader, &task->name);
        }
    } else {
        task->overload = ONEK;
    }
}
// 工作线程主循环：持续从队列取任务并分发消息，队列空时阻塞等待唤醒
static void _worker_loop(void *arg) {
    name_t name;
    worker_ctx *worker = (worker_ctx *)arg;
    loader_ctx *loader = worker->loader;
    worker_version *version = &loader->monitor.version[worker->index];
    task_dispatch_arg runarg;
    while (0 == loader->stop) {
        // 从队列取一任务
        name = _task_name_get(loader, worker);
        if (INVALID_TNAME != name) {
            runarg.task = task_grab(loader, name);
            if (NULL == runarg.task) {
                continue;
            }
            _task_run(loader, worker, version, &runarg);
            task_ungrab(runarg.task);
            continue;
        }
        mutex_lock(&worker->mutex);
        ATOMIC_ADD(&worker->waiting, 1);
        // 在 mutex 下二次检查队列，防止丢失唤醒：
        // 若在 _task_name_get 返回空到 ATOMIC_ADD 之间有生产者入队，
        // 生产者看到 waiting==0 会跳过 signal，这里再检查一次补漏。
        // mspc_size 是无锁读，不需要额外锁保护。
        if (mspc_size(&worker->qutasks) > 0) {
            ATOMIC_ADD(&worker->waiting, (atomic_t)-1);
            mutex_unlock(&worker->mutex);
            continue;
        }
        cond_wait(&worker->cond, &worker->mutex);
        ATOMIC_ADD(&worker->waiting, (atomic_t)-1);
        mutex_unlock(&worker->mutex);
    }
    LOG_INFO("worker thread %d exited.", worker->index);
}
// 检查各工作线程是否卡死（消息版本号未变化且仍有消息在处理）
static void _monitor_check(loader_ctx *loader) {
    worker_version *version;
    for (uint16_t i = 0; i < loader->nworker; i++) {
        version = &loader->monitor.version[i];
        if (version->ckver == version->ver
            && MSG_TYPE_NONE != version->msgtype) {
            LOG_WARN("task: %d message type: %d, maybe in an endless loop.",
                version->name, version->msgtype);
        } else {
            version->ckver = version->ver;
        }
    }
}
// 监控线程主循环：每 5 秒调用 _monitor_check 检测卡死的工作线程
static void _monitor_loop(void *arg) {
    loader_ctx *loader = (loader_ctx *)arg;
    uint64_t time = 0;
    while (0 == loader->monitor.stop) {
        MSLEEP(100);
        time += 100;
        if (0 == time % 5000) {
            _monitor_check(loader);
        }
    }
    LOG_INFO("%s", "worker monitor thread exited.");
}
loader_ctx *loader_init(uint16_t nnet, uint16_t nworker, uint32_t twcap) {
    loader_ctx *loader;
    CALLOC(loader, 1, sizeof(loader_ctx));
    prots_init(_message_handshaked_push);
#if WITH_SSL
    evssl_init();
    evssl_pool_init();
#endif
    loader->nworker = 0 == nworker ? procscnt() : nworker;
    CALLOC(loader->worker, 1, sizeof(worker_ctx) * loader->nworker);
    CALLOC(loader->monitor.version, 1, sizeof(worker_version) * loader->nworker);
    rwlock_init(&loader->lckmaptasks);
    loader->maptasks = hashmap_new_with_allocator(_malloc, _realloc, _free,
                                                  sizeof(name_t *), ONEK, 0, 0,
                                                  _map_task_hash, _map_task_compare, _map_task_free, NULL);
    loader->monitor.thread_monitor = thread_creat(_monitor_loop, loader);
    int32_t weights[] = { -1, 0, 0, 1, 2, 3 };
    worker_ctx *worker;
    uint16_t n = ARRAY_SIZE(weights);
    for (uint16_t i = 0; i < loader->nworker; i++) {
        worker = &loader->worker[i];
        worker->index = i;
        worker->weight = weights[i % n];
        worker->loader = loader;
        mspc_init(&worker->qutasks, ONEK);
        mutex_init(&worker->mutex);
        cond_init(&worker->cond);
        worker->thread_worker = thread_creat(_worker_loop, worker);
    }
    tw_init(&loader->tw, twcap);
    ev_init(&loader->netev, nnet);
    return loader;
}
// hashmap_scan 回调：向每个任务推送 MSG_TYPE_CLOSING 消息（仅推一次）
static bool _closing_push(const void *item, void *udata) {
    task_ctx *task = UPCAST(*((name_t **)item), task_ctx, name);
    if (ATOMIC_CAS(&task->closing, 0, 1)) {
        _task_message_push(task, udata);
    }
    return true;
}
// hashmap_scan 回调：打印仍未退出的任务警告（关闭超时时使用）
static bool _closing_timeout(const void *item, void *udata) {
    (void)udata;
    task_ctx *task = UPCAST(*((name_t **)item), task_ctx, name);
    LOG_WARN("task %d close timeout, ref %d.", task->name, task->ref);
    return true;
}
// 广播关闭消息给所有任务，并等待所有任务退出（最长 15 秒超时告警）
static void _task_closing(loader_ctx *loader) {
    message_ctx closing;
    closing.mtype = MSG_TYPE_CLOSING;
    rwlock_rdlock(&loader->lckmaptasks);
    hashmap_scan(loader->maptasks, _closing_push, &closing);
    rwlock_unlock(&loader->lckmaptasks);
    size_t n;
    uint32_t time = 0;
    for (;;) {
        rwlock_rdlock(&loader->lckmaptasks);
        n = hashmap_count(loader->maptasks);
        if (0 == n) {
            rwlock_unlock(&loader->lckmaptasks);
            break;
        }
        if (time >= 15 * 1000) {
            time = 0;
            hashmap_scan(loader->maptasks, _closing_timeout, NULL);
        }
        rwlock_unlock(&loader->lckmaptasks);
        MSLEEP(50);
        time += 50;
    }
}
void loader_free(loader_ctx *loader) {
    _task_closing(loader);
    loader->stop = 1;
    worker_ctx *worker;
    _worker_wakeup_all(loader);
    for (uint16_t i = 0; i < loader->nworker; i++) {
        worker = &loader->worker[i];
        thread_join(worker->thread_worker);
    }
    loader->monitor.stop = 1;
    thread_join(loader->monitor.thread_monitor);
    ev_free(&loader->netev);
    tw_free(&loader->tw);
    for (uint16_t i = 0; i < loader->nworker; i++) {
        worker = &loader->worker[i];
        mspc_free(&worker->qutasks);
        mutex_free(&worker->mutex);
        cond_free(&worker->cond);
    }
    hashmap_free(loader->maptasks);
    rwlock_free(&loader->lckmaptasks);
    prots_free();
#if WITH_SSL
    evssl_pool_free();
#endif
    FREE(loader->worker);
    FREE(loader->monitor.version);
    FREE(loader);
}
