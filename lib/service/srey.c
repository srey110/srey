#include "service/srey.h"
#include "hashmap.h"

#define INVALID_INDEX         USHRT_MAX//无效的工作线程下标
typedef void(*_dispatch_func)(task_msg_arg *arg);
static _dispatch_func _disp_funcs[TTYPE_CNT] = { 0 };

static void _task_free(task_ctx *task);
static inline uint64_t _maptask_hash(const void *item, uint64_t seed0, uint64_t seed1) {
    return hash((const char *)(*(name_t **)item), sizeof(name_t));
}
static inline int _maptask_compare(const void *a, const void *b, void *ud) {
    return *(*(name_t **)a) - *(*(name_t **)b);
}
static inline void _maptask_set(struct hashmap *map, task_ctx *task) {
    name_t *key = &task->name;
    ASSERTAB(NULL == hashmap_set(map, &key), "task name repeat.");
}
static inline void *_maptask_del(struct hashmap *map, name_t name) {
    name_t *key = &name;
    return (void *)hashmap_delete(map, &key);
}
static inline task_ctx *_maptask_get(struct hashmap *map, name_t name) {
    name_t *key = &name;
    name_t **ptr =(name_t **)hashmap_get(map, &key);
    if (NULL == ptr) {
        return NULL;
    }
    return UPCAST(*ptr, task_ctx, name);
}
static void _maptask_free(void *item) {
    _task_free(UPCAST(*((name_t **)item), task_ctx, name));
}
void message_clean(msg_type mtype, pack_type pktype, void *data) {
    switch (mtype) {
    case MSG_TYPE_RECV:
    case MSG_TYPE_RECVFROM:
        protos_pkfree(pktype, data);
        break;
    case MSG_TYPE_REQUEST:
    case MSG_TYPE_RESPONSE:
        FREE(data);
        break;
    default:
        break;
    }
}
static inline void _worker_wakeup(worker_ctx *worker, name_t name, int32_t sig) {
    mutex_lock(&worker->mutex);
    if (INVALID_TNAME != name) {
        qu_task_push(&worker->qutasks, &name);
    }
    if (worker->waiting > 0
        && 0 != sig) {
        cond_signal(&worker->cond);
    }
    mutex_unlock(&worker->mutex);
}
static inline void _push_message(task_ctx *task, message_ctx *msg) {
    uint8_t add = 0;
    spin_lock(&task->spin_msg);
    if (MSG_TYPE_TIMEOUT == msg->mtype) {
        qu_message_push(&task->qutmo, msg);
    } else {
        qu_message_push(&task->qumsg, msg);
    }
    if (0 == task->global) {
        add = 1;
        task->global = 1;
    }
    spin_unlock(&task->spin_msg);
    if (0 != add) {
        _worker_wakeup(&task->srey->worker[task->index], task->name, 1);
    }
}
static inline void _add_active_tasks(arr_task *arractive, name_t name) {
    size_t n = arr_task_size(arractive);
    for (size_t i = 0; i < n; i++) {
        if (name == *arr_task_at(arractive, i)) {
            return;
        }
    }
    arr_task_push_back(arractive, &name);
}
static inline void _reset_cpu_cost(srey_ctx *ctx, arr_task *arractive) {
    size_t n = arr_task_size(arractive);
    task_ctx *task;
    for (size_t i = 0; i < n; i++) {
        task = srey_task_grab(ctx, *arr_task_at(arractive, i));
        if (NULL != task) {
#if WITH_CORO
            _coro_shrink(task->coro);
#endif
            task->cpu_cost = 0;
            srey_task_release(task);
        }
    }
}
static inline void _adjustment_task(srey_ctx *ctx, worker_ctx *worker, arr_task *arractive) {
    size_t n = arr_task_size(arractive);
    if (n < 2) {
        _reset_cpu_cost(ctx, arractive);
        return;
    }
    uint8_t onlyone = 1;
    task_ctx *min_task = NULL;
    task_ctx *task;
    for (size_t i = 0; i < n; i++) {
        task = srey_task_grab(ctx, *arr_task_at(arractive, i));
        if (NULL == task) {
            continue;
        }
#if WITH_CORO
        _coro_shrink(task->coro);
#endif
        if (0 == task->cpu_cost
            || task->index != worker->index) {//可能释放后重新加到其他线程
            srey_task_release(task);
            continue;
        }
#if RECORD_WORKER_LOAD
        LOG_INFO("thread %d task %d cpu cost %d.", worker->index, task->name, task->cpu_cost);
#endif
        if (NULL == min_task) {
            min_task = task;
            continue;
        }
        onlyone = 0;
        if (task->cpu_cost < min_task->cpu_cost) {
            min_task->cpu_cost = 0;
            srey_task_release(min_task);
            min_task = task;
            continue;
        }
        task->cpu_cost = 0;
        srey_task_release(task);
    }
    if (NULL == min_task) {
        return;
    }
    min_task->cpu_cost = 0;
    if (0 != onlyone) {
        srey_task_release(min_task);
        return;
    }
#if RECORD_WORKER_LOAD
    ATOMIC_ADD(&ctx->worker[task->index].ntask, -1);
    ATOMIC_ADD(&ctx->worker[worker->toindex].ntask, 1);
    LOG_INFO("move task %d from thread %d to thread %d.", min_task->name, min_task->index, worker->toindex);
#endif
    min_task->index = worker->toindex;
    srey_task_release(min_task);  
}
static inline void _adjustment(srey_ctx *ctx, worker_ctx *worker, arr_task *arractive) {
    if (INVALID_INDEX != worker->toindex) {
        _adjustment_task(ctx, worker, arractive);
        worker->toindex = INVALID_INDEX;
    } else {
        _reset_cpu_cost(ctx, arractive);
    }
    arr_task_clear(arractive);
    worker->adjusting = 0;
    worker->cpu_cost = 0;
}
static inline name_t _get_task_name(worker_ctx *worker) {
    name_t name = INVALID_TNAME;
    mutex_lock(&worker->mutex);
    name_t *ptr = qu_task_pop(&worker->qutasks);
    if (NULL == ptr) {
        worker->waiting++;
        cond_wait(&worker->cond, &worker->mutex);
        worker->waiting--;
    } else {
        name = *ptr;
    }
    mutex_unlock(&worker->mutex);
    return name;
}
static inline int32_t _get_tmo_message(task_ctx *task, message_ctx *msg) {
    message_ctx *tmp;
    spin_lock(&task->spin_msg);
    tmp = qu_message_pop(&task->qutmo);
    if (NULL == tmp) {
        spin_unlock(&task->spin_msg);
        return ERR_FAILED;
    }
    *msg = *tmp;
    spin_unlock(&task->spin_msg);
    return ERR_OK;
}
static inline int32_t _get_message(task_ctx *task, message_ctx *msg) {
    message_ctx *tmp;
    spin_lock(&task->spin_msg);
    tmp = qu_message_pop(&task->qumsg);
    if (NULL == tmp) {
        spin_unlock(&task->spin_msg);
        return ERR_FAILED;
    }
    *msg = *tmp;
    spin_unlock(&task->spin_msg);
    return ERR_OK;
}
static inline void _message_run(srey_ctx *ctx, worker_ctx *worker, worker_version *version, task_msg_arg *arg) {
    ATOMIC_ADD(&version->ver, 1);
    version->msgtype = arg->msg.mtype;
    if (ctx->nworker > 1) {
        timer_start(&worker->timer);
    }
    _disp_funcs[arg->task->ttype](arg);
    if (ctx->nworker > 1) {
        uint32_t elapsed = (uint32_t)(timer_elapsed(&worker->timer) / 1000);
        ATOMIC_ADD(&worker->cpu_cost, elapsed);
        arg->task->cpu_cost += elapsed;
    }
    version->msgtype = MSG_TYPE_NONE;
}
static inline void _dispatch_message(srey_ctx *ctx, worker_ctx *worker, worker_version *version, task_msg_arg *arg) {
    int32_t over;
    for (uint16_t i = 0; i < arg->task->maxcnt; i++) {
        over = 1;
        if (ERR_OK == _get_message(arg->task, &arg->msg)) {
            _message_run(ctx, worker, version, arg);
            over = 0;
        }
        if (ERR_OK == _get_tmo_message(arg->task, &arg->msg)) {
            _message_run(ctx, worker, version, arg);
            over = 0;
        }
        if (0 != over) {
            break;
        }
    }
}
static inline void _task_run(srey_ctx *ctx, worker_ctx *worker, worker_version *version,
    arr_task *arractive, task_msg_arg *msgarg) {
    if (worker->index != msgarg->task->index) {
        _worker_wakeup(&ctx->worker[msgarg->task->index], msgarg->task->name, 1);
        return;
    }
    //执行
    version->name = msgarg->task->name;
    _dispatch_message(ctx, worker, version, msgarg);
    if (ctx->nworker > 1) {
        _add_active_tasks(arractive, msgarg->task->name);
    }
    //加回队列
    uint8_t add;
    spin_lock(&msgarg->task->spin_msg);
    if (0 == qu_message_size(&msgarg->task->qumsg)
        &&0 == qu_message_size(&msgarg->task->qutmo)) {
        add = 0;
        msgarg->task->global = 0;
    } else {
        add = 1;
    }
    spin_unlock(&msgarg->task->spin_msg);
    if (0 != add) {
        _worker_wakeup(&ctx->worker[msgarg->task->index], msgarg->task->name, 0);
    }
}
static void _loop_worker(void *arg) {
    name_t name;
    worker_ctx *worker = (worker_ctx *)arg;
    srey_ctx *ctx = worker->srey;
    worker_version *version = &ctx->monitor.version[worker->index];
    task_msg_arg msgarg;
    arr_task arractive;
    arr_task_init(&arractive, ONEK);
    while (0 == ctx->stop) {
        //从队列取一任务
        name = _get_task_name(worker);
        if (ctx->nworker > 1
            && 0 != worker->adjusting) {
            //调整负载 协程池
            _adjustment(ctx, worker, &arractive);
        }
        msgarg.task = srey_task_grab(ctx, name);
        if (NULL == msgarg.task) {
            continue;
        }
        _task_run(ctx, worker, version, &arractive, &msgarg);
        srey_task_release(msgarg.task);
    }
    arr_task_free(&arractive);
}
static inline void _monitor_check_ver(srey_ctx *ctx) {
    worker_version *version;
    for (uint16_t i = 0; i < ctx->nworker; i++) {
        version = &ctx->monitor.version[i];
        if (version->ck_ver == version->ver
            && MSG_TYPE_NONE != version->msgtype) {
            LOG_ERROR("task: %d message type: %d, maybe in an endless loop. version: %d",
                version->name, version->msgtype, version->ver);
        } else {
            version->ck_ver = version->ver;
        }
    }
}
static inline void _set_adjusting(srey_ctx *ctx) {
    for (uint16_t i = 0; i < ctx->nworker; i++) {
        ctx->worker[i].adjusting = 1;
    }
}
static inline void _monitor_worker(srey_ctx *ctx) {
    if (ctx->nworker <= 1) {
        return;
    }
    uint32_t cpu_cost = ctx->worker[0].cpu_cost;
    if (0 != ctx->worker[0].adjusting) {//上次调整都还未执行,该线程空闲
        cpu_cost = 0;
    }
#if RECORD_WORKER_LOAD
    LOG_INFO("thread 0 cpu cost %d task number %d.", cpu_cost, ctx->worker[0].ntask);
#endif
    uint32_t max_cpu_cost = cpu_cost, min_cpu_cost = cpu_cost;
    uint16_t max_index = 0, min_index = 0;
    for (uint16_t i = 1; i < ctx->nworker; i++) {
        cpu_cost = ctx->worker[i].cpu_cost;
        if (0 != ctx->worker[i].adjusting) {
            cpu_cost = 0;
        }
#if RECORD_WORKER_LOAD
        LOG_INFO("thread %d cpu cost %d task number %d.", i, cpu_cost, ctx->worker[i].ntask);
#endif
        if (cpu_cost > max_cpu_cost) {
            max_cpu_cost = cpu_cost;
            max_index = i;
            continue;
        }
        if (cpu_cost < min_cpu_cost) {
            min_cpu_cost = cpu_cost;
            min_index = i;
        }
    }
    if (min_index == max_index
        || min_cpu_cost >= max_cpu_cost
        || (max_cpu_cost - min_cpu_cost) / 1000 < (uint32_t)ctx->monitor.threshold) {
        _set_adjusting(ctx);
        return;
    }
#if RECORD_WORKER_LOAD
    LOG_INFO("prepare move task from thread %d to thread %d.", max_index, min_index);
#endif
    ctx->worker[max_index].toindex = min_index;
    _set_adjusting(ctx);
    _worker_wakeup(&ctx->worker[max_index], INVALID_TNAME, 1);
}
static void _loop_monitor(void *arg) {
    srey_ctx *ctx = (srey_ctx *)arg;
    uint64_t time = 0;
    while (0 == ctx->monitor.stop) {
        MSLEEP(100);
        time += 100;
        if (0 == time % 5000) {
            _monitor_check_ver(ctx);
        }
        if (0 == time % ctx->monitor.interval) {
            _monitor_worker(ctx);
        }
    }
}
static inline int32_t _task_startup(srey_ctx *ctx, task_ctx *task, message_ctx *startup) {
    if (NULL != task->_init) {
        task->handle = task->_init(task, task->arg);
        if (NULL == task->handle) {
            LOG_ERROR("task %d init failed.", task->name);
            _task_free(task);
            return ERR_FAILED;
        }
    }
    rwlock_wrlock(&ctx->lcktasks);
    if (NULL != _maptask_get(ctx->maptasks, task->name)) {
        rwlock_unlock(&ctx->lcktasks);
        LOG_ERROR("task name %d repeat.", task->name);
        _task_free(task);
        return ERR_FAILED;
    }
    _maptask_set(ctx->maptasks, task);
    qu_message_push(&task->qumsg, startup);
    task->global = 1;
    rwlock_unlock(&ctx->lcktasks);
#if RECORD_WORKER_LOAD
    ATOMIC_ADD(&ctx->worker[task->index].ntask, 1);
#endif
    _worker_wakeup(&ctx->worker[task->index], task->name, 1);
    return ERR_OK;
}
static void _loop_initer(void *arg) {
    srey_ctx *ctx = (srey_ctx *)arg;
    initer_ctx *initer = &ctx->initer;
    task_ctx *totask;
    initer_msg *tmp;
    initer_msg initmsg;
    message_ctx startup;
    startup.mtype = MSG_TYPE_STARTUP;
    message_ctx wakeup;
    wakeup.mtype = MSG_TYPE_WAKEUP;
    while (0 == initer->stop) {
        mutex_lock(&initer->mutex);
        tmp = qu_initer_pop(&initer->qutask);
        if (NULL == tmp) {
            initer->waiting++;
            cond_wait(&initer->cond, &initer->mutex);
            initer->waiting--;
        } else {
            initmsg = *tmp;
        }
        mutex_unlock(&initer->mutex);
        if (NULL == tmp) {
            continue;
        }
        wakeup.erro = (int8_t)_task_startup(ctx, initmsg.task, &startup);
        totask = srey_task_grab(ctx, initmsg.src);
        if (NULL == totask) {
            continue;
        }
        wakeup.sess = initmsg.sess;
        _push_message(totask, &wakeup);
        srey_task_release(totask);
    }
}
static inline void _dispatch_default(task_msg_arg *arg) {
    arg->task->_run(arg->task, &arg->msg);
    message_clean(arg->msg.mtype, arg->msg.pktype, arg->msg.data);
    if (MSG_TYPE_CLOSING == arg->msg.mtype) {
        srey_task_release(arg->task);
    }
}
#if WITH_LUA
static inline void _dispatch_lua(task_msg_arg *arg) {
    arg->task->_run(arg->task, &arg->msg);
}
#endif
static void _disp_funcs_init(void) {
#if WITH_CORO
    _disp_funcs[TTYPE_C] = _dispatch_coro;
#else
    _disp_funcs[TTYPE_C] = _dispatch_default;
#endif
#if WITH_LUA
    _disp_funcs[TTYPE_LUA] = _dispatch_lua;
#endif
}
srey_ctx *srey_init(uint16_t nnet, uint16_t nworker, size_t stack_size, uint16_t interval, uint16_t threshold) {
    srey_ctx *ctx;
    CALLOC(ctx, 1, sizeof(srey_ctx));
#if WITH_CORO
    _coro_init_desc(stack_size);
#endif
    _disp_funcs_init();
    protos_init();
    ctx->nworker = 0 == nworker ? 1 : nworker;
    ctx->monitor.interval = 0 == interval ? 10000 : interval;
    ctx->monitor.threshold = 0 == threshold ? 100 : threshold;
    CALLOC(ctx->worker, 1, sizeof(worker_ctx) * ctx->nworker);
    CALLOC(ctx->monitor.version, 1, sizeof(worker_version) * ctx->nworker);
    rwlock_init(&ctx->lcktasks);
    ctx->maptasks = hashmap_new_with_allocator(_malloc, _realloc, _free,
                                              sizeof(name_t *), ONEK, 0, 0,
                                              _maptask_hash, _maptask_compare, _maptask_free, NULL);
#if WITH_SSL
    rwlock_init(&ctx->lckcerts);
    arr_certs_init(&ctx->arrcerts, 0);
#endif
    mutex_init(&ctx->initer.mutex);
    cond_init(&ctx->initer.cond);
    qu_initer_init(&ctx->initer.qutask, 0);
    ctx->initer.thread = thread_creat(_loop_initer, ctx);
    ctx->monitor.thread = thread_creat(_loop_monitor, ctx);
    worker_ctx *worker;
    for (uint16_t i = 0; i < ctx->nworker; i++) {
        worker = &ctx->worker[i];
        worker->index = i;
        worker->srey = ctx;
        worker->toindex = INVALID_INDEX;
        mutex_init(&worker->mutex);
        cond_init(&worker->cond);
        qu_task_init(&worker->qutasks, ONEK);
        timer_init(&worker->timer);
        worker->thread = thread_creat(_loop_worker, worker);
    }
    tw_init(&ctx->tw);
    ev_init(&ctx->netev, nnet);
    return ctx;
}
static bool _scan_closing(const void *item, void *udata) {
    task_ctx *task = UPCAST(*((name_t **)item), task_ctx, name);
    if (ATOMIC_CAS(&task->closing, 0, 1)) {
        _push_message(task, udata);
    }
    return true;
}
static bool _scan_timeout(const void *item, void *udata) {
    task_ctx *task = UPCAST(*((name_t **)item), task_ctx, name);
    LOG_WARN("task %d close timeout, ref %d.", task->name, task->ref);
    return true;
}
static void _task_closing(srey_ctx *ctx) {
    message_ctx closing;
    closing.mtype = MSG_TYPE_CLOSING;
    rwlock_rdlock(&ctx->lcktasks);
    hashmap_scan(ctx->maptasks, _scan_closing, &closing);
    rwlock_unlock(&ctx->lcktasks);
    size_t n;
    uint32_t time = 0;
    for (;;) {
        rwlock_rdlock(&ctx->lcktasks);
        n = hashmap_count(ctx->maptasks);
        if (0 == n) {
            rwlock_unlock(&ctx->lcktasks);
            break;
        }
        if (time >= 3000) {
            hashmap_scan(ctx->maptasks, _scan_timeout, NULL);
            rwlock_unlock(&ctx->lcktasks);
            break;
        }
        rwlock_unlock(&ctx->lcktasks);
        MSLEEP(50);
        time += 50;
    }
}
void srey_free(srey_ctx *ctx) {
    _task_closing(ctx);
    ctx->stop = 1;
    worker_ctx *worker;
    for (uint16_t i = 0; i < ctx->nworker; i++) {
        worker = &ctx->worker[i];
        _worker_wakeup(worker, INVALID_TNAME, 1);
        thread_join(worker->thread);
    }
    ctx->initer.stop = 1;
    mutex_lock(&ctx->initer.mutex);
    if (ctx->initer.waiting > 0) {
        cond_signal(&ctx->initer.cond);
    }
    mutex_unlock(&ctx->initer.mutex);
    ctx->monitor.stop = 1;
    thread_join(ctx->monitor.thread);
    ev_free(&ctx->netev);
    tw_free(&ctx->tw);
#if WITH_SSL
    size_t n = arr_certs_size(&ctx->arrcerts);
    for (size_t i = 0; i < n; i++) {
        evssl_free(arr_certs_at(&ctx->arrcerts, i)->ssl);
    }
    arr_certs_free(&ctx->arrcerts);
    rwlock_free(&ctx->lckcerts);
#endif
    for (uint16_t i = 0; i < ctx->nworker; i++) {
        worker = &ctx->worker[i];
        mutex_free(&worker->mutex);
        cond_free(&worker->cond);
        qu_task_free(&worker->qutasks);
    }
    initer_msg *msg;
    while (NULL != (msg = qu_initer_pop(&ctx->initer.qutask))) {
        _task_free(msg->task);
    }
    hashmap_free(ctx->maptasks);
    rwlock_free(&ctx->lcktasks);
    mutex_free(&ctx->initer.mutex);
    cond_free(&ctx->initer.cond);
    qu_initer_free(&ctx->initer.qutask);
    FREE(ctx->worker);
    FREE(ctx->monitor.version);
    FREE(ctx);
}
uint16_t srey_nworker(srey_ctx *ctx) {
    return ctx->nworker;
}
void srey_worker_load(srey_ctx *ctx, uint16_t index, uint32_t *ntask, uint32_t *cpu_cost) {
    if (index >= ctx->nworker) {
        *ntask = 0;
        *cpu_cost = 0;
    } else {
#if RECORD_WORKER_LOAD
        *ntask = ctx->worker[index].ntask;
#else
        *ntask = 0;
#endif
        *cpu_cost = ctx->worker[index].cpu_cost;
    }
}
#if WITH_SSL
static inline certs_ctx *_ssl_get(srey_ctx *ctx, name_t name) {
    certs_ctx *cert;
    size_t n = arr_certs_size(&ctx->arrcerts);
    for (size_t i = 0; i < n; i++) {
        cert = arr_certs_at(&ctx->arrcerts, i);
        if (name == cert->name) {
            return cert;
        }
    }
    return NULL;
}
int32_t srey_ssl_register(srey_ctx *ctx, name_t name, struct evssl_ctx *evssl) {
    if (NULL == evssl) {
        LOG_WARN("%s", ERRSTR_NULLP);
        return ERR_FAILED;
    }
    certs_ctx cert;
    cert.name = name;
    cert.ssl = evssl;
    int32_t rtn;
    rwlock_wrlock(&ctx->lckcerts);
    if (NULL != _ssl_get(ctx, name)) {
        LOG_ERROR("ssl name %d repeat.", name);
        rtn = ERR_FAILED;
    } else {
        arr_certs_push_back(&ctx->arrcerts, &cert);
        rtn = ERR_OK;
    }
    rwlock_unlock(&ctx->lckcerts);
    return rtn;
}
struct evssl_ctx *srey_ssl_qury(srey_ctx *ctx, name_t name) {
    certs_ctx *cert;
    rwlock_rdlock(&ctx->lckcerts);
    cert = _ssl_get(ctx, name);
    rwlock_unlock(&ctx->lckcerts);
    return NULL == cert ? NULL : cert->ssl;
}
#endif
static void _task_free(task_ctx *task) {
    if (NULL != task->_arg_free
        && NULL != task->arg) {
        task->_arg_free(task->arg);
    }
    if (NULL != task->_free
        && NULL != task->handle) {
        task->_free(task->handle);
    }
    message_ctx *msg;
    while (NULL != (msg = qu_message_pop(&task->qumsg))) {
        message_clean(msg->mtype, msg->pktype, msg->data);
    }
#if WITH_CORO
    _coro_free(task->coro);
#endif
    qu_message_free(&task->qumsg);
    spin_free(&task->spin_msg);
    qu_message_free(&task->qutmo);
    FREE(task);
}
int32_t srey_task_new(srey_ctx *ctx, task_type ttype, name_t name, uint16_t maxcnt, uint16_t maxmsgqulens,
    name_t src, uint64_t sess, task_new _init, task_run _run, free_cb _tfree, free_cb _argfree, void *arg) {
    if (INVALID_TNAME == name
        || NULL == _run
        || (INVALID_TNAME != src
            && 0 == sess)) {
        if (NULL != _argfree
            && NULL != arg) {
            _argfree(arg);
        }
        return ERR_FAILED;
    }
    task_ctx *task;
    CALLOC(task, 1, sizeof(task_ctx));
    task->index = (uint16_t)(ATOMIC_ADD(&ctx->index, 1) % ctx->nworker);
    task->ttype = (uint8_t)ttype;
    task->name = name;
    task->maxcnt = 0 == maxcnt ? 5 : maxcnt;
    task->ref = 1;
    task->maxmsgqulens = 0 == maxmsgqulens ? ONEK : maxmsgqulens;
    task->_init = _init;
    task->_run = _run;
    task->_free = _tfree;
    task->_arg_free = _argfree;
    task->arg = arg;
    task->srey = ctx;
#if WITH_CORO
    task->coro = _coro_new();
#endif
    spin_init(&task->spin_msg, SPIN_CNT_TASKMSG);
    qu_message_init(&task->qumsg, task->maxmsgqulens);
    qu_message_init(&task->qutmo, task->maxmsgqulens);
    initer_msg initer;
    initer.src = src;
    initer.task = task;
    initer.sess = sess;
    mutex_lock(&ctx->initer.mutex);
    qu_initer_push(&ctx->initer.qutask, &initer);
    if (ctx->initer.waiting > 0) {
        cond_signal(&ctx->initer.cond);
    }
    mutex_unlock(&ctx->initer.mutex);
    return ERR_OK;
}
task_ctx *srey_task_grab(srey_ctx *ctx, name_t name) {
    if (INVALID_TNAME == name) {
        return NULL;
    }
    rwlock_rdlock(&ctx->lcktasks);
    task_ctx *task = _maptask_get(ctx->maptasks, name);
    if (NULL != task) {
        ATOMIC_ADD(&task->ref, 1);
    }
    rwlock_unlock(&ctx->lcktasks);
    return task;
}
void srey_task_addref(task_ctx *task) {
    ATOMIC_ADD(&task->ref, 1);
}
void srey_task_release(task_ctx *task) {
    if (1 != ATOMIC_ADD(&task->ref, -1)) {
        return;
    }
    if (ATOMIC_CAS(&task->closing, 0, 1)) {
        message_ctx closing;
        closing.mtype = MSG_TYPE_CLOSING;
        ATOMIC_ADD(&task->ref, 1);//MSG_TYPE_CLOSING(_coro)消息有一次release
        _push_message(task, &closing);
    } else {
        void *ptr = NULL;
        rwlock_wrlock(&task->srey->lcktasks);
        if (0 == task->ref) {
            ptr = _maptask_del(task->srey->maptasks, task->name);
        }
        rwlock_unlock(&task->srey->lcktasks);
        if (NULL != ptr) {
#if RECORD_WORKER_LOAD
            ATOMIC_ADD(&task->srey->worker[task->index].ntask, -1);
#endif
            _task_free(task);
        }
    }
}
size_t srey_task_qusize(task_ctx *task) {
    return qu_message_size(&task->qumsg) + qu_message_size(&task->qutmo);
}
static inline void _srey_timeout(ud_cxt *ud) {
    task_ctx *task = srey_task_grab(ud->data, ud->name);
    if (NULL == task) {
        return;
    }
    message_ctx msg;
    msg.mtype = MSG_TYPE_TIMEOUT;
    msg.sess = ud->sess;
    _push_message(task, &msg);
    srey_task_release(task);
}
void srey_timeout(task_ctx *task, uint64_t sess, uint32_t ms) {
    ud_cxt ud;
    ud.name = task->name;
    ud.data = task->srey;
    ud.sess = sess;
    tw_add(&task->srey->tw, ms, _srey_timeout, &ud);
}
void srey_request(task_ctx *dst, task_ctx *src, uint64_t sess, void *data, size_t size, int32_t copy) {
    message_ctx msg;
    msg.mtype = MSG_TYPE_REQUEST;
    if (NULL != src) {
        msg.src = src->name;
        msg.sess = sess;
    } else {
        msg.src = INVALID_TNAME;
        msg.sess = 0;
    }
    if (0 != copy) {
        MALLOC(msg.data, size);
        memcpy(msg.data, data, size);
    } else {
        msg.data = data;
    }
    msg.size = size;
    _push_message(dst, &msg);
}
void srey_response(task_ctx *dst, uint64_t sess, int32_t erro, void *data, size_t size, int32_t copy) {
    message_ctx msg;
    msg.mtype = MSG_TYPE_RESPONSE;
    msg.sess = sess;
    msg.erro = (int8_t)erro;
    msg.size = size;
    if (NULL != data
        && 0 != size) {
        if (0 != copy) {
            MALLOC(msg.data, size);
            memcpy(msg.data, data, size);
        } else {
            msg.data = data;
        }
    } else {
        msg.data = NULL;
    }
    _push_message(dst, &msg);
}
void srey_call(task_ctx *dst, void *data, size_t size, int32_t copy) {
    srey_request(dst, NULL, 0, data, size, copy);
}
static inline int32_t _net_accept(ev_ctx *ev, SOCKET fd, uint64_t skid, ud_cxt *ud) {
    task_ctx *task = srey_task_grab(ud->data, ud->name);
    if (NULL == task) {
        return ERR_FAILED;
    }
    message_ctx msg;
    msg.mtype = MSG_TYPE_ACCEPT;
    msg.pktype = ud->pktype;
    msg.fd = fd;
    msg.skid = skid;
    _push_message(task, &msg);
    srey_task_release(task);
    return ERR_OK;
}
static inline void _set_sess_slice(message_ctx *msg, ud_cxt *ud, int32_t slice) {
    if (0 != ud->sess
        && SLICE_NONE == ud->slice) {
        msg->sess = ud->sess;
        if (SLICE_START == slice) {
            ud->slice = SLICE;
            msg->slice = SLICE_START;
        } else {
            ud->sess = 0;
            msg->slice = SLICE_NONE;
        }
    } else if (SLICE == ud->slice) {
        if (SLICE_NONE == slice) {
            msg->slice = SLICE_NONE;
            msg->sess = 0;
        } else if (SLICE_END == slice) {
            msg->slice = SLICE_END;
            msg->sess = ud->sess;
            ud->sess = 0;
            ud->slice = SLICE_NONE;
        } else {
            msg->slice = SLICE;
            msg->sess = ud->sess;
        }
    } else {
        msg->slice = SLICE_NONE;
        msg->sess = 0;
    }
}
static inline void _net_recv(ev_ctx *ev, SOCKET fd, uint64_t skid, buffer_ctx *buf, size_t size, ud_cxt *ud) {
    task_ctx *task = srey_task_grab(ud->data, ud->name);
    if (NULL == task) {
        ev_close(ev, fd, skid);
        return;
    }
    message_ctx msg;
    msg.mtype = MSG_TYPE_RECV;
    msg.pktype = ud->pktype;
    msg.client = ud->client;
    msg.fd = fd;
    msg.skid = skid;
    void *data;
    size_t lens;
    int32_t closefd = 0;
    int32_t slice;
    do {
        data = protos_unpack(ev, fd, skid, buf, &lens, ud, &closefd, &slice);
        if (NULL != data) {
            msg.data = data;
            msg.size = lens;
            _set_sess_slice(&msg, ud, slice);
            _push_message(task, &msg);
        }
    } while (NULL != data && 0 != buffer_size(buf));
    if (0 != closefd) {
        ev_close(ev, fd, skid);
    }
    srey_task_release(task);
}
static inline void _net_send(ev_ctx *ev, SOCKET fd, uint64_t skid, size_t size, ud_cxt *ud) {
    task_ctx *task = srey_task_grab(ud->data, ud->name);
    if (NULL == task) {
        ev_close(ev, fd, skid);
        return;
    }
    message_ctx msg;
    msg.mtype = MSG_TYPE_SEND;
    msg.pktype = ud->pktype;
    msg.client = ud->client;
    msg.fd = fd;
    msg.skid = skid;
    msg.sess = ud->sess;
    msg.size = size;
    _push_message(task, &msg);
    srey_task_release(task);
}
static inline void _net_close(ev_ctx *ev, SOCKET fd, uint64_t skid, ud_cxt *ud) {
    task_ctx *task = srey_task_grab(ud->data, ud->name);
    if (NULL == task) {
        return;
    }
    message_ctx msg;
    msg.mtype = MSG_TYPE_CLOSE;
    msg.pktype = ud->pktype;
    msg.fd = fd;
    msg.skid = skid;
    msg.sess = ud->sess;
    _push_message(task, &msg);
    srey_task_release(task);
}
int32_t srey_listen(task_ctx *task, pack_type pktype, struct evssl_ctx *evssl,
    const char *ip, uint16_t port, int32_t sendev, uint64_t *id) {
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.pktype = pktype;
    ud.name = task->name;
    ud.data = task->srey;
    cbs_ctx cbs;
    ZERO(&cbs, sizeof(cbs));
    cbs.acp_cb = _net_accept;
    cbs.r_cb = _net_recv;
    cbs.c_cb = _net_close;
    if (0 != sendev) {
        cbs.s_cb = _net_send;
    }
    cbs.ud_free = protos_udfree;
    return ev_listen(&task->srey->netev, evssl, ip, port, &cbs, &ud, id);
}
static inline int32_t _net_connect(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t err, ud_cxt *ud) {
    task_ctx *task = srey_task_grab(ud->data, ud->name);
    if (NULL == task) {
        return ERR_FAILED;
    }
    message_ctx msg;
    msg.mtype = MSG_TYPE_CONNECT;
    msg.pktype = ud->pktype;
    msg.skid = skid;
    msg.fd = fd;
    msg.erro = (int8_t)err;
    msg.sess = ud->sess;
    ud->sess = 0;
    _push_message(task, &msg);
    srey_task_release(task);
    return ERR_OK;
}
SOCKET srey_connect(task_ctx *task, uint64_t sess, pack_type pktype, struct evssl_ctx *evssl,
    const char *ip, uint16_t port, int32_t sendev, uint64_t *skid) {
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.pktype = pktype;
    ud.client = 1;
    ud.name = task->name;
    ud.data = task->srey;
    ud.sess = sess;
    cbs_ctx cbs;
    ZERO(&cbs, sizeof(cbs));
    cbs.conn_cb = _net_connect;
    cbs.r_cb = _net_recv;
    cbs.c_cb = _net_close;
    if (0 != sendev) {
        cbs.s_cb = _net_send;
    }
    cbs.ud_free = protos_udfree;
    return ev_connect(&task->srey->netev, evssl, ip, port, &cbs, &ud, skid);
}
static inline void _net_recvfrom(ev_ctx *ev, SOCKET fd, uint64_t skid, char *buf, size_t size, netaddr_ctx *addr, ud_cxt *ud) {
    task_ctx *task = srey_task_grab(ud->data, ud->name);
    if (NULL == task) {
        ev_close(ev, fd, skid);
        return;
    }
    message_ctx msg;
    msg.mtype = MSG_TYPE_RECVFROM;
    msg.fd = fd;
    msg.skid = skid;
    char *umsg;
    MALLOC(umsg, sizeof(netaddr_ctx) + size);
    memcpy(umsg, addr, sizeof(netaddr_ctx));
    memcpy(umsg + sizeof(netaddr_ctx), buf, size);
    msg.data = umsg;
    msg.size = size;
    msg.sess = ud->sess;
    msg.slice = SLICE_NONE;
    ud->sess = 0;
    _push_message(task, &msg);
    srey_task_release(task);
}
SOCKET srey_udp(task_ctx *task, const char *ip, uint16_t port, uint64_t *skid) {
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.name = task->name;
    ud.data = task->srey;
    cbs_ctx cbs;
    ZERO(&cbs, sizeof(cbs));
    cbs.rf_cb = _net_recvfrom;
    cbs.ud_free = protos_udfree;
    return ev_udp(&task->srey->netev, ip, port, &cbs, &ud, skid);
}
void push_handshaked(SOCKET fd, uint64_t skid, ud_cxt *ud, int32_t *closefd, int32_t erro) {
    task_ctx *task = srey_task_grab(ud->data, ud->name);
    if (NULL == task) {
        *closefd = 1;
        return;
    }
    message_ctx msg;
    msg.mtype = MSG_TYPE_HANDSHAKED;
    msg.pktype = ud->pktype;
    msg.client = ud->client;
    msg.fd = fd;
    msg.skid = skid;
    msg.sess = ud->sess;
    msg.erro = (int8_t)erro;
    _push_message(task, &msg);
    srey_task_release(task);
}
