#include "srey/scheduler.h"
#include "ds/hashmap.h"
#include "srey/task.h"

scheduler_ctx *g_scheduler;
static uint64_t _map_task_hash(const void *item, uint64_t seed0, uint64_t seed1) {
    return hash((const char *)(*(name_t **)item), sizeof(name_t));
}
static int _map_task_compare(const void *a, const void *b, void *ud) {
    return *(*(name_t **)a) - *(*(name_t **)b);
}
static void _map_task_free(void *item) {
    task_free(UPCAST(*((name_t **)item), task_ctx, name));
}
#if !SCHEDULER_GLOBAL
static int32_t _max_task_index(scheduler_ctx *scheduler) {
    uint16_t index = 0;
    uint32_t max = qu_task_size(&scheduler->worker[0].qutasks);
    uint32_t count;
    for (uint16_t i = 1; i < scheduler->nworker; i++) {
        count = qu_task_size(&scheduler->worker[i].qutasks);
        if (count > max) {
            index = i;
            max = count;
        }
    }
    return 0 == max ? -1 : (int32_t)index;
}
#endif
static void _worker_wakeup_all(scheduler_ctx *scheduler) {
#if SCHEDULER_GLOBAL
    mutex_lock(&scheduler->mutex);
    if (scheduler->waiting > 0) {
        cond_broadcast(&scheduler->cond);
    }
    mutex_unlock(&scheduler->mutex);
#else
    worker_ctx *worker;
    for (uint16_t i = 0; i < scheduler->nworker; i++) {
        worker = &scheduler->worker[i];
        mutex_lock(&worker->mutex);
        if (worker->waiting > 0) {
            cond_signal(&worker->cond);
        }
        mutex_unlock(&worker->mutex);
    }
#endif
}
static void _worker_wakeup(scheduler_ctx *scheduler, name_t *task) {
#if SCHEDULER_GLOBAL
    spin_lock(&scheduler->lckglobal);
    qu_task_push(&scheduler->quglobal, task);
    spin_unlock(&scheduler->lckglobal);
    mutex_lock(&scheduler->mutex);
    if (scheduler->waiting > 0) {
        cond_signal(&scheduler->cond);
    }
    mutex_unlock(&scheduler->mutex);
#else
    worker_ctx *worker = &scheduler->worker[ATOMIC64_ADD(&scheduler->index, 1) % scheduler->nworker];
    spin_lock(&worker->lcktasks);
    qu_task_push(&worker->qutasks, task);
    spin_unlock(&worker->lcktasks);
    mutex_lock(&worker->mutex);
    if (worker->waiting > 0) {
        cond_signal(&worker->cond);
    }
    mutex_unlock(&worker->mutex);
#endif
}
void _task_message_push(task_ctx *task, message_ctx *msg) {
    int32_t add = 0;
    spin_lock(&task->lckmsg);
    qu_message_push(&task->qumsg, msg);
    if (0 == task->global) {
        add = 1;
        task->global = 1;
    }
    spin_unlock(&task->lckmsg);
    if (0 != add) {
        _worker_wakeup(task->scheduler, &task->name);
    }
}
static name_t _task_name_get(scheduler_ctx *scheduler, worker_ctx *worker) {
    name_t *ptr;
    name_t name = INVALID_TNAME;
#if SCHEDULER_GLOBAL
    spin_lock(&scheduler->lckglobal);
    ptr = qu_task_pop(&scheduler->quglobal);
    if (NULL != ptr) {
        name = *ptr;
    }
    spin_unlock(&scheduler->lckglobal);
#else
    spin_lock(&worker->lcktasks);
    ptr = qu_task_pop(&worker->qutasks);
    if (NULL != ptr) {
        name = *ptr;
    }
    spin_unlock(&worker->lcktasks);
    if (INVALID_TNAME == name) {
        int32_t index = _max_task_index(scheduler);
        if (-1 != index) {
            spin_lock(&scheduler->worker[index].lcktasks);
            ptr = qu_task_pop(&scheduler->worker[index].qutasks);
            if (NULL != ptr) {
                name = *ptr;
            }
            spin_unlock(&scheduler->worker[index].lcktasks);
        }
    }
#endif
    return name;
}
static int32_t _task_message_pop(task_ctx *task, message_ctx *msg) {
    message_ctx *tmp;
    spin_lock(&task->lckmsg);
    tmp = qu_message_pop(&task->qumsg);
    if (NULL == tmp) {
        spin_unlock(&task->lckmsg);
        return ERR_FAILED;
    }
    *msg = *tmp;
    spin_unlock(&task->lckmsg);
    return ERR_OK;
}
static void _task_run(scheduler_ctx *scheduler, worker_ctx *worker,
    worker_version *version, task_dispatch_arg *runarg) {
    //执行
    version->name = runarg->task->name;
    while (ERR_OK == _task_message_pop(runarg->task, &runarg->msg)) {
        ++version->ver;
        version->msgtype = runarg->msg.mtype;
        runarg->task->_task_dispatch(runarg);
        version->msgtype = MSG_TYPE_NONE;
    }
    //加回队列
    int32_t add = 1;
    spin_lock(&runarg->task->lckmsg);
    if (0 == qu_message_size(&runarg->task->qumsg)) {
        add = 0;
        runarg->task->global = 0;
    }
    spin_unlock(&runarg->task->lckmsg);
    if (0 != add) {
        _worker_wakeup(scheduler, &runarg->task->name);
    }
}
static void _worker_hang(scheduler_ctx *scheduler, worker_ctx *worker) {
#if SCHEDULER_GLOBAL
    mutex_lock(&scheduler->mutex);
    ++scheduler->waiting;
    cond_wait(&scheduler->cond, &scheduler->mutex);
    --scheduler->waiting;
    mutex_unlock(&scheduler->mutex);
#else
    mutex_lock(&worker->mutex);
    ++worker->waiting;
    cond_wait(&worker->cond, &worker->mutex);
    --worker->waiting;
    mutex_unlock(&worker->mutex);
#endif
}
static void _worker_loop(void *arg) {
    name_t name;
    worker_ctx *worker = (worker_ctx *)arg;
    scheduler_ctx *scheduler = worker->scheduler;
    worker_version *version = &scheduler->monitor.version[worker->index];
    task_dispatch_arg runarg;
    while (0 == scheduler->stop) {
        //从队列取一任务
        name = _task_name_get(scheduler, worker);
        if (INVALID_TNAME != name) {
            runarg.task = task_grab(scheduler, name);
            if (NULL == runarg.task) {
                continue;
            }
            _task_run(scheduler, worker, version, &runarg);
            task_ungrab(runarg.task);
            continue;
        }
        _worker_hang(scheduler, worker);
    }
    LOG_INFO("worker thread %d exited.", worker->index);
}
//检查死循环
static void _monitor_check(scheduler_ctx *scheduler) {
    worker_version *version;
    for (uint16_t i = 0; i < scheduler->nworker; i++) {
        version = &scheduler->monitor.version[i];
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
    scheduler_ctx *scheduler = (scheduler_ctx *)arg;
    uint64_t time = 0;
    while (0 == scheduler->monitor.stop) {
        MSLEEP(100);
        time += 100;
        if (0 == time % 5000) {
            _monitor_check(scheduler);
        }
    }
    LOG_INFO("%s", "worker monitor thread exited.");
}
scheduler_ctx *scheduler_init(uint16_t nnet, uint16_t nworker) {
    scheduler_ctx *scheduler;
    CALLOC(scheduler, 1, sizeof(scheduler_ctx));
    protos_init(_message_handshaked_push);
#if WITH_CORO
    _mcoro_init(0);
#endif
#if SCHEDULER_GLOBAL
    spin_init(&scheduler->lckglobal, SPIN_CNT_SCHEDULER);
    qu_task_init(&scheduler->quglobal, ONEK);
    mutex_init(&scheduler->mutex);
    cond_init(&scheduler->cond);
#endif
    scheduler->nworker = 0 == nworker ? 1 : nworker;
    CALLOC(scheduler->worker, 1, sizeof(worker_ctx) * scheduler->nworker);
    CALLOC(scheduler->monitor.version, 1, sizeof(worker_version) * scheduler->nworker);
    rwlock_init(&scheduler->lckmaptasks);
    scheduler->maptasks = hashmap_new_with_allocator(_malloc, _realloc, _free,
                                               sizeof(name_t *), ONEK, 0, 0,
                                               _map_task_hash, _map_task_compare, _map_task_free, NULL);
#if WITH_SSL
    rwlock_init(&scheduler->lckcerts);
    arr_certs_init(&scheduler->arrcerts, 0);
#endif
    scheduler->monitor.thread_monitor = thread_creat(_monitor_loop, scheduler);
    worker_ctx *worker;
    for (uint16_t i = 0; i < scheduler->nworker; i++) {
        worker = &scheduler->worker[i];
        worker->index = i;
        worker->scheduler = scheduler;
#if !SCHEDULER_GLOBAL
        spin_init(&worker->lcktasks, SPIN_CNT_SCHEDULER);
        qu_task_init(&worker->qutasks, ONEK);
        mutex_init(&worker->mutex);
        cond_init(&worker->cond);
#endif
        worker->thread_worker = thread_creat(_worker_loop, worker);
    }
    tw_init(&scheduler->tw);
    ev_init(&scheduler->netev, nnet);
    return scheduler;
}
static bool _closing_push(const void *item, void *udata) {
    task_ctx *task = UPCAST(*((name_t **)item), task_ctx, name);
    if (ATOMIC_CAS(&task->closing, 0, 1)) {
        _task_message_push(task, udata);
    }
    return true;
}
static bool _closing_timeout(const void *item, void *udata) {
    task_ctx *task = UPCAST(*((name_t **)item), task_ctx, name);
    LOG_WARN("task %d close timeout, ref %d.", task->name, task->ref);
    return true;
}
static void _task_closing(scheduler_ctx *scheduler) {
    message_ctx closing;
    closing.mtype = MSG_TYPE_CLOSING;
    rwlock_rdlock(&scheduler->lckmaptasks);
    hashmap_scan(scheduler->maptasks, _closing_push, &closing);
    rwlock_unlock(&scheduler->lckmaptasks);
    size_t n;
    uint32_t time = 0;
    for (;;) {
        rwlock_rdlock(&scheduler->lckmaptasks);
        n = hashmap_count(scheduler->maptasks);
        if (0 == n) {
            rwlock_unlock(&scheduler->lckmaptasks);
            break;
        }
        if (time >= 5 * 1000) {
            time = 0;
            hashmap_scan(scheduler->maptasks, _closing_timeout, NULL);
        }
        rwlock_unlock(&scheduler->lckmaptasks);
        MSLEEP(50);
        time += 50;
    }
}
void scheduler_free(scheduler_ctx *scheduler) {
    _task_closing(scheduler);
    scheduler->stop = 1;
    worker_ctx *worker;
    _worker_wakeup_all(scheduler);
    for (uint16_t i = 0; i < scheduler->nworker; i++) {
        worker = &scheduler->worker[i];
        thread_join(worker->thread_worker);
    }
    scheduler->monitor.stop = 1;
    thread_join(scheduler->monitor.thread_monitor);
    ev_free(&scheduler->netev);
    tw_free(&scheduler->tw);
#if WITH_SSL
    uint32_t n = arr_certs_size(&scheduler->arrcerts);
    for (uint32_t i = 0; i < n; i++) {
        evssl_free(arr_certs_at(&scheduler->arrcerts, i)->ssl);
    }
    arr_certs_free(&scheduler->arrcerts);
    rwlock_free(&scheduler->lckcerts);
#endif
#if SCHEDULER_GLOBAL
    spin_free(&scheduler->lckglobal);
    qu_task_free(&scheduler->quglobal);
    mutex_free(&scheduler->mutex);
    cond_free(&scheduler->cond);
#else
    for (uint16_t i = 0; i < scheduler->nworker; i++) {
        worker = &scheduler->worker[i];
        spin_free(&worker->lcktasks);
        qu_task_free(&worker->qutasks);
        mutex_free(&worker->mutex);
        cond_free(&worker->cond);
    }
#endif
    hashmap_free(scheduler->maptasks);
    rwlock_free(&scheduler->lckmaptasks);
    FREE(scheduler->worker);
    FREE(scheduler->monitor.version);
    FREE(scheduler);
}
