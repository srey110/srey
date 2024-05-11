#include "srey/task.h"
#include "ds/hashmap.h"

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
task_ctx *task_new(name_t name, _task_dispatch_cb _dispatch, free_cb _argfree, void *arg) {
    if (INVALID_TNAME == name) {
        return NULL;
    }
    task_ctx *task;
    CALLOC(task, 1, sizeof(task_ctx));
    task->name = name;
    task->ref = 1;
    if (NULL == _dispatch) {
        task->_task_dispatch = _message_dispatch;
    } else {
        task->_task_dispatch = _dispatch;
    }
    task->_arg_free = _argfree;
    task->arg = arg;
#if WITH_CORO
    _coro_new(task);
#endif
    spin_init(&task->lckmsg, SPIN_CNT_TASKMSG);
    qu_message_init(&task->qumsg, ONEK);
    return task;
}
void task_free(task_ctx *task) {
    if (NULL != task->_arg_free
        && NULL != task->arg) {
        task->_arg_free(task->arg);
    }
    message_ctx *msg;
    while (NULL != (msg = qu_message_pop(&task->qumsg))) {
        _message_clean(msg->mtype, msg->pktype, msg->data);
    }
#if WITH_CORO
    _coro_free(task);
#endif
    qu_message_free(&task->qumsg);
    spin_free(&task->lckmsg);
    FREE(task);
}
int32_t task_register(scheduler_ctx *scheduler, task_ctx *task, _task_startup_cb _startup, _task_closing_cb _closing) {
    task->scheduler = scheduler;
    task->_task_startup = _startup;
    task->_task_closing = _closing;
    message_ctx startup;
    startup.mtype = MSG_TYPE_STARTUP;
    rwlock_wrlock(&scheduler->lckmaptasks);
    if (NULL != _map_task_get(scheduler->maptasks, task->name)) {
        rwlock_unlock(&scheduler->lckmaptasks);
        LOG_ERROR("task name %d repeat.", task->name);
        return ERR_FAILED;
    }
    _map_task_set(scheduler->maptasks, task);
    _task_message_push(task, &startup);
    rwlock_unlock(&scheduler->lckmaptasks);
    return ERR_OK;
}
void task_close(task_ctx *task) {
    if (ATOMIC_CAS(&task->closing, 0, 1)) {
        message_ctx closing;
        closing.mtype = MSG_TYPE_CLOSING;
        _task_message_push(task, &closing);
    }
}
task_ctx *task_grab(scheduler_ctx *scheduler, name_t name) {
    if (INVALID_TNAME == name) {
        return NULL;
    }
    rwlock_rdlock(&scheduler->lckmaptasks);
    task_ctx *task = _map_task_get(scheduler->maptasks, name);
    if (NULL != task) {
        ATOMIC_ADD(&task->ref, 1);
    }
    rwlock_unlock(&scheduler->lckmaptasks);
    return task;
}
void task_incref(task_ctx *task) {
    ATOMIC_ADD(&task->ref, 1);
}
void task_ungrab(task_ctx *task) {
    if (1 != ATOMIC_ADD(&task->ref, -1)) {
        return;
    }
    void *ptr = NULL;
    rwlock_wrlock(&task->scheduler->lckmaptasks);
    if (0 == task->ref) {
        ptr = _map_task_del(task->scheduler->maptasks, task->name);
    }
    rwlock_unlock(&task->scheduler->lckmaptasks);
    if (NULL != ptr) {
        task_free(task);
    }
}
