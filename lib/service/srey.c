#include "service/srey.h"
#include "service/minicoro.h"
#include "service/maps.h"
#include "hashmap.h"
#include "sarray.h"
#include "spinlock.h"
#include "rwlock.h"
#include "queue.h"
#include "cond.h"
#include "tw.h"

typedef enum timeout_type {
    TMO_TYPE_SLEEP = 0x01,
    TMO_TYPE_NORMAL,
    TMO_TYPE_NET
}timeout_type;
#if WITH_SSL
typedef struct certs_ctx {
    uint32_t name;
    struct evssl_ctx *ssl;
}certs_ctx;
ARRAY_DECL(certs_ctx, arr_certs);
#endif
typedef struct worker_monitor {
    int8_t msgtype;
    uint32_t name;
    uint32_t ck_ver;
    atomic_t ver;
}worker_monitor;
typedef struct monitor_ctx {
    uint8_t stop;
    uint16_t adjinterval;
    uint16_t adjthreshold;
    worker_monitor *info;
    pthread_t thread;
}monitor_ctx;
typedef struct worker_ctx {
    uint8_t waiting;
    uint8_t adjusting;
    uint16_t index;
    uint16_t toindex;
    atomic_t cpu_cost;
    srey_ctx *srey;
    pthread_t thread;
    mutex_ctx mutex;
    cond_ctx cond;
    qu_void qutasks;
    timer_ctx timer;
}worker_ctx;
struct srey_ctx {
    uint8_t stop;
    uint8_t startup;
    uint16_t nworker;
    atomic_t index;
    worker_ctx *worker;
#if WITH_SSL
    arr_certs arrcert;
    rwlock_ctx lckarrcert;
#endif
    struct hashmap *maptask;
    rwlock_ctx lckmaptask;
    monitor_ctx monitor;
    tw_ctx tw;
    ev_ctx netev;
    mco_desc codesc;
};
QUEUE_DECL(mco_coro *, qu_copool);
QUEUE_DECL(message_ctx, qu_message);
struct task_ctx {
    uint8_t global;
    uint8_t closed;
    uint16_t index;
    uint16_t maxcnt;
    uint16_t maxmsgqulens;
    uint32_t name;
    uint32_t cpu_cost;
    atomic_t startup;
    task_run _run;
    task_free _free;
    void *handle;
    srey_ctx *srey;
    mco_coro *curco;
    mapco_ctx mapco;
    spin_ctx spin;
    qu_message qumsg;
    qu_copool qucopool;
};
typedef struct co_arg_ctx {
    task_ctx *task;
    message_ctx msg;
}co_arg_ctx;

#define CONNECT_TIMEOUT       3000
#define NETRD_TIMEOUT         3000
#define MSG_INIT_CAP          512
#define MSG_MAX_QULENS        ONEK
#define COPOOL_INIT_CAP       128
#define INVALID_INDEX         USHRT_MAX//无效的工作线程下标
#define CHECKVER_TIME         5000//检查死循环间隔
#define ADJINDEX_TIME         5000//检测时间间隔
#define ADJ_THRESHOLD         50//调整的阈值

#define CO_CREATE(arg)\
do {\
    arg->task->curco = _co_create(arg->task);\
    mco_result cortn = mco_push(arg->task->curco, arg, sizeof(co_arg_ctx));\
    ASSERTAB(MCO_SUCCESS == cortn, mco_result_description(cortn));\
    cortn = mco_resume(arg->task->curco);\
    ASSERTAB(MCO_SUCCESS == cortn, mco_result_description(cortn));\
} while (0)
#define CO_YIELD(task)\
do {\
    mco_result cortn = mco_yield(task->curco); \
    ASSERTAB(MCO_SUCCESS == cortn, mco_result_description(cortn));\
} while (0)
#define CO_RESUME(arg, co) \
do {\
    arg->task->curco = co; \
    mco_result cortn = mco_push(co, &arg->msg, sizeof(message_ctx)); \
    ASSERTAB(MCO_SUCCESS == cortn, mco_result_description(cortn)); \
    cortn = mco_resume(co); \
    ASSERTAB(MCO_SUCCESS == cortn, mco_result_description(cortn));\
} while (0)
#define CO_POP(co, msg)\
do {\
    mco_result cortn = mco_pop(co, &msg, sizeof(msg));\
    ASSERTAB(MCO_SUCCESS == cortn, mco_result_description(cortn));\
} while (0)

static void _task_free(task_ctx *task);
static inline uint64_t _maptask_hash(const void *item, uint64_t seed0, uint64_t seed1) {
    return hash((const char *)&((*(task_ctx **)item)->name), sizeof((*(task_ctx **)item)->name));
}
static inline int _maptask_compare(const void *a, const void *b, void *ud) {
    return (*(task_ctx **)a)->name - (*(task_ctx **)b)->name;
}
static void _maptask_free(void *item) {
    _task_free(*(task_ctx **)item);
}
static inline mco_coro *_co_create(task_ctx *task) {
    mco_result cortn;
    mco_coro **co = qu_copool_pop(&task->qucopool);
    if (NULL != co) {
        cortn = mco_uninit(*co);
        ASSERTAB(MCO_SUCCESS == cortn, mco_result_description(cortn));
        cortn = mco_init(*co, &task->srey->codesc);
        ASSERTAB(MCO_SUCCESS == cortn, mco_result_description(cortn));
        return *co;
    }
    mco_coro *conew;
    cortn = mco_create(&conew, &task->srey->codesc);
    ASSERTAB(MCO_SUCCESS == cortn, mco_result_description(cortn));
    return conew;
}
static inline void _message_clean(message_ctx *msg) {
    switch (msg->msgtype) {
    case MSG_TYPE_RECV:
    case MSG_TYPE_RECVFROM:
        protos_pkfree(msg->pktype, msg->data);
        break;
    case MSG_TYPE_REQUEST:
    case MSG_TYPE_RESPONSE:
        FREE(msg->data);
        break;
    default:
        break;
    }
}
static inline void _co_cb(mco_coro *co) {
    co_arg_ctx arg;
    CO_POP(co, arg);
    arg.task->_run(arg.task, &arg.msg);
    if (MSG_TYPE_CLOSING == arg.msg.msgtype) {
        arg.task->closed = 1;
    }
    qu_copool_push(&arg.task->qucopool, &co);
    _message_clean(&arg.msg);
}
static inline void _dispatch_timeout(co_arg_ctx *arg) {
    co_tmo_ctx cotmo;
    if (ERR_OK != _map_cotmo_get(&arg->task->mapco, arg->msg.sess, &cotmo)) {
        return;
    }
    switch (cotmo.type) {
    case TMO_TYPE_SLEEP:
        CO_RESUME(arg, cotmo.co);
        break;
    case TMO_TYPE_NORMAL:
        CO_CREATE(arg);
        break;
    case TMO_TYPE_NET: {
        co_sess_ctx cosess;
        if (ERR_OK == _map_cosess_get(&arg->task->mapco, arg->msg.sess, &cosess, 1)) {
            CO_RESUME(arg, cosess.co);
        }
        break;
    }
    default:
        break;
    }
}
static inline void _dispatch_connect(co_arg_ctx *arg) {
    if (0 != arg->msg.sess) {
        co_sess_ctx cosess;
        if (ERR_OK == _map_cosess_get(&arg->task->mapco, arg->msg.sess, &cosess, 1)) {
            _map_cotmo_del(&arg->task->mapco, arg->msg.sess);
            CO_RESUME(arg, cosess.co);
        } else {
            if (ERR_OK == arg->msg.erro) {
                ev_close(task_netev(arg->task), arg->msg.fd, arg->msg.skid);
            }
        }
    } else {
        CO_CREATE(arg);
    }
}
static inline void _dispatch_netrd(co_arg_ctx *arg) {
    if (0 != arg->msg.sess) {
        int32_t del;
        if (SLICE == arg->msg.slice
            || SLICE_START == arg->msg.slice) {
            del = 0;
        } else {
            del = 1;
        }
        co_sess_ctx cosess;
        if (ERR_OK == _map_cosess_get(&arg->task->mapco, arg->msg.sess, &cosess, del)) {
            if (SLICE_NONE == arg->msg.slice
                || SLICE_START == arg->msg.slice) {
                _map_cotmo_del(&arg->task->mapco, arg->msg.sess);
            }
            CO_RESUME(arg, cosess.co);
        }
        _message_clean(&arg->msg);
    } else {
        CO_CREATE(arg);
    }
}
static inline void _dispatch_close(co_arg_ctx *arg) {
    if (0 != arg->msg.sess) {
        co_sess_ctx cosess;
        if (ERR_OK == _map_cosess_get(&arg->task->mapco, arg->msg.sess, &cosess, 1)) {
            _map_cotmo_del(&arg->task->mapco, arg->msg.sess);
            CO_RESUME(arg, cosess.co);
        }
    }
    CO_CREATE(arg);
}
static inline void _dispatch_response(co_arg_ctx *arg) {
    co_sess_ctx cosess;
    if (ERR_OK == _map_cosess_get(&arg->task->mapco, arg->msg.sess, &cosess, 1)) {
        CO_RESUME(arg, cosess.co);
    } else {
        LOG_ERROR("task: %d, can't find session:%"PRIu64, arg->task->name, arg->msg.sess);
    }
    _message_clean(&arg->msg);
}
static inline void _dispatch_message(co_arg_ctx *arg) {
    if (0 != arg->task->closed) {
        _message_clean(&arg->msg);
        return;
    }
    switch (arg->msg.msgtype) {
    case MSG_TYPE_STARTED:
        CO_CREATE(arg);
        break;
    case MSG_TYPE_CLOSING:
        CO_CREATE(arg);
        break;
    case MSG_TYPE_TIMEOUT:
        _dispatch_timeout(arg);
        break;
    case MSG_TYPE_ACCEPT:
        CO_CREATE(arg);
        break;
    case MSG_TYPE_CONNECT:
        _dispatch_connect(arg);
        break;
    case MSG_TYPE_HANDSHAKED:
        CO_CREATE(arg);
        break;
    case MSG_TYPE_RECV:
        _dispatch_netrd(arg);
        break;
    case MSG_TYPE_SEND:
        CO_CREATE(arg);
        break;
    case MSG_TYPE_CLOSE:
        _dispatch_close(arg);
        break;
    case MSG_TYPE_RECVFROM:
        _dispatch_netrd(arg);
        break;
    case MSG_TYPE_REQUEST:
        CO_CREATE(arg);
        break;
    case MSG_TYPE_RESPONSE:
        _dispatch_response(arg);
        break;
    default:
        break;
    }
}
static inline void _wakeup_worker(worker_ctx *worker, task_ctx *task, int32_t sig) {
    mutex_lock(&worker->mutex);
    if (NULL != task) {
        qu_void_push(&worker->qutasks, (void **)&task);
    }
    if (worker->waiting > 0
        && 0 != sig) {
        cond_signal(&worker->cond);
    }
    mutex_unlock(&worker->mutex);
}
static inline void _push_message(task_ctx *task, message_ctx *msg) {
    int32_t add = 0;
    spin_lock(&task->spin);
    qu_message_push(&task->qumsg, msg);
    if (0 == task->global) {
        add = 1;
        task->global = 1;
    }
    spin_unlock(&task->spin);
    if (0 != add) {
        _wakeup_worker(&task->srey->worker[task->index], task, 1);
    }
}
static inline int32_t _task_dispatch_message(worker_ctx *worker, worker_monitor *monitor, co_arg_ctx *coarg) {
    spin_lock(&coarg->task->spin);
    message_ctx *tmp = qu_message_pop(&coarg->task->qumsg);
    if (NULL == tmp) {
        spin_unlock(&coarg->task->spin);
        return ERR_FAILED;
    }
    coarg->msg = *tmp;
    spin_unlock(&coarg->task->spin);

    ATOMIC_ADD(&monitor->ver, 1);
    monitor->msgtype = coarg->msg.msgtype;
    timer_start(&worker->timer);
    _dispatch_message(coarg);
    uint32_t elapsed = (uint32_t)(timer_elapsed(&worker->timer) / 1000);
    ATOMIC_ADD(&worker->cpu_cost, elapsed);
    coarg->task->cpu_cost += elapsed;
    monitor->msgtype = MSG_TYPE_NONE;
    return ERR_OK;
}
static inline void _task_run(worker_ctx *worker, worker_monitor *monitor, co_arg_ctx *coarg) {
    for (uint16_t i = 0; i < coarg->task->maxcnt; i++) {
        if (ERR_OK != _task_dispatch_message(worker, monitor, coarg)) {
            break;
        }
    }
}
static inline void _add_active_tasks(arr_void *arractive, task_ctx *task) {
    size_t n = arr_void_size(arractive);
    for (size_t i = 0; i < n; i++) {
        if (task->name == ((task_ctx *)*arr_void_at(arractive, i))->name) {
            return;
        }
    }
    arr_void_push_back(arractive, (void **)&task);
}
static inline void _clear_cpu_cost(arr_void *arractive) {
    size_t n = arr_void_size(arractive);
    for (size_t i = 0; i < n; i++) {
        ((task_ctx *)*arr_void_at(arractive, i))->cpu_cost = 0;
    }
}
static inline void _adjustment_taskto(arr_void *arractive, uint16_t toindex) {
    size_t i;
    size_t n = arr_void_size(arractive);
    if (n < 2) {
        if (1 == n) {
            ((task_ctx *)*arr_void_at(arractive, 0))->cpu_cost = 0;
        }
        return;
    }
    task_ctx *min_task = NULL, *max_task = NULL;
    size_t min_pos = 0, max_pos = 0;
    task_ctx *tmp;
    for (i = 0; i < n; i++) {
        tmp = *arr_void_at(arractive, i);
        LOG_DEBUG("thread %d task %d cpu_cost %d.", tmp->index, tmp->name, tmp->cpu_cost);
        if (0 == tmp->cpu_cost) {
            continue;
        }
        if (NULL == min_task) {
            min_task = tmp;
            max_task = tmp;
            min_pos = i;
            max_pos = i;
            continue;
        }
        if (tmp->cpu_cost < min_task->cpu_cost) {
            min_task->cpu_cost = 0;
            min_task = tmp;
            min_pos = i;
            continue;
        }
        if (tmp->cpu_cost > max_task->cpu_cost) {
            max_task->cpu_cost = 0;
            max_task = tmp;
            max_pos = i;
            continue;
        }
        tmp->cpu_cost = 0;
    }
    if (NULL == min_task) {
        return;
    }
    if (min_pos == max_pos) {
        min_task->cpu_cost = 0;
        return;
    } else {
        min_task->cpu_cost = 0;
        max_task->cpu_cost = 0;
    }
    LOG_DEBUG("adjustment task %d from thread %d to %d.", min_task->name, min_task->index, toindex);
    min_task->index = toindex;
}
static inline void _adjustment_load(srey_ctx *ctx, worker_ctx *worker, arr_void *arractive) {
    if (INVALID_INDEX != worker->toindex) {
        _adjustment_taskto(arractive, worker->toindex);
        worker->toindex = INVALID_INDEX;
    } else {
        _clear_cpu_cost(arractive);
    }
    arr_void_clear(arractive);
    worker->adjusting = 0;
    worker->cpu_cost = 0;
}
static void _loop_worker(void *arg) {
    void **tmp;
    worker_ctx *worker = (worker_ctx *)arg;
    srey_ctx *ctx = worker->srey;
    worker_monitor *monitor = &ctx->monitor.info[worker->index];
    int32_t add;
    arr_void arractive;
    arr_void_init(&arractive, 256);
    co_arg_ctx coarg;
    while (0 == ctx->stop) {
        //从队列取一任务
        mutex_lock(&worker->mutex);
        tmp = qu_void_pop(&worker->qutasks);
        if (NULL == tmp) {
            worker->waiting++;
            cond_wait(&worker->cond, &worker->mutex);
            worker->waiting--;
        } else {
            coarg.task = *tmp;
        }
        mutex_unlock(&worker->mutex);
        if (NULL == tmp) {
            if (ctx->nworker > 1
                && 0 != worker->adjusting) {
                //调整线程负载
                _adjustment_load(ctx, worker, &arractive);
            }
            continue;
        }
        if (worker->index != coarg.task->index) {
            LOG_DEBUG("different thread id,cur %d task %d, task name %d.", worker->index, coarg.task->index, coarg.task->name);
            _wakeup_worker(&ctx->worker[coarg.task->index], coarg.task, 1);
            continue;
        }
        //执行
        monitor->name = coarg.task->name;
        _task_run(worker, monitor, &coarg);
        if (ctx->nworker > 1) {
            _add_active_tasks(&arractive, coarg.task);
            if (0 != worker->adjusting) {
                //调整线程负载
                _adjustment_load(ctx, worker, &arractive);
            }
        }
        //加回队列
        spin_lock(&coarg.task->spin);
        if (0 == qu_message_size(&coarg.task->qumsg)) {
            add = 0;
            coarg.task->global = 0;
        } else {
            add = 1;
        }
        spin_unlock(&coarg.task->spin);
        if (0 != add) {
            _wakeup_worker(&ctx->worker[coarg.task->index], coarg.task, 0);
        }
    }
    arr_void_free(&arractive);
}
static inline void _monitor_check_ver(srey_ctx *ctx) {
    worker_monitor *m;
    for (uint16_t i = 0; i < ctx->nworker; i++) {
        m = &ctx->monitor.info[i];
        if (m->ck_ver == m->ver) {
            if (MSG_TYPE_NONE != m->msgtype) {
                LOG_ERROR("task: %d message type: %d, maybe in an endless loop. version: %d", m->name, m->msgtype, m->ver);
            }
        } else {
            m->ck_ver = m->ver;
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
    LOG_DEBUG("-----------------------------------------");
    LOG_DEBUG("thread %d cpu_cost %d.", 0, cpu_cost);
    uint32_t max_cpu_cost = cpu_cost, min_cpu_cost = cpu_cost;
    uint16_t max_index = 0, min_index = 0;
    for (uint16_t i = 1; i < ctx->nworker; i++) {
        cpu_cost = ctx->worker[i].cpu_cost;
        if (0 != ctx->worker[i].adjusting) {
            cpu_cost = 0;
        }
        LOG_DEBUG("thread %d cpu_cost %d.", i, cpu_cost);
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
        || (max_cpu_cost - min_cpu_cost) / 1000 < ctx->monitor.adjthreshold) {
        _set_adjusting(ctx);
        return;
    }
    LOG_DEBUG("prepare adjustment from thread %d to %d.", max_index, min_index);
    ctx->worker[max_index].toindex = min_index;
    _set_adjusting(ctx);
    _wakeup_worker(&ctx->worker[max_index], NULL, 1);
}
static void _loop_monitor(void *arg) {
    uint64_t time = 0;
    srey_ctx *ctx = (srey_ctx *)arg;
    while (0 == ctx->monitor.stop) {
        MSLEEP(100);
        time += 100;
        if (0 == time % CHECKVER_TIME) {
            _monitor_check_ver(ctx);
        }
        if (0 == time % ctx->monitor.adjinterval) {
            _monitor_worker(ctx);
        }
    }
}
srey_ctx *srey_init(uint16_t nnet, uint16_t nworker, uint16_t adjinterval, uint16_t adjthreshold) {
    srey_ctx *ctx;
    CALLOC(ctx, 1, sizeof(srey_ctx));
    ctx->nworker = nworker;
    ctx->monitor.adjinterval = 0 == adjinterval ? ADJINDEX_TIME : adjinterval;
    ctx->monitor.adjthreshold = 0 == adjthreshold ? ADJ_THRESHOLD : adjthreshold;
    CALLOC(ctx->worker, 1, sizeof(worker_ctx) * ctx->nworker);
    CALLOC(ctx->monitor.info, 1, sizeof(worker_monitor) * ctx->nworker);
    ctx->codesc = mco_desc_init(_co_cb, 0);
    rwlock_init(&ctx->lckmaptask);
    ctx->maptask = hashmap_new_with_allocator(_malloc, _realloc, _free,
                                              sizeof(task_ctx *), ONEK, 0, 0,
                                              _maptask_hash, _maptask_compare, _maptask_free, NULL);
#if WITH_SSL
    rwlock_init(&ctx->lckarrcert);
    arr_certs_init(&ctx->arrcert, 0);
#endif
    ctx->monitor.thread = thread_creat(_loop_monitor, ctx);
    worker_ctx *worker;
    for (uint16_t i = 0; i < ctx->nworker; i++) {
        worker = &ctx->worker[i];
        worker->index = i;
        worker->srey = ctx;
        worker->toindex = INVALID_INDEX;
        mutex_init(&worker->mutex);
        cond_init(&worker->cond);
        qu_void_init(&worker->qutasks, ONEK);
        timer_init(&worker->timer);
        worker->thread = thread_creat(_loop_worker, worker);
    }
    tw_init(&ctx->tw);
    ev_init(&ctx->netev, nnet);
    return ctx;
}
static inline bool _map_scan(const void *item, void *udata) {
    task_ctx *task = *(task_ctx **)item;
    if (!ATOMIC_CAS(&task->startup, 0, 1)) {
        return true;
    }
    message_ctx *msg = udata;
    _push_message(task, msg);
    return true;
}
void srey_startup(srey_ctx *ctx) {
    message_ctx msg;
    msg.msgtype = MSG_TYPE_STARTED;
    rwlock_rdlock(&ctx->lckmaptask);
    ctx->startup = 1;
    hashmap_scan(ctx->maptask, _map_scan, &msg);
    rwlock_unlock(&ctx->lckmaptask);
}
static inline bool _push_closing(const void *item, void *udata) {
    _push_message(*(task_ctx **)item, udata);
    return true;
}
static inline bool _check_closing(const void *item, void *udata) {
    if (0 == (*(task_ctx **)item)->closed) {
        *((int32_t *)udata) = 1;
        return false;
    }
    return true;
}
static void _task_closing(srey_ctx *ctx) {
    message_ctx closing;
    closing.msgtype = MSG_TYPE_CLOSING;
    rwlock_rdlock(&ctx->lckmaptask);
    hashmap_scan(ctx->maptask, _push_closing, &closing);
    rwlock_unlock(&ctx->lckmaptask);
    int32_t closed;
    do {
        closed = 0;
        rwlock_rdlock(&ctx->lckmaptask);
        hashmap_scan(ctx->maptask, _check_closing, &closed);
        rwlock_unlock(&ctx->lckmaptask);
        if (0 != closed) {
            MSLEEP(50);
        }
    } while (0 != closed);
}
void srey_free(srey_ctx *ctx) {
    uint16_t i;
    _task_closing(ctx);
    ctx->stop = 1;
    worker_ctx *worker;
    for (i = 0; i < ctx->nworker; i++) {
        worker = &ctx->worker[i];
        _wakeup_worker(worker, NULL, 1);
        thread_join(worker->thread);
    }
    ctx->monitor.stop = 1;
    thread_join(ctx->monitor.thread);
    ev_free(&ctx->netev);
    tw_free(&ctx->tw);
    hashmap_free(ctx->maptask);
    rwlock_free(&ctx->lckmaptask);
#if WITH_SSL
    certs_ctx *cert;
    size_t n = arr_certs_size(&ctx->arrcert);
    for (size_t i = 0; i < n; i++) {
        cert = arr_certs_at(&ctx->arrcert, i);
        evssl_free(cert->ssl);
    }
    arr_certs_free(&ctx->arrcert);
    rwlock_free(&ctx->lckarrcert);
#endif
    for (i = 0; i < ctx->nworker; i++) {
        worker = &ctx->worker[i];
        mutex_free(&worker->mutex);
        cond_free(&worker->cond);
        qu_void_free(&worker->qutasks);
    }
    FREE(ctx->worker);
    FREE(ctx->monitor.info);
    FREE(ctx);
}
static void _task_free(task_ctx *task) {
    if (NULL != task->_free) {
        task->_free(task);
    }
    message_ctx *msg;
    while (NULL != (msg = qu_message_pop(&task->qumsg))) {
        _message_clean(msg);
    }
    qu_message_free(&task->qumsg);
    mco_coro **co;
    while (NULL != (co = qu_copool_pop(&task->qucopool))) {
        mco_destroy(*co);
    }
    qu_copool_free(&task->qucopool);
    _map_co_free(&task->mapco);
    spin_free(&task->spin);
    FREE(task);
}
task_ctx *srey_tasknew(srey_ctx *ctx, uint32_t name, uint16_t maxcnt, uint16_t maxmsgqulens,
    task_new _init, task_run _run, task_free _tfree, void *arg) {
    if (NULL == _run) {
        LOG_WARN("task %d, %s", name, ERRSTR_INVPARAM);
        return NULL;
    }
    task_ctx *task;
    CALLOC(task, 1, sizeof(task_ctx));
    task->index = (uint16_t)(ATOMIC_ADD(&ctx->index, 1) % ctx->nworker);
    task->name = name;
    task->maxcnt = maxcnt;
    task->maxmsgqulens = 0 == maxmsgqulens ? MSG_MAX_QULENS : maxmsgqulens;
    task->_run = _run;
    task->_free = _tfree;
    task->srey = ctx;
    spin_init(&task->spin, SPIN_CNT_TASKMSG);
    _map_co_init(&task->mapco);
    qu_message_init(&task->qumsg, MSG_INIT_CAP);
    qu_copool_init(&task->qucopool, COPOOL_INIT_CAP);
    if (NULL != _init) {
        task->handle = _init(task, arg);
        if (NULL == task->handle) {
            _task_free(task);
            return NULL;
        }
    }
    uint8_t started = 0;
    rwlock_wrlock(&ctx->lckmaptask);
    if (NULL != hashmap_get(ctx->maptask, &task)) {
        rwlock_unlock(&ctx->lckmaptask);
        _task_free(task);
        LOG_ERROR("task %d repeat.", name);
        return NULL;
    }
    hashmap_set(ctx->maptask, &task);
    started = ctx->startup;
    rwlock_unlock(&ctx->lckmaptask);
    if (0 != started) {
        if (ATOMIC_CAS(&task->startup, 0, 1)) {
            message_ctx msg;
            msg.msgtype = MSG_TYPE_STARTED;
            _push_message(task, &msg);
        }
    }
    return task;
}
task_ctx *srey_taskqury(srey_ctx *ctx, uint32_t name) {
    task_ctx key;
    key.name = name;
    task_ctx *pkey = &key;
    rwlock_rdlock(&ctx->lckmaptask);
    void **tmp = (void **)hashmap_get(ctx->maptask, &pkey);
    task_ctx *task = (NULL == tmp ? NULL : *tmp);
    rwlock_unlock(&ctx->lckmaptask);
    return task;
}
ev_ctx *srey_netev(srey_ctx *ctx) {
    return &ctx->netev;
}
srey_ctx *task_srey(task_ctx *task) {
    return task->srey;
}
ev_ctx *task_netev(task_ctx *task) {
    return &task->srey->netev;
}
void *task_handle(task_ctx *task) {
    return task->handle;
}
uint32_t task_name(task_ctx *task) {
    return task->name;
}
#if WITH_SSL
static inline certs_ctx *_certs_get(srey_ctx *ctx, uint32_t name) {
    certs_ctx *cert;
    size_t n = arr_certs_size(&ctx->arrcert);
    for (size_t i = 0; i < n; i++) {
        cert = arr_certs_at(&ctx->arrcert, i);
        if (name == cert->name) {
            return cert;
        }
    }
    return NULL;
}
int32_t certs_register(srey_ctx *ctx, uint32_t name, struct evssl_ctx *evssl) {
    certs_ctx cert;
    cert.name = name;
    cert.ssl = evssl;
    int32_t rtn;
    rwlock_wrlock(&ctx->lckarrcert);
    if (NULL != _certs_get(ctx, name)) {
        LOG_ERROR("ssl name %d repeat.", name);
        rtn = ERR_FAILED;
    } else {
        arr_certs_push_back(&ctx->arrcert, &cert);
        rtn = ERR_OK;
    }
    rwlock_unlock(&ctx->lckarrcert);
    return rtn;
}
struct evssl_ctx *certs_qury(srey_ctx *ctx, uint32_t name) {
    certs_ctx *cert;
    rwlock_rdlock(&ctx->lckarrcert);
    cert = _certs_get(ctx, name);
    rwlock_unlock(&ctx->lckarrcert);
    return NULL == cert ? NULL : cert->ssl;
}
#endif
void _push_handshaked(SOCKET fd, uint64_t skid, ud_cxt *ud) {
    message_ctx msg;
    msg.msgtype = MSG_TYPE_HANDSHAKED;
    msg.pktype = ud->pktype;
    msg.fd = fd;
    msg.skid = skid;
    _push_message(ud->data, &msg);
}
static inline void _srey_timeout(ud_cxt *ud) {
    message_ctx msg;
    msg.msgtype = MSG_TYPE_TIMEOUT;
    msg.sess = ud->sess;
    _push_message(ud->data, &msg);
}
static inline void _task_timeout(task_ctx *task, uint64_t sess, uint32_t ms, uint32_t type) {
    ud_cxt ud;
    ud.data = task;
    ud.sess = sess;
    _map_cotmo_add(&task->mapco, type, task->curco, sess);
    tw_add(&task->srey->tw, ms, _srey_timeout, &ud);
    if (TMO_TYPE_SLEEP == type) {
        CO_YIELD(task);
        message_ctx msg;
        CO_POP(task->curco, msg);
        if (sess != msg.sess) {
            LOG_ERROR("task: %d, request session: %"PRIu64", response session: %"PRIu64" not the same.", task->name, sess, msg.sess);
            return;
        }
        if (MSG_TYPE_TIMEOUT != msg.msgtype) {
            LOG_ERROR("task: %d, message type: %d error, session: %"PRIu64, task->name, msg.msgtype, sess);
        }
    }
}
void task_sleep(task_ctx *task, uint32_t ms) {
    _task_timeout(task, createid(), ms, TMO_TYPE_SLEEP);
}
void task_timeout(task_ctx *task, uint64_t sess, uint32_t ms) {
    _task_timeout(task, sess, ms, TMO_TYPE_NORMAL);
}
static inline void *_task_request(task_ctx *dst, task_ctx *src, void *data, size_t size, int32_t copy, size_t *lens) {
    message_ctx msg;
    msg.msgtype = MSG_TYPE_REQUEST;
    if (NULL == src) {
        msg.sess = 0;
    } else {
        msg.sess = createid();
        msg.src = src->name;
    }
    if (0 != copy) {
        MALLOC(msg.data, size);
        memcpy(msg.data, data, size);
    } else {
        msg.data = data;
    }
    msg.size = size;
    if (NULL != src) {
        _map_cosess_add(&src->mapco, src->curco, msg.sess);
    }
    _push_message(dst, &msg);
    if (NULL != src) {
        CO_YIELD(src);
        message_ctx resp;
        CO_POP(src->curco, resp);
        if (msg.sess != resp.sess) {
            LOG_ERROR("task: %d, request session: %"PRIu64", response session: %"PRIu64" not the same.", src->name, msg.sess, resp.sess);
            return NULL;
        }
        if (MSG_TYPE_RESPONSE != resp.msgtype) {
            LOG_ERROR("task: %d, message type: %d error, session: %"PRIu64, src->name, resp.msgtype, msg.sess);
            return NULL;
        }
        *lens = resp.size;
        return resp.data;
    }
    return NULL;
}
void task_call(task_ctx *dst, void *data, size_t size, int32_t copy) {
    _task_request(dst, NULL, data, size, copy, NULL);
}
void *task_request(task_ctx *dst, task_ctx *src, void *data, size_t size, int32_t copy, size_t *lens) {
    return _task_request(dst, src, data, size, copy, lens);
}
void task_response(task_ctx *dst, uint64_t sess, void *data, size_t size, int32_t copy) {
    message_ctx msg;
    msg.msgtype = MSG_TYPE_RESPONSE;
    msg.sess = sess;
    if (0 != copy) {
        MALLOC(msg.data, size);
        memcpy(msg.data, data, size);
    } else {
        msg.data = data;
    }
    msg.size = size;
    _push_message(dst, &msg);
}
void *task_slice(task_ctx *task, uint64_t sess, size_t *size, int32_t *end) {
    CO_YIELD(task);
    message_ctx msg;
    CO_POP(task->curco, msg);
    if (sess != msg.sess) {
        LOG_ERROR("task: %d, request session: %"PRIu64", response session: %"PRIu64" not the same.", task->name, sess, msg.sess);
        return NULL;
    }
    if (MSG_TYPE_CLOSE == msg.msgtype) {
        return NULL;
    }
    if (MSG_TYPE_RECV != msg.msgtype) {
        LOG_ERROR("task: %d, message type: %d error, session: %"PRIu64, task->name, msg.msgtype, sess);
        return NULL;
    }
    if (SLICE_END == msg.slice) {
        *end = 1;
    }
    *size = msg.size;
    return msg.data;
}
static inline int32_t _task_net_accept(ev_ctx *ev, SOCKET fd, uint64_t skid, ud_cxt *ud) {
    message_ctx msg;
    msg.msgtype = MSG_TYPE_ACCEPT;
    msg.pktype = ud->pktype;
    msg.fd = fd;
    msg.skid = skid;
    _push_message(ud->data, &msg);
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
static inline void _task_net_recv(ev_ctx *ev, SOCKET fd, uint64_t skid, buffer_ctx *buf, size_t size, ud_cxt *ud) {
    message_ctx msg;
    msg.msgtype = MSG_TYPE_RECV;
    msg.pktype = ud->pktype;
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
            _push_message(ud->data, &msg);
        }
    } while (NULL != data && 0 != buffer_size(buf));
    if (0 != closefd) {
        ev_close(ev, fd, skid);
    }
}
static inline void _task_net_send(ev_ctx *ev, SOCKET fd, uint64_t skid, size_t size, ud_cxt *ud) {
    message_ctx msg;
    msg.msgtype = MSG_TYPE_SEND;
    msg.pktype = ud->pktype;
    msg.fd = fd;
    msg.skid = skid;
    msg.size = size;
    _push_message(ud->data, &msg);
}
static inline void _task_net_close(ev_ctx *ev, SOCKET fd, uint64_t skid, ud_cxt *ud) {
    message_ctx msg;
    msg.msgtype = MSG_TYPE_CLOSE;
    msg.pktype = ud->pktype;
    msg.fd = fd;
    msg.skid = skid;
    msg.sess = ud->sess;
    _push_message(ud->data, &msg);
}
int32_t task_netlisten(task_ctx *task, pack_type pktype, struct evssl_ctx *evssl,
    const char *ip, uint16_t port, int32_t sendev, uint64_t *id) {
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.pktype = pktype;
    ud.data = task;
    cbs_ctx cbs;
    ZERO(&cbs, sizeof(cbs));
    cbs.acp_cb = _task_net_accept;
    cbs.r_cb = _task_net_recv;
    cbs.c_cb = _task_net_close;
    if (0 != sendev) {
        cbs.s_cb = _task_net_send;
    }
    cbs.ud_free = protos_udfree;
    return ev_listen(&task->srey->netev, evssl, ip, port, &cbs, &ud, id);
}
static inline int32_t _task_net_connect(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t err, ud_cxt *ud) {
    message_ctx msg;
    msg.msgtype = MSG_TYPE_CONNECT;
    msg.pktype = ud->pktype;
    msg.skid = skid;
    msg.fd = fd;
    msg.erro = (int8_t)err;
    msg.sess = skid;
    _push_message(ud->data, &msg);
    return ERR_OK;
}
SOCKET task_netconnect(task_ctx *task, pack_type pktype, struct evssl_ctx *evssl,
    const char *ip, uint16_t port, int32_t sendev, uint64_t *skid) {
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.pktype = pktype;
    ud.data = task;
    cbs_ctx cbs;
    ZERO(&cbs, sizeof(cbs));
    cbs.conn_cb = _task_net_connect;
    cbs.r_cb = _task_net_recv;
    cbs.c_cb = _task_net_close;
    if (0 != sendev) {
        cbs.s_cb = _task_net_send;
    }
    cbs.ud_free = protos_udfree;
    SOCKET fd = ev_connect(&task->srey->netev, evssl, ip, port, &cbs, &ud, skid);
    if (INVALID_SOCK == fd) {
        return INVALID_SOCK;
    }
    _map_cosess_add(&task->mapco, task->curco, *skid);
    _task_timeout(task, *skid, CONNECT_TIMEOUT, TMO_TYPE_NET);
    CO_YIELD(task);
    message_ctx msg;
    CO_POP(task->curco, msg);
    if (*skid != msg.sess) {
        LOG_ERROR("task: %d, request session: %"PRIu64", response session: %"PRIu64" not the same.", task->name, *skid, msg.sess);
        return INVALID_SOCK;
    }
    if (MSG_TYPE_TIMEOUT == msg.msgtype) {
        return INVALID_SOCK;
    }
    if (MSG_TYPE_CONNECT != msg.msgtype) {
        LOG_ERROR("task: %d, message type: %d error, session: %"PRIu64, task->name, msg.msgtype, *skid);
        return INVALID_SOCK;
    }
    if (ERR_OK != msg.erro) {
        return INVALID_SOCK;
    }
    return fd;
}
static inline void _task_net_recvfrom(ev_ctx *ev, SOCKET fd, uint64_t skid, char *buf, size_t size, netaddr_ctx *addr, ud_cxt *ud) {
    message_ctx msg;
    msg.msgtype = MSG_TYPE_RECVFROM;
    msg.fd = fd;
    msg.skid = skid;
    udp_msg_ctx *umsg;
    MALLOC(umsg, sizeof(udp_msg_ctx) + size);
    memcpy(&umsg->addr, addr, sizeof(netaddr_ctx));
    memcpy(umsg->data, buf, size);
    msg.data = umsg;
    msg.size = size;
    msg.sess = ud->sess;
    msg.slice = SLICE_NONE;
    ud->sess = 0;
    _push_message(ud->data, &msg);
}
SOCKET task_netudp(task_ctx *task, const char *ip, uint16_t port, uint64_t *skid) {
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.data = task;
    cbs_ctx cbs;
    ZERO(&cbs, sizeof(cbs));
    cbs.rf_cb = _task_net_recvfrom;
    cbs.ud_free = protos_udfree;
    return ev_udp(&task->srey->netev, ip, port, &cbs, &ud, skid);
}
void task_netsend(task_ctx *task, SOCKET fd, uint64_t skid,
    void *data, size_t len, pack_type pktype) {
    size_t size;
    void *pack = protos_pack(pktype, data, len, &size);
    ev_send(&task->srey->netev, fd, skid, pack, size, 0);
}
void *task_synsend(task_ctx *task, SOCKET fd, uint64_t skid,
    void *data, size_t len, size_t *size, pack_type pktype, uint64_t *sess) {
    *sess = createid();
    _map_cosess_add(&task->mapco, task->curco, *sess);
    _task_timeout(task, *sess, NETRD_TIMEOUT, TMO_TYPE_NET);
    ev_ud_sess(&task->srey->netev, fd, skid, *sess);
    task_netsend(task, fd, skid, data, len, pktype);
    CO_YIELD(task);
    message_ctx msg;
    CO_POP(task->curco, msg);
    if (*sess != msg.sess) {
        LOG_ERROR("task: %d, request session: %"PRIu64", response session: %"PRIu64" not the same.", task->name, *sess, msg.sess);
        return NULL;
    }
    if (MSG_TYPE_TIMEOUT == msg.msgtype) {
        return NULL;
    }
    if (MSG_TYPE_CLOSE == msg.msgtype) {
        return NULL;
    }
    if (MSG_TYPE_RECV != msg.msgtype) {
        LOG_ERROR("task: %d, message type: %d error, session: %"PRIu64, task->name, msg.msgtype, *sess);
        return NULL;
    }
    *size = msg.size;
    return msg.data;
}
void *task_synsendto(task_ctx *task, SOCKET fd, uint64_t skid,
    const char *ip, const uint16_t port, void *data, size_t len, size_t *size) {
    uint64_t sess = createid();
    _map_cosess_add(&task->mapco, task->curco, sess);
    _task_timeout(task, sess, NETRD_TIMEOUT, TMO_TYPE_NET);
    ev_ud_sess(&task->srey->netev, fd, skid, sess);
    if (ERR_OK != ev_sendto(&task->srey->netev, fd, skid, ip, port, data, len)) {
        ev_ud_sess(&task->srey->netev, fd, skid, 0);
        _map_cosess_del(&task->mapco, sess);
        _map_cotmo_del(&task->mapco, sess);
        return NULL;
    }
    CO_YIELD(task);
    message_ctx msg;
    CO_POP(task->curco, msg);
    if (sess != msg.sess) {
        LOG_ERROR("task: %d, request session: %"PRIu64", response session: %"PRIu64" not the same.", task->name, sess, msg.sess);
        return NULL;
    }
    if (MSG_TYPE_TIMEOUT == msg.msgtype) {
        return NULL;
    }
    if (MSG_TYPE_RECVFROM != msg.msgtype) {
        LOG_ERROR("task: %d, message type: %d error, session: %"PRIu64, task->name, msg.msgtype, sess);
        return NULL;
    }
    udp_msg_ctx *umsg = msg.data;
    *size = msg.size;
    return umsg->data;
}
