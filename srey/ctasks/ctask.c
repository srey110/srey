#include "ctasks/ctask.h"
#include "hashmap.h"

typedef struct timeout_cbs {
    int32_t once;
    uint32_t ms;
    timeout_cb _timeout;
    free_cb _arg_free;
    void *arg;
    uint64_t sess;
}timeout_cbs;
struct ctask_ctx {
    task_ctx *task;
    struct hashmap *timeout;
    message_cb cbs[MSG_TYPE_CNT];
};

static inline uint64_t _map_timeout_hash(const void *item, uint64_t seed0, uint64_t seed1) {
    return hash((const char *)&(((timeout_cbs *)item)->sess), sizeof(((timeout_cbs *)item)->sess));
}
static inline int _map_timeout_compare(const void *a, const void *b, void *ud) {
    return (int)(((timeout_cbs *)a)->sess - ((timeout_cbs *)b)->sess);
}
static void _map_timeout_free(void *item) {
    timeout_cbs *cb = item;
    if (NULL != cb->_arg_free) {
        cb->_arg_free(cb->arg);
    }
}
static inline _map_timeout_add(struct hashmap *timeout, uint64_t sess, uint32_t ms, int32_t once,
    timeout_cb _timeout, free_cb _arg_free, void *arg) {
    timeout_cbs cbs;
    cbs.sess = sess;
    cbs.ms = ms;
    cbs.once = once;
    cbs._timeout = _timeout;
    cbs._arg_free = _arg_free;
    cbs.arg = arg;
    ASSERTAB(NULL == hashmap_set(timeout, &cbs), "session repeat!");
}
static inline int32_t _map_timeout_get(struct hashmap *timeout, uint64_t sess, timeout_cbs *cbs) {
    timeout_cbs key;
    key.sess = sess;
    timeout_cbs *tmp = (timeout_cbs *)hashmap_get(timeout, &key);
    if (NULL == tmp) {
        return ERR_FAILED;
    }
    *cbs = *tmp;
    if (0 != cbs->once) {
        hashmap_delete(timeout, &key);
    }
    return ERR_OK;
}
static void *_task_new(task_ctx *task, void *arg) {
    ((ctask_ctx *)arg)->task = task;
    return arg;
}
static inline void _task_run(task_ctx *task, message_ctx *msg) {
    ctask_ctx *ctask = task->handle;
    if (MSG_TYPE_TIMEOUT == msg->mtype) {
        timeout_cbs cb;
        if (ERR_OK != _map_timeout_get(ctask->timeout, msg->sess, &cb)) {
            return;
        }
        if (0 != cb.once) {
            cb._timeout(ctask, task, cb.arg);
            if (NULL != cb._arg_free) {
                cb._arg_free(cb.arg);
            }
        } else {
            srey_timeout(ctask->task, cb.sess, cb.ms);
            cb._timeout(ctask, task, cb.arg);
        }
        return;
    }
    message_cb func = ctask->cbs[msg->mtype];
    if (NULL != func) {
        func(ctask, task, msg);
    }
}
static void _ctask_free(ctask_ctx *ctask) {
    hashmap_free(ctask->timeout);
    FREE(ctask);
}
ctask_ctx *ctask_new(void) {
    ctask_ctx *ctask;
    CALLOC(ctask, 1, sizeof(ctask_ctx));
    ctask->timeout = hashmap_new_with_allocator(_malloc, _realloc, _free,
                                                sizeof(timeout_cbs), 0, 0, 0,
                                                _map_timeout_hash, _map_timeout_compare, _map_timeout_free, NULL);
    return ctask;
}
void ctask_regcb(ctask_ctx *ctask, msg_type mtype, message_cb cb) {
    ctask->cbs[mtype] = cb;
}
int32_t ctask_register(srey_ctx *ctx, ctask_ctx *ctask, ctask_ctx *parent, name_t name, uint16_t maxcnt, uint16_t maxmsgqulens) {
#if WITH_CORO
    if (NULL != parent) {
        return syn_task_new(parent->task, TTYPE_C, name, maxcnt, maxmsgqulens,
                            _task_new, _task_run, NULL, _ctask_free, ctask);
    }
#endif
    return srey_task_new(ctx, TTYPE_C, name, maxcnt, maxmsgqulens, INVALID_TNAME, 0,
                         _task_new, _task_run, NULL, _ctask_free, ctask);
}
void ctask_timeout(ctask_ctx *ctask, uint32_t ms, int32_t once, timeout_cb _timeout, free_cb _arg_free, void *arg) {
    uint64_t sess = createid();
    _map_timeout_add(ctask->timeout, sess, ms, once, _timeout, _arg_free, arg);
    srey_timeout(ctask->task, sess, ms);
}
