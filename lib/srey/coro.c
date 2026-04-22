#include "srey/coro.h"
#include "containers/hashmap.h"
#include "containers/heap.h"
#include "utils/timer.h"
#define MINICORO_IMPL
#include "srey/minicoro.h"

/* 超时堆节点：嵌入最小堆，存储过期时间和关联 session */
typedef struct timeout_entry {
    heap_node hnode;     /* 必须在首位，供 UPCAST 使用 */
    uint64_t  timeout;
    uint64_t  sess;
} timeout_entry;
#define _TE_FROM_HNODE(n) UPCAST(n, timeout_entry, hnode)

typedef struct coro_info {
    msg_type       mtype;
    mco_coro      *co;
    uint64_t       timeout;
    timeout_entry *te;   /* 非 NULL 表示已注册到超时堆 */
}coro_info;
QUEUE_DECL(coro_info, qu_coinfo)
typedef struct coro_sess {
    int32_t disposable; /* 1 = one-shot, 0 = persistent (connection-lifetime) */
    uint64_t sess;
    union {
        coro_info     coinfo;   /* disposable == 1: single pending coroutine */
        qu_coinfo_ctx qucoinfo; /* disposable == 0: queue of pending coroutines (inline, no malloc) */
    };
}coro_sess;
typedef struct coro_ctx {//task->arg
    int32_t nyield;
    mco_coro *curco;
    struct hashmap *mapco;
    qu_ptr_ctx qucopool;
    timer_ctx timer;
    heap_ctx timeout_heap; /* 按过期时间排序的最小堆，O(1) 检查堆顶即可判断是否有超时 */
}coro_ctx;

static mco_desc _coro_desc;

/* 最小堆比较函数：timeout 小的优先（堆顶是最早到期的） */
static int _timeout_cmp(const heap_node *lhs, const heap_node *rhs) {
    return _TE_FROM_HNODE(lhs)->timeout < _TE_FROM_HNODE(rhs)->timeout;
}

static uint64_t _map_cosess_hash(const void *item, uint64_t seed0, uint64_t seed1) {
    return hash((const char *)&(((coro_sess *)item)->sess), sizeof(((coro_sess *)item)->sess));
}
static int _map_cosess_compare(const void *a, const void *b, void *ud) {
    return (int)(((coro_sess *)a)->sess - ((coro_sess *)b)->sess);
}
/* 若 ms > 0，创建 timeout_entry 并插入超时堆，返回指针；否则返回 NULL */
static timeout_entry *_te_insert(coro_ctx *coctx, uint64_t timeout, uint64_t sess) {
    timeout_entry *te;
    MALLOC(te, sizeof(timeout_entry));
    te->hnode.parent = te->hnode.left = te->hnode.right = NULL;
    te->timeout = timeout;
    te->sess    = sess;
    heap_insert(&coctx->timeout_heap, &te->hnode);
    return te;
}
static void _map_cosess_set(task_ctx *task, int32_t disposable, mco_coro *coro, uint64_t sess, msg_type mtype, uint32_t ms) {
    coro_ctx *coctx = task->arg;
    coro_sess key;
    key.sess = sess;
    if (disposable) {
        coro_sess cosess;
        cosess.disposable = 1;
        cosess.sess = sess;
        cosess.coinfo.te = NULL;
        cosess.coinfo.timeout = ms > 0 ? timer_cur_ms(&coctx->timer) + ms : 0;
        cosess.coinfo.co = coro;
        cosess.coinfo.mtype = mtype;
        ASSERTAB(NULL == hashmap_set(coctx->mapco, &cosess), "repeat session");
        if (ms > 0) {
            /* hashmap_set 已拷贝 cosess；取真实存储位置设 te */
            coro_sess *stored = (coro_sess *)hashmap_get(coctx->mapco, &key);
            stored->coinfo.te = _te_insert(coctx, cosess.coinfo.timeout, sess);
        }
        return;
    }
    /* non-disposable: inline qu_coinfo_ctx, no heap allocation for the queue header */
    coro_info coinfo;
    coinfo.te = NULL;
    coinfo.timeout = ms > 0 ? timer_cur_ms(&coctx->timer) + ms : 0;
    coinfo.co = coro;
    coinfo.mtype = mtype;
    coro_sess *cofind = (coro_sess *)hashmap_get(coctx->mapco, &key);
    if (NULL != cofind) {
        /* entry already exists: push directly into the inline queue */
        qu_coinfo_push(&cofind->qucoinfo, &coinfo);
        if (ms > 0) {
            coro_info *last = qu_coinfo_at(&cofind->qucoinfo,
                                           qu_coinfo_size(&cofind->qucoinfo) - 1);
            last->te = _te_insert(coctx, coinfo.timeout, sess);
        }
    } else {
        /* new entry: init inline queue on the stack, hashmap_set copies the whole struct */
        coro_sess cosess;
        cosess.disposable = 0;
        cosess.sess = sess;
        qu_coinfo_init(&cosess.qucoinfo, 0);   /* allocates cosess.qucoinfo.ptr on heap */
        qu_coinfo_push(&cosess.qucoinfo, &coinfo);
        hashmap_set(coctx->mapco, &cosess);
        /* hashmap_set copies cosess (struct copy); qucoinfo.ptr is now owned by the stored copy.
         * cosess goes out of scope but the heap array lives on through the stored copy. */
        if (ms > 0) {
            coro_sess *stored = (coro_sess *)hashmap_get(coctx->mapco, &key);
            coro_info *last = qu_coinfo_at(&stored->qucoinfo,
                                           qu_coinfo_size(&stored->qucoinfo) - 1);
            last->te = _te_insert(coctx, coinfo.timeout, sess);
        }
    }
}
/* Returns a direct pointer into the hashmap's internal storage, or NULL if not found/filtered.
 * The caller must NOT call _map_cosess_delete until it has finished using the returned pointer. */
static coro_sess *_map_cosess_get(coro_ctx *coctx, uint64_t sess, msg_type mtype) {
    coro_sess key;
    key.sess = sess;
    coro_sess *cofind = (coro_sess *)hashmap_get(coctx->mapco, &key);
    if (NULL == cofind) {
        return NULL;
    }
    if (cofind->disposable) {
        if (mtype != cofind->coinfo.mtype && MSG_TYPE_CLOSE != mtype) {
            return NULL;
        }
    } else {
        coro_info *coinfo = qu_coinfo_peek(&cofind->qucoinfo);
        if (NULL == coinfo) {
            return NULL;
        }
        if (mtype != coinfo->mtype && MSG_TYPE_CLOSE != mtype) {
            return NULL;
        }
    }
    return cofind;
}
static void _map_cosess_delete(coro_ctx *coctx, uint64_t sess) {
    coro_sess key;
    key.sess = sess;
    hashmap_delete(coctx->mapco, &key);
}
/* 取出协程并在必要时从超时堆中移除对应条目 */
static inline mco_coro *_get_mco(task_ctx *task, coro_sess *cosess) {
    coro_ctx *coctx = (coro_ctx *)task->arg;
    coro_info *coinfo;
    mco_coro  *co;
    if (cosess->disposable) {
        coinfo = &cosess->coinfo;
        co     = coinfo->co;
    } else {
        coinfo = qu_coinfo_pop(&cosess->qucoinfo);  /* inline queue: pass address */
        if (NULL == coinfo) {
            return NULL;
        }
        co = coinfo->co;
    }
    /* 协程在超时前已被正常唤醒：从堆中删除对应超时节点 */
    if (NULL != coinfo->te) {
        heap_remove(&coctx->timeout_heap, &coinfo->te->hnode);
        FREE(coinfo->te);
    }
    return co;
}
static void _mco_cb(mco_coro *coro) {
    mco_result rtn;
    for (;;) {
        rtn = mco_yield(coro);
        ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
        /* Pop the pointer (8 bytes) and copy into coroutine-stack local so that
         * arg.fd / arg.skid etc. stay valid for the entire coroutine lifetime
         * even after the caller's stack frame is reused. */
        task_dispatch_arg *argp;
        rtn = mco_pop(coro, &argp, sizeof(argp));
        ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
        task_dispatch_arg arg = *argp;             /* one copy, on coroutine stack */
        task_incref(arg.task);//��֤_message_run��yield�󲻻ᱻ�ͷ�
        _message_run(arg.task, &arg.msg);
        qu_ptr_push(&((coro_ctx *)arg.task->arg)->qucopool, (void **)&coro);
        task_ungrab(arg.task);
    }
}
void coro_desc_init(size_t stack_size) {
    _coro_desc = mco_desc_init(_mco_cb, stack_size);
}
static coro_ctx *_coro_ctx_init(void) {
    coro_ctx *coctx;
    CALLOC(coctx, 1, sizeof(coro_ctx));
    qu_ptr_init(&coctx->qucopool, 0);
    timer_init(&coctx->timer);
    coctx->mapco = hashmap_new_with_allocator(_malloc, _realloc, _free,
                                              sizeof(coro_sess), ONEK, 0, 0,
                                              _map_cosess_hash, _map_cosess_compare, NULL, NULL);
    heap_init(&coctx->timeout_heap, _timeout_cmp);
    return coctx;
}
static void _coro_ctx_free(void *arg) {
    coro_ctx *coctx = arg;
    mco_result rtn;
    mco_coro **coro;
    while (NULL != (coro = (mco_coro **)qu_ptr_pop(&coctx->qucopool))) {
        rtn = mco_destroy(*coro);
        if (MCO_SUCCESS != rtn) {
            LOG_WARN("%s", mco_result_description(rtn));
        }
    }
    qu_ptr_free(&coctx->qucopool);
    /* 先释放超时堆（堆节点独立分配，不依赖 mapco） */
    while (NULL != coctx->timeout_heap.root) {
        timeout_entry *te = _TE_FROM_HNODE(coctx->timeout_heap.root);
        heap_dequeue(&coctx->timeout_heap);
        FREE(te);
    }
    size_t iter = 0;
    coro_sess *corosess;
    while (hashmap_iter(coctx->mapco, &iter, (void **)&corosess)) {
        if (!corosess->disposable) {
            qu_coinfo_free(&corosess->qucoinfo); /* free inline queue's ptr array; no struct FREE */
        }
    }
    hashmap_free(coctx->mapco);
    FREE(coctx);
}
static mco_coro *_coro_pool_get(task_ctx *task) {
    coro_ctx *coctx = task->arg;
    mco_coro **coro = (mco_coro **)qu_ptr_pop(&coctx->qucopool);
    if (NULL != coro) {
        return *coro;
    }
    mco_coro *coronew;
    mco_result rtn = mco_create(&coronew, &_coro_desc);
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
    rtn = mco_resume(coronew);
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
    return coronew;
}
static void _co_create(task_dispatch_arg *arg) {
    coro_ctx *coctx = arg->task->arg;
    coctx->curco = _coro_pool_get(arg->task);
    /* Push the pointer (8 bytes) instead of the full struct; _mco_cb copies on resume */
    mco_result rtn = mco_push(coctx->curco, &arg, sizeof(arg));
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
    rtn = mco_resume(coctx->curco);
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
}
static void _co_resume(mco_coro *coro, task_dispatch_arg *arg) {
    coro_ctx *coctx = arg->task->arg;
    coctx->curco = coro;
    /* Push message pointer (8 bytes) instead of the full message_ctx struct */
    message_ctx *msgptr = &arg->msg;
    mco_result rtn = mco_push(coro, &msgptr, sizeof(msgptr));
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
    rtn = mco_resume(coro);
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
    _message_clean(arg->msg.mtype, arg->msg.pktype, arg->msg.data);
}
static void _timeout_dispatch(task_dispatch_arg *arg) {
    if (0 == arg->msg.sess) {
        _co_create(arg);
        return;
    }
    coro_ctx *coctx = arg->task->arg;
    coro_sess *cosess = _map_cosess_get(coctx, arg->msg.sess, (msg_type)arg->msg.mtype);
    if (NULL == cosess) {
        LOG_WARN("task %d message type %d, can't find session %"PRIu64, arg->task->name, arg->msg.mtype, arg->msg.sess);
        return;
    }
    mco_coro *coro = _get_mco(arg->task, cosess);
    if (cosess->disposable) {
        _map_cosess_delete(coctx, arg->msg.sess);
    }
    _co_resume(coro, arg);
}
static void _connected_dispatch(task_dispatch_arg *arg) {
    coro_ctx *coctx = arg->task->arg;
    coro_sess *cosess = _map_cosess_get(coctx, arg->msg.skid, (msg_type)arg->msg.mtype);
    if (NULL == cosess) {
        _co_create(arg);
        return;
    }
    mco_coro *coro = _get_mco(arg->task, cosess);
    if (NULL == coro) {
        _co_create(arg);
    } else {
        _co_resume(coro, arg);
    }
}
static void _ssl_exchanged_dispatch(task_dispatch_arg *arg) {
    coro_ctx *coctx = arg->task->arg;
    coro_sess *cosess = _map_cosess_get(coctx, arg->msg.skid, (msg_type)arg->msg.mtype);
    if (NULL == cosess) {
        _co_create(arg);
        return;
    }
    mco_coro *coro = _get_mco(arg->task, cosess);
    if (NULL == coro) {
        _co_create(arg);
    } else {
        _co_resume(coro, arg);
    }
}
static void _handshaked_dispatch(task_dispatch_arg *arg) {
    coro_ctx *coctx = arg->task->arg;
    coro_sess *cosess = _map_cosess_get(coctx, arg->msg.skid, (msg_type)arg->msg.mtype);
    if (NULL == cosess) {
        _co_create(arg);
        return;
    }
    mco_coro *coro = _get_mco(arg->task, cosess);
    if (NULL == coro) {
        _co_create(arg);
    } else {
        _co_resume(coro, arg);
    }
}
static void _recved_dispatch(task_dispatch_arg *arg) {
    if (0 == arg->msg.sess
        || ERR_OK != prots_may_resume(arg->msg.pktype, arg->msg.data)) {
        _co_create(arg);
        return;
    }
    coro_ctx *coctx = arg->task->arg;
    coro_sess *cosess = _map_cosess_get(coctx, arg->msg.sess, (msg_type)arg->msg.mtype);
    if (NULL == cosess) {
        _co_create(arg);
        return;
    }
    mco_coro *coro = _get_mco(arg->task, cosess);
    if (NULL == coro) {
        _co_create(arg);
    } else {
        _co_resume(coro, arg);
    }
}
static void _closed_dispatch(task_dispatch_arg *arg) {
    coro_ctx *coctx = arg->task->arg;
    coro_sess *cosess = _map_cosess_get(coctx, arg->msg.skid, (msg_type)arg->msg.mtype);
    if (NULL != cosess) {
        mco_coro *coro;
        /* Drain all waiting coroutines BEFORE deleting from hashmap (pointer stays valid) */
        while (NULL != (coro = _get_mco(arg->task, cosess))) {
            _co_resume(coro, arg);
        }
        if (!cosess->disposable) {
            qu_coinfo_free(&cosess->qucoinfo); /* free the inline queue's internal ptr array */
        }
        _map_cosess_delete(coctx, arg->msg.skid);
    }
    _co_create(arg);
}
static void _recvfrom_dispatch(task_dispatch_arg *arg) {
    if (0 == arg->msg.sess) {
        _co_create(arg);
        return;
    }
    coro_ctx *coctx = arg->task->arg;
    coro_sess *cosess = _map_cosess_get(coctx, arg->msg.sess, (msg_type)arg->msg.mtype);
    if (NULL == cosess) {
        _co_create(arg);
    } else {
        /* recvfrom sessions are always disposable */
        mco_coro *coro = _get_mco(arg->task, cosess);
        _map_cosess_delete(coctx, arg->msg.sess);
        _co_resume(coro, arg);
    }
}
static void _response_dispatch(task_dispatch_arg *arg) {
    coro_ctx *coctx = arg->task->arg;
    coro_sess *cosess = _map_cosess_get(coctx, arg->msg.sess, (msg_type)arg->msg.mtype);
    if (NULL == cosess) {
        _co_create(arg);
    } else {
        /* response sessions are always disposable */
        mco_coro *coro = _get_mco(arg->task, cosess);
        _map_cosess_delete(coctx, arg->msg.sess);
        _co_resume(coro, arg);
    }
}
static void _timeout_monitor(task_ctx *task, uint64_t sess) {
    coro_ctx *coctx = task->arg;
    /* 只有存在挂起协程且堆非空才需要检查 */
    if (coctx->nyield > 0 && NULL != coctx->timeout_heap.root) {
        uint64_t now = timer_cur_ms(&coctx->timer);
        task_dispatch_arg arg;
        arg.task = task;
        arg.msg.mtype = MSG_TYPE_TIMEOUT;
        /* 堆顶是最早到期的条目：若堆顶未到期，后续全部未到期，O(1) 退出 */
        while (NULL != coctx->timeout_heap.root) {
            timeout_entry *te = _TE_FROM_HNODE(coctx->timeout_heap.root);
            if (te->timeout > now) {
                break; /* 最早的都没到期，无需继续 */
            }
            heap_dequeue(&coctx->timeout_heap);
            coro_sess key;
            key.sess = te->sess;
            coro_sess *cosess = (coro_sess *)hashmap_get(coctx->mapco, &key);
            if (NULL == cosess) {
                /* 已被正常路径消费（_get_mco 已删堆节点），此处只需释放 te */
                FREE(te);
                continue;
            }
            mco_coro *coro;
            coro_info *coinfo;
            if (cosess->disposable) {
                coinfo = &cosess->coinfo;
                coinfo->te = NULL; /* 堆节点已由 heap_dequeue 移除 */
                coro = coinfo->co;
                LOG_INFO("task %d message type %d session %"PRIu64" timeout.",
                         task->name, coinfo->mtype, te->sess);
                hashmap_delete(coctx->mapco, &key);
            } else {
                coinfo = qu_coinfo_peek(&cosess->qucoinfo);
                if (NULL == coinfo) {
                    FREE(te);
                    continue;
                }
                coinfo->te = NULL;
                coro = coinfo->co;
                LOG_INFO("task %d message type %d session %"PRIu64" timeout.",
                         task->name, coinfo->mtype, te->sess);
                qu_coinfo_pop(&cosess->qucoinfo);
            }
            arg.msg.sess = te->sess;
            FREE(te);
            _co_resume(coro, &arg);
        }
    }
    task_timeout(task, 0, 3 * 1000, _timeout_monitor);
}
static void _message_dispatch(task_dispatch_arg *arg) {
    switch (arg->msg.mtype) {
    case MSG_TYPE_STARTUP:
        task_timeout(arg->task, 0, 3 * 1000, _timeout_monitor);
        _co_create(arg);
        break;
    case MSG_TYPE_CLOSING:
        _co_create(arg);
        if (((coro_ctx *)arg->task->arg)->nyield > 0) {
            LOG_WARN("task %d yield %d.", arg->task->name, ((coro_ctx *)arg->task->arg)->nyield);
        }
        break;
    case MSG_TYPE_TIMEOUT:
        _timeout_dispatch(arg);
        break;
    case MSG_TYPE_ACCEPT:
        _co_create(arg);
        break;
    case MSG_TYPE_CONNECT:
        _connected_dispatch(arg);
        break;
    case MSG_TYPE_SSLEXCHANGED:
        _ssl_exchanged_dispatch(arg);
        break;
    case MSG_TYPE_HANDSHAKED:
        _handshaked_dispatch(arg);
        break;
    case MSG_TYPE_RECV:
        _recved_dispatch(arg);
        break;
    case MSG_TYPE_SEND:
        _co_create(arg);
        break;
    case MSG_TYPE_CLOSE:
        _closed_dispatch(arg);
        break;
    case MSG_TYPE_RECVFROM:
        _recvfrom_dispatch(arg);
        break;
    case MSG_TYPE_REQUEST:
        _co_create(arg);
        break;
    case MSG_TYPE_RESPONSE:
        _response_dispatch(arg);
        break;
    default:
        break;
    }
}
task_ctx *coro_task_register(loader_ctx *loader, name_t name, _task_startup_cb _startup, _task_closing_cb _closing) {
    coro_ctx *coctx = _coro_ctx_init();
    task_ctx *task = task_new(loader, name, _message_dispatch, _coro_ctx_free, coctx);
    if (ERR_OK != task_register(task, _startup, _closing)) {
        task_free(task);
        return NULL;
    }
    return task;
}
void coro_sync(task_ctx *task, SOCKET fd, uint64_t skid) {
    ev_ud_sess(&task->loader->netev, fd, skid, skid);
}
/* Yield and wait for the next matching message.
 * Returns a pointer directly into the caller's (dispatcher) arg->msg — valid
 * until the next _coro_wait call or until _co_resume returns. */
static inline message_ctx *_coro_wait(task_ctx *task, int32_t disposable, uint64_t sess, msg_type mtype, uint32_t ms) {
    coro_ctx *coctx = task->arg;
    _map_cosess_set(task, disposable, coctx->curco, sess, mtype, ms);
    ++coctx->nyield;
    mco_result rtn = mco_yield(coctx->curco);
    --coctx->nyield;
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
    /* Pop the pointer (8 bytes) pushed by _co_resume — no large struct copy */
    message_ctx *msg;
    rtn = mco_pop(coctx->curco, &msg, sizeof(msg));
    ASSERTAB(MCO_SUCCESS == rtn, mco_result_description(rtn));
    ASSERTAB(sess == msg->sess, "different session");
    return msg;
}
void coro_sleep(task_ctx *task, uint32_t ms) {
    uint64_t sess = createid();
    task_timeout(task, sess, ms, NULL);
    _coro_wait(task, 1, sess, MSG_TYPE_TIMEOUT, 0);
}
void *coro_request(task_ctx *dst, task_ctx *src, uint8_t rtype, void *data, size_t size, int32_t copy, int32_t *erro, size_t *lens) {
    uint64_t sess = createid();
    task_request(dst, src, rtype, sess, data, size, copy);
    message_ctx *msg = _coro_wait(src, 1, sess, MSG_TYPE_RESPONSE, task_get_request_timeout(src));
    if (MSG_TYPE_TIMEOUT == msg->mtype) {
        *erro = ERR_FAILED;
        LOG_WARN("dst %d src %d request type %d timeout, session %"PRIu64".", dst->name, src->name, rtype, sess);
        return NULL;
    }
    *erro = msg->erro;
    SET_PTR(lens, msg->size);
    return msg->data;
}
static int32_t _wait_ssl_exchanged(task_ctx *task, SOCKET fd, uint64_t skid) {
    message_ctx *msg = _coro_wait(task, 0, skid, MSG_TYPE_SSLEXCHANGED, task_get_netread_timeout(task));
    if (MSG_TYPE_TIMEOUT == msg->mtype) {
        ev_close(&task->loader->netev, fd, skid);
        LOG_WARN("task %d, ssl exchange timeout, skid %"PRIu64".", task->name, skid);
        return ERR_FAILED;
    }
    if (MSG_TYPE_CLOSE == msg->mtype) {
        return ERR_FAILED;
    }
    return ERR_OK;
}
int32_t coro_ssl_exchange(task_ctx *task, SOCKET fd, uint64_t skid, int32_t client, struct evssl_ctx *evssl) {
    if (ERR_OK != ev_ssl(&task->loader->netev, fd, skid, client, evssl)) {
        return ERR_FAILED;
    }
    return _wait_ssl_exchanged(task, fd, skid);
}
void *coro_handshaked(task_ctx *task, SOCKET fd, uint64_t skid, int32_t *err, size_t *size) {
    message_ctx *msg = _coro_wait(task, 0, skid, MSG_TYPE_HANDSHAKED, task_get_netread_timeout(task));
    if (MSG_TYPE_TIMEOUT == msg->mtype) {
        *err = ERR_FAILED;
        ev_close(&task->loader->netev, fd, skid);
        LOG_WARN("task: %d, handshake timeout, skid %"PRIu64".", task->name, skid);
        return NULL;
    }
    if (MSG_TYPE_CLOSE == msg->mtype) {
        *err = ERR_FAILED;
        return NULL;
    }
    *err = msg->erro;
    SET_PTR(size, msg->size);
    return msg->data;
}
int32_t coro_connect(task_ctx *task, pack_type pktype, struct evssl_ctx *evssl, const char *ip, uint16_t port, int32_t netev, void *extra,
    SOCKET *fd, uint64_t *skid) {
    if (ERR_OK != task_connect(task, pktype, evssl, ip, port, netev, extra, fd, skid)) {
        LOG_WARN("task: %d, connect %s:%d error.", task->name, ip, port);
        return ERR_FAILED;
    }
    message_ctx *msg = _coro_wait(task, 0, *skid, MSG_TYPE_CONNECT, task_get_connect_timeout(task));
    if (MSG_TYPE_TIMEOUT == msg->mtype) {
        ev_close(&task->loader->netev, *fd, *skid);
        LOG_WARN("task: %d, connect %s:%d timeout.", task->name, ip, port);
        return ERR_FAILED;
    }
    if (ERR_OK != msg->erro) {
        LOG_WARN("task: %d, connect %s:%d error.", task->name, ip, port);
        return ERR_FAILED;
    }
    if (NULL != evssl) {
        if (ERR_OK != _wait_ssl_exchanged(task, *fd, *skid)) {
            return ERR_FAILED;
        }
    }
    coro_sync(task, *fd, *skid);
    return ERR_OK;
}
/* Returns the received message pointer, or NULL on timeout/close.
 * Valid until the next _coro_wait call. */
static message_ctx *_wait_recved(task_ctx *task, SOCKET fd, uint64_t skid) {
    message_ctx *msg = _coro_wait(task, 0, skid, MSG_TYPE_RECV, task_get_netread_timeout(task));
    if (MSG_TYPE_TIMEOUT == msg->mtype) {
        ev_close(&task->loader->netev, fd, skid);
        LOG_WARN("task %d, recve timeout, skid %"PRIu64".", task->name, skid);
        return NULL;
    }
    if (MSG_TYPE_CLOSE == msg->mtype) {
        return NULL;
    }
    return msg;
}
void *coro_send(task_ctx *task, SOCKET fd, uint64_t skid, void *data, size_t len, size_t *size, int32_t copy) {
    if (ERR_OK != ev_send(&task->loader->netev, fd, skid, data, len, copy)) {
        return NULL;
    }
    message_ctx *msg = _wait_recved(task, fd, skid);
    if (NULL == msg) {
        return NULL;
    }
    SET_PTR(size, msg->size);
    return msg->data;
}
void *coro_slice(task_ctx *task, SOCKET fd, uint64_t skid, size_t *size, int32_t *end) {
    message_ctx *msg = _wait_recved(task, fd, skid);
    if (NULL == msg) {
        return NULL;
    }
    if (PROT_SLICE_END == msg->slice) {
        *end = 1;
    }
    SET_PTR(size, msg->size);
    return msg->data;
}
void *coro_sendto(task_ctx *task, SOCKET fd, uint64_t skid, const char *ip, const uint16_t port,
    void *data, size_t len, size_t *size, int32_t copy) {
    coro_sync(task, fd, skid);
    if (ERR_OK != ev_sendto(&task->loader->netev, fd, skid, ip, port, data, len, copy)) {
        LOG_WARN("task %d, sendto error, skid %"PRIu64".", task->name, skid);
        ev_ud_sess(&task->loader->netev, fd, skid, 0);
        return NULL;
    }
    message_ctx *msg = _coro_wait(task, 1, skid, MSG_TYPE_RECVFROM, task_get_netread_timeout(task));
    if (MSG_TYPE_TIMEOUT == msg->mtype) {
        ev_ud_sess(&task->loader->netev, fd, skid, 0);
        LOG_WARN("task %d, sendto timeout, skid %"PRIu64".", task->name, skid);
        return NULL;
    }
    SET_PTR(size, msg->size);
    return ((char *)msg->data) + sizeof(netaddr_ctx);
}
