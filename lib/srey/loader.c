#include "srey/loader.h"
#include "containers/hashmap.h"
#include "srey/task.h"

loader_ctx *g_loader;

static uint64_t _map_task_hash(const void *item, uint64_t seed0, uint64_t seed1) {
    (void)seed0;
    (void)seed1;
    return hash((const char *)(*(name_t **)item), sizeof(name_t));
}
static int _map_task_compare(const void *a, const void *b, void *ud) {
    (void)ud;
    return *(*(name_t **)a) - *(*(name_t **)b);
}
static void _map_task_free(void *item) {
    task_free(UPCAST(*((name_t **)item), task_ctx, name));
}
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
static void _worker_wakeup(loader_ctx *loader, name_t *task) {
    /* nworker==1 fast path: skip atomic round-robin */
    worker_ctx *worker = (1 == loader->nworker)
        ? &loader->worker[0]
        : &loader->worker[ATOMIC64_ADD(&loader->index, 1) % loader->nworker];
    /* 无锁入队；队列满时自旋等待（正常负载下极少发生）*/
    while (ERR_FAILED == mspc_push(&worker->qutasks, (void *)(uintptr_t)*task)) {
        CPU_PAUSE();
    }
    /* Fast path: skip mutex when no worker is sleeping */
    if (ATOMIC_GET(&worker->waiting) > 0) {
        mutex_lock(&worker->mutex);
        cond_signal(&worker->cond);
        mutex_unlock(&worker->mutex);
    }
}
void _task_message_push(task_ctx *task, message_ctx *msg) {
    /* 堆分配 message_ctx 包装体，由消费方负责释放 */
    message_ctx *pmsg;
    MALLOC(pmsg, sizeof(message_ctx));
    *pmsg = *msg;
    /* 无锁入队；队列满时自旋等待 */
    while (ERR_FAILED == mspc_push(&task->qumsg, pmsg)) {
        CPU_PAUSE();
    }
    /* CAS 0→1：只有首个生产者负责调度，避免重复唤醒 */
    if (ATOMIC_CAS(&task->global, 0, 1)) {
        _worker_wakeup(task->loader, &task->name);
    }
}
static name_t _task_name_get(loader_ctx *loader, worker_ctx *worker) {
    void *p = mspc_pop(&worker->qutasks);
    if (NULL != p) {
        return (name_t)(uintptr_t)p;
    }
    /* 本地队列为空：尝试从积压最多的 worker 偷一个任务 */
    int32_t index = _max_task_index(loader);
    if (-1 != index) {
        p = mspc_pop(&loader->worker[index].qutasks);
        if (NULL != p) {
            return (name_t)(uintptr_t)p;
        }
    }
    return INVALID_TNAME;
}
/* 无锁批量 pop：单次最多取出的消息条数（栈上数组上限）*/
#define TASK_MSG_BATCH  64
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
    /* 无锁批量 pop：逐条取出后复制到栈上数组，立即释放堆包装体，再批量 dispatch */
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
            break; /* 队列已耗尽，提前退出 */
        }
    }
    /*
     * 无锁重调度：先将 global CAS 1→0（取消调度），再检查队列是否仍有消息。
     * 若有：尝试 CAS 0→1 重新调度；若 CAS 失败说明某生产者已抢先调度。
     * 这两步均为 seq_cst 原子操作，保证不丢消息。
     */
    ATOMIC_CAS(&task->global, 1, 0);
    if (mspc_size(&task->qumsg) > 0) {
        if (ATOMIC_CAS(&task->global, 0, 1)) {
            _worker_wakeup(loader, &task->name);
        }
    } else {
        task->overload = ONEK;
    }
}
static void _worker_loop(void *arg) {
    name_t name;
    worker_ctx *worker = (worker_ctx *)arg;
    loader_ctx *loader = worker->loader;
    worker_version *version = &loader->monitor.version[worker->index];
    task_dispatch_arg runarg;
    while (0 == loader->stop) {
        //从队列取一任务
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
        /* Re-check local queue under mutex to close the lost-wakeup window:
         * a push may have landed between _task_name_get returning empty and
         * our ATOMIC_ADD above; the pusher saw waiting==0 and skipped signal.
         * mspc_size 本身是无锁读，不需要额外锁保护。*/
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
//监控循环
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
loader_ctx *loader_init(uint16_t nnet, uint16_t nworker) {
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
    tw_init(&loader->tw);
    ev_init(&loader->netev, nnet);
    return loader;
}
static bool _closing_push(const void *item, void *udata) {
    task_ctx *task = UPCAST(*((name_t **)item), task_ctx, name);
    if (ATOMIC_CAS(&task->closing, 0, 1)) {
        _task_message_push(task, udata);
    }
    return true;
}
static bool _closing_timeout(const void *item, void *udata) {
    (void)udata;
    task_ctx *task = UPCAST(*((name_t **)item), task_ctx, name);
    LOG_WARN("task %d close timeout, ref %d.", task->name, task->ref);
    return true;
}
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
