#include "service/srey.h"
#include "ds/hashmap.h"

typedef struct ctask_tmo_arg {
    ctask_timeout _timeout;
    free_cb _argfree;
    void *arg;
}ctask_tmo_arg;

#define INVALID_INDEX         USHRT_MAX//无效的工作线程下标
typedef void(*_dispatch_func)(task_msg_arg *arg);
static _dispatch_func _disp_funcs[TTYPE_CNT] = { 0 };

static uint64_t _map_task_hash(const void *item, uint64_t seed0, uint64_t seed1) {
    return hash((const char *)(*(name_t **)item), sizeof(name_t));
}
static int _map_task_compare(const void *a, const void *b, void *ud) {
    return *(*(name_t **)a) - *(*(name_t **)b);
}
static void _map_task_set(struct hashmap *map, task_ctx *task) {
    name_t *key = &task->name;
    ASSERTAB(NULL == hashmap_set(map, &key), "task name repeat.");
}
static void *_map_task_del(struct hashmap *map, name_t name) {
    name_t *key = &name;
    return (void *)hashmap_delete(map, &key);
}
static task_ctx *_map_task_get(struct hashmap *map, name_t name) {
    name_t *key = &name;
    name_t **ptr = (name_t **)hashmap_get(map, &key);
    if (NULL == ptr) {
        return NULL;
    }
    return UPCAST(*ptr, task_ctx, name);
}
static void _map_task_free(void *item) {
    srey_task_free(UPCAST(*((name_t **)item), task_ctx, name));
}
void message_clean(task_ctx *task, msg_type mtype, pack_type pktype, void *data) {
    switch (mtype) {
    case MSG_TYPE_RECV:
    case MSG_TYPE_RECVFROM:
        protos_pkfree(pktype, data);
        break;
    case MSG_TYPE_REQUEST:
    case MSG_TYPE_RESPONSE:
        FREE(data);
        break;
    case MSG_TYPE_TIMEOUT:
        if (NULL != data) {
            ctask_tmo_arg *tmo = data;
            if (NULL != tmo->_argfree
                && NULL != tmo->arg) {
                tmo->_argfree(tmo->arg);
            }
            ZERO(tmo, sizeof(ctask_tmo_arg));
            qu_ptr_push(&task->qutmoarg, (void **)&tmo);
        }
        break;
    default:
        break;
    }
}
#if !SCHEDULER_GLOBAL
static uint16_t _min_task_index(srey_ctx *ctx) {
    uint32_t min = qu_task_size(&ctx->worker[0].qutasks);
    if (0 == min) {
        return 0;
    }
    uint16_t index = 0;
    uint32_t count;
    for (uint16_t i = 1; i < ctx->nworker; i++) {
        count = qu_task_size(&ctx->worker[i].qutasks);
        if (count < min) {
            index = i;
            if (0 == count) {
                break;
            }
            min = count;
        }
    }
    return index;
}
static int32_t _max_task_index(srey_ctx *ctx) {
    uint16_t index = 0;
    uint32_t max = qu_task_size(&ctx->worker[0].qutasks);
    uint32_t count;
    for (uint16_t i = 1; i < ctx->nworker; i++) {
        count = qu_task_size(&ctx->worker[i].qutasks);
        if (count > max) {
            index = i;
            max = count;
        }
    }
    return 0 == max ? -1 : (int32_t)index;
}
#endif
static void _worker_wakeup_all(srey_ctx *ctx) {
#if SCHEDULER_GLOBAL
    mutex_lock(&ctx->mutex);
    if (ctx->waiting > 0) {
        cond_broadcast(&ctx->cond);
    }
    mutex_unlock(&ctx->mutex);
#else
    worker_ctx *worker;
    for (uint16_t i = 0; i < ctx->nworker; i++) {
        worker = &ctx->worker[i];
        mutex_lock(&worker->mutex);
        if (worker->waiting > 0) {
            cond_signal(&worker->cond);
        }
        mutex_unlock(&worker->mutex);
    }
#endif
}
static void _worker_wakeup(srey_ctx *ctx, name_t *task) {
#if SCHEDULER_GLOBAL
    spin_lock(&ctx->lckglobal);
    qu_task_push(&ctx->quglobal, task);
    spin_unlock(&ctx->lckglobal);
    mutex_lock(&ctx->mutex);
    if (ctx->waiting > 0) {
        cond_signal(&ctx->cond);
    }
    mutex_unlock(&ctx->mutex);
#else
    uint16_t index = _min_task_index(ctx);
    worker_ctx *worker = &ctx->worker[index];
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
static void _task_message_push(task_ctx *task, message_ctx *msg) {
    int32_t add = 0;
    spin_lock(&task->lckmsg);
    qu_message_push(&task->qumsg, msg);
    if (0 == task->global) {
        add = 1;
        task->global = 1;
    }
    spin_unlock(&task->lckmsg);
    if (0 != add) {
        _worker_wakeup(task->srey, &task->name);
    }
}
static name_t _task_name_pop(srey_ctx *ctx, worker_ctx *worker) {
    name_t *ptr;
    name_t name = INVALID_TNAME;
#if SCHEDULER_GLOBAL
    spin_lock(&ctx->lckglobal);
    ptr = qu_task_pop(&ctx->quglobal);
    if (NULL != ptr) {
        name = *ptr;
    }
    spin_unlock(&ctx->lckglobal);
#else
    spin_lock(&worker->lcktasks);
    ptr = qu_task_pop(&worker->qutasks);
    if (NULL != ptr) {
        name = *ptr;
    }
    spin_unlock(&worker->lcktasks);
    if (INVALID_TNAME == name) {
        int32_t index = _max_task_index(ctx);
        if (-1 != index) {
            spin_lock(&ctx->worker[index].lcktasks);
            ptr = qu_task_pop(&ctx->worker[index].qutasks);
            if (NULL != ptr) {
                name = *ptr;
            }
            spin_unlock(&ctx->worker[index].lcktasks);
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
static void _task_message_dispatch(srey_ctx *ctx, worker_ctx *worker,
    worker_version *version, task_msg_arg *msgarg) {
    //执行
    version->name = msgarg->task->name;
    while (ERR_OK == _task_message_pop(msgarg->task, &msgarg->msg)) {
        ++version->ver;
        version->msgtype = msgarg->msg.mtype;
        _disp_funcs[msgarg->task->ttype](msgarg);
        version->msgtype = MSG_TYPE_NONE;
    }
    //加回队列
    int32_t add = 1;
    spin_lock(&msgarg->task->lckmsg);
    if (0 == qu_message_size(&msgarg->task->qumsg)) {
        add = 0;
        msgarg->task->global = 0;
    }
    spin_unlock(&msgarg->task->lckmsg);
    if (0 != add) {
        _worker_wakeup(ctx, &msgarg->task->name);
    }
}
static void _worker_hang(srey_ctx *ctx, worker_ctx *worker) {
#if SCHEDULER_GLOBAL
    mutex_lock(&ctx->mutex);
    ++ctx->waiting;
    cond_wait(&ctx->cond, &ctx->mutex);
    --ctx->waiting;
    mutex_unlock(&ctx->mutex);
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
    srey_ctx *ctx = worker->srey;
    worker_version *version = &ctx->monitor.version[worker->index];
    task_msg_arg msgarg;
    while (0 == ctx->stop) {
        //从队列取一任务
        name = _task_name_pop(ctx, worker);
        if (INVALID_TNAME != name) {
            msgarg.task = srey_task_grab(ctx, name);
            if (NULL == msgarg.task) {
                continue;
            }
            _task_message_dispatch(ctx, worker, version, &msgarg);
            srey_task_ungrab(msgarg.task);
            continue;
        }
        _worker_hang(ctx, worker);
    }
    LOG_INFO("worker thread %d exited.", worker->index);
}
//检查死循环
static void _monitor_check(srey_ctx *ctx) {
    worker_version *version;
    for (uint16_t i = 0; i < ctx->nworker; i++) {
        version = &ctx->monitor.version[i];
        if (version->ckver == version->ver
            && MSG_TYPE_NONE != version->msgtype) {
            LOG_ERROR("task: %d message type: %d, maybe in an endless loop.",
                version->name, version->msgtype);
        } else {
            version->ckver = version->ver;
        }
    }
}
static void _monitor_loop(void *arg) {
    srey_ctx *ctx = (srey_ctx *)arg;
    uint64_t time = 0;
    while (0 == ctx->monitor.stop) {
        MSLEEP(100);
        time += 100;
        if (0 == time % 5000) {
            _monitor_check(ctx);
        }
    }
    LOG_INFO("%s", "worker monitor thread exited.");
}
#if WITH_LUA
static void _lua_dispatch(task_msg_arg *arg) {
    ASSERTAB(NULL != arg->task->_run[arg->msg.mtype], "lua task not set callback functhion.");
    arg->task->_run[arg->msg.mtype](arg->task, &arg->msg);
}
#endif
srey_ctx *srey_init(uint16_t nnet, uint16_t nworker, size_t stack_size, const char *key) {
    size_t klens = strlen(key);
    if (klens >= SIGN_KEY_LENS) {
        LOG_ERROR("sign key too long.");
        return NULL;
    }
    srey_ctx *ctx;
    CALLOC(ctx, 1, sizeof(srey_ctx));
    strcpy(ctx->key, key);
    _coro_init(stack_size);
    _disp_funcs[TTYPE_C] = _coro_dispatch;
#if WITH_LUA
    _disp_funcs[TTYPE_LUA] = _lua_dispatch;
#endif
    protos_init();
#if SCHEDULER_GLOBAL
    spin_init(&ctx->lckglobal, SPIN_CNT_SCHEDULER);
    qu_task_init(&ctx->quglobal, ONEK);
    mutex_init(&ctx->mutex);
    cond_init(&ctx->cond);
#endif
    ctx->nworker = 0 == nworker ? 1 : nworker;
    CALLOC(ctx->worker, 1, sizeof(worker_ctx) * ctx->nworker);
    CALLOC(ctx->monitor.version, 1, sizeof(worker_version) * ctx->nworker);
    rwlock_init(&ctx->lckmaptasks);
    ctx->maptasks = hashmap_new_with_allocator(_malloc, _realloc, _free,
                                              sizeof(name_t *), ONEK, 0, 0,
                                              _map_task_hash, _map_task_compare, _map_task_free, NULL);
#if WITH_SSL
    rwlock_init(&ctx->lckcerts);
    arr_certs_init(&ctx->arrcerts, 0);
#endif
    ctx->monitor.thread = thread_creat(_monitor_loop, ctx);
    worker_ctx *worker;
    for (uint16_t i = 0; i < ctx->nworker; i++) {
        worker = &ctx->worker[i];
        worker->index = i;
        worker->srey = ctx;
#if !SCHEDULER_GLOBAL
        spin_init(&worker->lcktasks, SPIN_CNT_SCHEDULER);
        qu_task_init(&worker->qutasks, ONEK);
        mutex_init(&worker->mutex);
        cond_init(&worker->cond);
#endif
        worker->thread = thread_creat(_worker_loop, worker);
    }
    tw_init(&ctx->tw);
    ev_init(&ctx->netev, nnet);
    return ctx;
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
static void _task_closing(srey_ctx *ctx) {
    message_ctx closing;
    closing.mtype = MSG_TYPE_CLOSING;
    rwlock_rdlock(&ctx->lckmaptasks);
    hashmap_scan(ctx->maptasks, _closing_push, &closing);
    rwlock_unlock(&ctx->lckmaptasks);
    size_t n;
    uint32_t time = 0;
    for (;;) {
        rwlock_rdlock(&ctx->lckmaptasks);
        n = hashmap_count(ctx->maptasks);
        if (0 == n) {
            rwlock_unlock(&ctx->lckmaptasks);
            break;
        }
        if (time >= 5 * 1000) {
            time = 0;
            hashmap_scan(ctx->maptasks, _closing_timeout, NULL);
        }
        rwlock_unlock(&ctx->lckmaptasks);
        MSLEEP(50);
        time += 50;
    }
}
void srey_free(srey_ctx *ctx) {
    _task_closing(ctx);
    ctx->stop = 1;
    worker_ctx *worker;
    _worker_wakeup_all(ctx);
    for (uint16_t i = 0; i < ctx->nworker; i++) {
        worker = &ctx->worker[i];
        thread_join(worker->thread);
    }
    ctx->monitor.stop = 1;
    thread_join(ctx->monitor.thread);
    ev_free(&ctx->netev);
    tw_free(&ctx->tw);
#if WITH_SSL
    uint32_t n = arr_certs_size(&ctx->arrcerts);
    for (uint32_t i = 0; i < n; i++) {
        evssl_free(arr_certs_at(&ctx->arrcerts, i)->ssl);
    }
    arr_certs_free(&ctx->arrcerts);
    rwlock_free(&ctx->lckcerts);
#endif
#if SCHEDULER_GLOBAL
    spin_free(&ctx->lckglobal);
    qu_task_free(&ctx->quglobal);
    mutex_free(&ctx->mutex);
    cond_free(&ctx->cond);
#else
    for (uint16_t i = 0; i < ctx->nworker; i++) {
        worker = &ctx->worker[i];
        spin_free(&worker->lcktasks);
        qu_task_free(&worker->qutasks);
        mutex_free(&worker->mutex);
        cond_free(&worker->cond);
    }
#endif
    hashmap_free(ctx->maptasks);
    rwlock_free(&ctx->lckmaptasks);
    FREE(ctx->worker);
    FREE(ctx->monitor.version);
    FREE(ctx);
}
#if WITH_SSL
static certs_ctx *_ssl_get(srey_ctx *ctx, name_t name) {
    certs_ctx *cert;
    uint32_t n = arr_certs_size(&ctx->arrcerts);
    for (uint32_t i = 0; i < n; i++) {
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
static void _ctask_request_run(task_ctx *task, message_ctx *msg) {
    if (NULL != task->_request[msg->pktype]) {
        task->_request[msg->pktype](task, msg);
    } else {
        task_ctx *dst = srey_task_grab(task->srey, msg->src);
        if (NULL != dst) {
            srey_response(dst, msg->sess, ERR_FAILED, NULL, 0, 0);
            srey_task_ungrab(dst);
        }
        LOG_WARN("request type %d not register callback.", msg->pktype);
    }
}
static void _ctask_timeout_run(task_ctx *task, message_ctx *msg) {
    ctask_tmo_arg *tmo = msg->data;
    if (NULL == tmo
        || NULL == tmo->_timeout) {
        return;
    }
    tmo->_timeout(task, tmo->arg);
}
task_ctx *srey_task_new(task_type ttype, name_t name, free_cb _argfree, void *arg) {
    if (INVALID_TNAME == name) {
        return NULL;
    }
    task_ctx *task;
    CALLOC(task, 1, sizeof(task_ctx));
    task->ttype = (uint8_t)ttype;
    task->name = name;
    task->ref = 1;
    task->_arg_free = _argfree;
    task->arg = arg;
    if (TTYPE_C == ttype) {
        task->_run[MSG_TYPE_REQUEST] = _ctask_request_run;
        task->_run[MSG_TYPE_TIMEOUT] = _ctask_timeout_run;
        task->_request[REQ_TYPE_RPC] = _ctask_rpc;
    }
    _coro_new(task);
    _rpc_new(task);
    spin_init(&task->lckmsg, SPIN_CNT_TASKMSG);
    qu_message_init(&task->qumsg, ONEK);
    qu_ptr_init(&task->qutmoarg, 0);
    return task;
}
static void _timeout_arg_free(ctask_tmo_arg *tmo) {
    if (NULL == tmo) {
        return;
    }
    if (NULL != tmo->_argfree
        && NULL != tmo->arg) {
        tmo->_argfree(tmo->arg);
    }
    FREE(tmo);
}
void srey_task_free(task_ctx *task) {
    if (NULL != task->_arg_free
        && NULL != task->arg) {
        task->_arg_free(task->arg);
    }
    message_ctx *msg;
    while (NULL != (msg = qu_message_pop(&task->qumsg))) {
        message_clean(task, msg->mtype, msg->pktype, msg->data);
    }
    ctask_tmo_arg **tmo;
    while (NULL != (tmo = (ctask_tmo_arg **)qu_ptr_pop(&task->qutmoarg))) {
        _timeout_arg_free(*tmo);
    }
    _coro_free(task);
    _rpc_free(task);
    qu_message_free(&task->qumsg);
    spin_free(&task->lckmsg);
    qu_ptr_free(&task->qutmoarg);
    FREE(task);
}
void srey_task_regcb(task_ctx *task, msg_type mtype, task_run _cb) {
    if (TTYPE_C == task->ttype) {
        if (MSG_TYPE_TIMEOUT == mtype) {
            return;
        }
        if (MSG_TYPE_REQUEST == mtype) {
            task->_request[REQ_TYPE_DEF] = _cb;
            return;
        }
        task->_run[mtype] = _cb;
    } else {
        if (MSG_TYPE_ALL == mtype) {
            for (msg_type i = MSG_TYPE_NONE + 1; i < MSG_TYPE_ALL; i++) {
                task->_run[i] = _cb;
            }
        } else {
            task->_run[mtype] = _cb;
        }
    }
}
int32_t srey_task_register(srey_ctx *ctx, task_ctx *task) {
    task->srey = ctx;
    message_ctx startup;
    startup.mtype = MSG_TYPE_STARTUP;
    rwlock_wrlock(&ctx->lckmaptasks);
    if (NULL != _map_task_get(ctx->maptasks, task->name)) {
        rwlock_unlock(&ctx->lckmaptasks);
        LOG_ERROR("task name %d repeat.", task->name);
        return ERR_FAILED;
    }
    _map_task_set(ctx->maptasks, task);
    _task_message_push(task, &startup);
    rwlock_unlock(&ctx->lckmaptasks);
    return ERR_OK;
}
task_ctx *srey_task_grab(srey_ctx *ctx, name_t name) {
    if (INVALID_TNAME == name) {
        return NULL;
    }
    rwlock_rdlock(&ctx->lckmaptasks);
    task_ctx *task = _map_task_get(ctx->maptasks, name);
    if (NULL != task) {
        ATOMIC_ADD(&task->ref, 1);
    }
    rwlock_unlock(&ctx->lckmaptasks);
    return task;
}
void srey_task_incref(task_ctx *task) {
    ATOMIC_ADD(&task->ref, 1);
}
void srey_task_ungrab(task_ctx *task) {
    if (1 != ATOMIC_ADD(&task->ref, -1)) {
        return;
    }
    void *ptr = NULL;
    rwlock_wrlock(&task->srey->lckmaptasks);
    if (0 == task->ref) {
        ptr = _map_task_del(task->srey->maptasks, task->name);
    }
    rwlock_unlock(&task->srey->lckmaptasks);
    if (NULL != ptr) {
        srey_task_free(task);
    }
}
void srey_task_close(task_ctx *task) {
    if (ATOMIC_CAS(&task->closing, 0, 1)) {
        message_ctx closing;
        closing.mtype = MSG_TYPE_CLOSING;
        _task_message_push(task, &closing);
    }
}
static void _srey_timeout(ud_cxt *ud) {
    task_ctx *task = srey_task_grab(ud->data, ud->name);
    if (NULL == task) {
        _timeout_arg_free(ud->extra);
        return;
    }
    message_ctx msg;
    msg.mtype = MSG_TYPE_TIMEOUT;
    msg.sess = ud->sess;
    msg.data = ud->extra;
    _task_message_push(task, &msg);
    srey_task_ungrab(task);
}
static void _timeout_ud_free(void *arg) {
    _timeout_arg_free(((ud_cxt *)arg)->extra);
}
static ctask_tmo_arg *_timeout_arg(task_ctx *task, ctask_timeout _timeout, free_cb _argfree, void *arg) {
    ctask_tmo_arg *tmo, **tmp;
    tmp = (ctask_tmo_arg **)qu_ptr_pop(&task->qutmoarg);
    if (NULL == tmp) {
        MALLOC(tmo, sizeof(ctask_tmo_arg));
    } else {
        tmo = *tmp;
    }
    tmo->_timeout = _timeout;
    tmo->_argfree = _argfree;
    tmo->arg = arg;
    return tmo;
}
void srey_timeout(task_ctx *task, uint64_t sess, uint32_t ms, ctask_timeout _timeout, free_cb _argfree, void *arg) {
    ud_cxt ud;
    ud.name = task->name;
    ud.data = task->srey;
    ud.sess = sess;
    if (NULL != _timeout) {
        ud.extra = _timeout_arg(task, _timeout, _argfree, arg);
    } else {
        ud.extra = NULL;
    }
    tw_add(&task->srey->tw, ms, _srey_timeout, _timeout_ud_free, &ud);
}
void srey_request(task_ctx *dst, task_ctx *src, request_type rtype, uint64_t sess, void *data, size_t size, int32_t copy) {
    message_ctx msg;
    msg.mtype = MSG_TYPE_REQUEST;
    msg.pktype = rtype;
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
    _task_message_push(dst, &msg);
}
void srey_response(task_ctx *dst, uint64_t sess, int32_t erro, void *data, size_t size, int32_t copy) {
    message_ctx msg;
    msg.mtype = MSG_TYPE_RESPONSE;
    msg.sess = sess;
    msg.erro = erro;
    msg.size = size;
    if (NULL != data) {
        if (0 != copy) {
            MALLOC(msg.data, size);
            memcpy(msg.data, data, size);
        } else {
            msg.data = data;
        }
    } else {
        msg.data = NULL;
    }
    _task_message_push(dst, &msg);
}
void srey_call(task_ctx *dst, request_type rtype, void *data, size_t size, int32_t copy) {
    srey_request(dst, NULL, rtype, 0, data, size, copy);
}
static int32_t _net_accept(ev_ctx *ev, SOCKET fd, uint64_t skid, ud_cxt *ud) {
    task_ctx *task = srey_task_grab(ud->data, ud->name);
    if (NULL == task) {
        return ERR_FAILED;
    }
    message_ctx msg;
    msg.mtype = MSG_TYPE_ACCEPT;
    msg.pktype = ud->pktype;
    msg.fd = fd;
    msg.skid = skid;
    _task_message_push(task, &msg);
    srey_task_ungrab(task);
    return ERR_OK;
}
static void _set_sess_slice(message_ctx *msg, ud_cxt *ud, int32_t slice) {
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
static void _net_recv(ev_ctx *ev, SOCKET fd, uint64_t skid, buffer_ctx *buf, size_t size, ud_cxt *ud) {
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
            _task_message_push(task, &msg);
        }
    } while (NULL != data && 0 != buffer_size(buf));
    if (0 != closefd) {
        ev_close(ev, fd, skid);
    }
    srey_task_ungrab(task);
}
static void _net_send(ev_ctx *ev, SOCKET fd, uint64_t skid, size_t size, ud_cxt *ud) {
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
    _task_message_push(task, &msg);
    srey_task_ungrab(task);
}
static void _net_close(ev_ctx *ev, SOCKET fd, uint64_t skid, ud_cxt *ud) {
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
    _task_message_push(task, &msg);
    srey_task_ungrab(task);
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
static int32_t _net_connect(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t err, ud_cxt *ud) {
    task_ctx *task = srey_task_grab(ud->data, ud->name);
    if (NULL == task) {
        return ERR_FAILED;
    }
    message_ctx msg;
    msg.mtype = MSG_TYPE_CONNECT;
    msg.pktype = ud->pktype;
    msg.skid = skid;
    msg.fd = fd;
    msg.erro = err;
    msg.sess = ud->sess;
    ud->sess = 0;
    _task_message_push(task, &msg);
    srey_task_ungrab(task);
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
static void _net_recvfrom(ev_ctx *ev, SOCKET fd, uint64_t skid, char *buf, size_t size, netaddr_ctx *addr, ud_cxt *ud) {
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
    _task_message_push(task, &msg);
    srey_task_ungrab(task);
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
void handshaked_push(SOCKET fd, uint64_t skid, ud_cxt *ud, int32_t *closefd, int32_t erro) {
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
    msg.erro = erro;
    _task_message_push(task, &msg);
    srey_task_ungrab(task);
}
