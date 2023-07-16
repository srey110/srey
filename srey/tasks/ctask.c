#include "tasks/ctask.h"
#include "hashmap.h"

typedef struct timeout_cbs {
    int32_t once;
    uint32_t ms;
    void(*_timeout)(ctask_ctx *ctask, void *arg);
    void(*_arg_free)(void *arg);
    void *arg;
    uint64_t sess;
}timeout_cbs;
struct ctask_ctx {
    task_ctx *task;
    struct hashmap *timeout;
    void *arg;
    void(*_arg_free)(void *arg); 
    void *req_cbs[REQ_TYPE_CNT];
    void *cbs[MSG_TYPE_CNT];
};

#define REG_CBS(ctask, msgtype, cb) ctask->cbs[msgtype] = (void *)cb

static inline uint64_t _map_timeout_hash(const void *item, uint64_t seed0, uint64_t seed1) {
    return hash((const char *)&(((timeout_cbs *)item)->sess), sizeof(((timeout_cbs *)item)->sess));
}
static inline int  _map_timeout_compare(const void *a, const void *b, void *ud) {
    return (int)(((timeout_cbs *)a)->sess - ((timeout_cbs *)b)->sess);
}
static void _map_timeout_free(void *item) {
    timeout_cbs *cb = item;
    if (NULL != cb->_arg_free) {
        cb->_arg_free(cb->arg);
    }
}
static inline _map_timeout_add(struct hashmap *timeout, uint64_t sess, uint32_t ms, int32_t once,
    void(*_timeout)(ctask_ctx *ctask, void *arg), void(*_arg_free)(void *arg), void *arg) {
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

static void *_ctask_new(task_ctx *task, void *arg) {
    ((ctask_ctx *)arg)->task = task;
    return arg;
}
static void _ctask_free(ctask_ctx *ctask) {
    if (NULL != ctask->_arg_free) {
        ctask->_arg_free(ctask->arg);
    }
    hashmap_free(ctask->timeout);
    FREE(ctask);
}
//static inline int32_t _json_get_int32(cJSON *json, const char *key, int32_t def) {
//    cJSON *val = cJSON_GetObjectItemCaseSensitive(json, key);
//    if (NULL == val
//        || !cJSON_IsNumber(val)) {
//        return def;
//    }
//    return val->valueint;
//}
//static inline cJSON *_ctask_request_rpc(ctask_ctx *ctask, cJSON *json) {
//    void *func = ctask->req_cbs[REQ_TYPE_RPC];
//    if (NULL == func) {
//        return NULL;
//    }
//    int32_t dst = _json_get_int32(json, "dst", INVALID_TNAME);
//    int32_t src = _json_get_int32(json, "src", INVALID_TNAME);
//    int32_t method = _json_get_int32(json, "method", -1);
//    if (-1 == method) {
//        return NULL;
//    }
//    cJSON *args = cJSON_GetObjectItemCaseSensitive(json, "args");
//    return ((ctask_req_rpc_cb)func)(ctask, method, args);
//}
////{"type":0, dst, src, method, args[],....}
//static inline void _ctask_request(ctask_ctx *ctask, message_ctx *msg) {
//    cJSON *json = cJSON_ParseWithLength(msg->data, msg->size);
//    if (NULL == json) {
//        const char *erro = cJSON_GetErrorPtr();
//        if (NULL != erro) {
//            LOG_WARN("%s", erro);
//        }
//        return;
//    }
//    int32_t type = _json_get_int32(json, "type", 0);
//    switch (type) {
//    case REQ_TYPE_RPC: {
//        cJSON *rtn = _ctask_request_rpc(ctask, json);
//        if (NULL == rtn) {
//        }
//        break;
//    }
//    default:
//        break;
//    }
//    cJSON_Delete(json);
//}
static inline void _ctask_run(task_ctx *task, message_ctx *msg) {
    ctask_ctx *ctask = task->handle;
    switch (msg->msgtype) {
    case MSG_TYPE_STARTUP: {
        void *func = ctask->cbs[msg->msgtype];
        if (NULL != func) {
            ((ctask_startup_cb)func)(ctask);
        }
        break;
    }
    case MSG_TYPE_CLOSING: {
        void *func = ctask->cbs[msg->msgtype];
        if (NULL != func) {
            ((ctask_closing_cb)func)(ctask);
        }
        break;
    }
    case MSG_TYPE_TIMEOUT: {
        timeout_cbs cb;
        if (ERR_OK != _map_timeout_get(ctask->timeout, msg->sess, &cb)) {
            break;
        }
        if (0 != cb.once) {
            cb._timeout(ctask, cb.arg);
            if (NULL != cb._arg_free) {
                cb._arg_free(cb.arg);
            }
        } else {
            task_timeout(ctask->task, cb.sess, cb.ms);
            cb._timeout(ctask, cb.arg);
        }
        break;
    }
    case MSG_TYPE_CONNECT: {//pktype fd skid err
        void *func = ctask->cbs[msg->msgtype];
        if (NULL != func) {
            ((ctask_connect_cb)func)(ctask, msg->pktype, msg->fd, msg->skid, msg->erro);
        }
        break;
    }
    case MSG_TYPE_HANDSHAKED: {//pktype fd skid
        void *func = ctask->cbs[msg->msgtype];
        if (NULL != func) {
            ((ctask_handshake_cb)func)(ctask, msg->pktype, msg->fd, msg->skid);
        }
        break;
    }
    case MSG_TYPE_ACCEPT: {//pktype fd skid
        void *func = ctask->cbs[msg->msgtype];
        if (NULL != func) {
            ((ctask_accept_cb)func)(ctask, msg->pktype, msg->fd, msg->skid);
        }
        break;
    }
    case MSG_TYPE_SEND: {//pktype fd skid size
        void *func = ctask->cbs[msg->msgtype];
        if (NULL != func) {
            ((ctask_send_cb)func)(ctask, msg->pktype, msg->fd, msg->skid, msg->size);
        }
        break;
    }
    case MSG_TYPE_CLOSE: {//pktype fd skid
        void *func = ctask->cbs[msg->msgtype];
        if (NULL != func) {
            ((ctask_close_cb)func)(ctask, msg->pktype, msg->fd, msg->skid);
        }
        break;
    }
    case MSG_TYPE_RECV: {//pktype fd skid data size
        if (PACK_RPC == msg->pktype) {

        } else {
            void *func = ctask->cbs[msg->msgtype];
            if (NULL != func) {
                ((ctask_recv_cb)func)(ctask, msg->pktype, msg->fd, msg->skid, msg->data, msg->size);
            }
        }
        break;
    }
    case MSG_TYPE_RECVFROM: {//fd skid addr data size
        netaddr_ctx *addr = msg->data;
        void *func = ctask->cbs[msg->msgtype];
        if (NULL != func) {
            ((ctask_recvfrom_cb)func)(ctask, msg->pktype, msg->fd, msg->skid, addr, ((char *)msg->data) + sizeof(netaddr_ctx), msg->size);
        }
        break;
    }
    case MSG_TYPE_REQUEST://sess src data size
        //_ctask_request(ctask, msg);
        break;
    default:
        break;
    }
}
int32_t ctask_register(srey_ctx *ctx, ctask_ctx *parent, name_t name, uint16_t maxcnt, uint16_t maxmsgqulens,
    ctask_startup_cb startup, void(*_arg_free)(void *arg), void *arg) {
    ASSERTAB(NULL != startup, ERRSTR_INVPARAM);
    ctask_ctx *ctask;
    CALLOC(ctask, 1, sizeof(ctask_ctx));
    ctask->arg = arg;
    ctask->_arg_free = _arg_free;
    REG_CBS(ctask, MSG_TYPE_STARTUP, startup);
    ctask->timeout = hashmap_new_with_allocator(_malloc, _realloc, _free,
                                                sizeof(timeout_cbs), 0, 0, 0,
                                                _map_timeout_hash, _map_timeout_compare, _map_timeout_free, NULL);
    if (NULL == parent) {
        return srey_task_new(ctx, TTYPE_C, name, maxcnt, maxmsgqulens, INVALID_TNAME, 0,
                             _ctask_new, _ctask_run, NULL, _ctask_free, ctask);
    }
    return task_synnew(parent->task, TTYPE_C, name, maxcnt, maxmsgqulens,
                       _ctask_new, _ctask_run, NULL, _ctask_free, ctask);
}
task_ctx *ctask_task(ctask_ctx *ctask) {
    return ctask->task;
}
void *ctask_arg(ctask_ctx *ctask) {
    return ctask->arg;
}
void ctask_closing(ctask_ctx *ctask, ctask_closing_cb cb) {
    REG_CBS(ctask, MSG_TYPE_CLOSING, cb);
}
void ctask_connect(ctask_ctx *ctask, ctask_connect_cb cb) {
    REG_CBS(ctask, MSG_TYPE_CONNECT, cb);
}
void ctask_handshake(ctask_ctx *ctask, ctask_handshake_cb cb) {
    REG_CBS(ctask, MSG_TYPE_HANDSHAKED, cb);
}
void ctask_accept(ctask_ctx *ctask, ctask_accept_cb cb) {
    REG_CBS(ctask, MSG_TYPE_ACCEPT, cb);
}
void ctask_recv(ctask_ctx *ctask, ctask_recv_cb cb) {
    REG_CBS(ctask, MSG_TYPE_RECV, cb);
}
void ctask_send(ctask_ctx *ctask, ctask_send_cb cb) {
    REG_CBS(ctask, MSG_TYPE_SEND, cb);
}
void ctask_close(ctask_ctx *ctask, ctask_close_cb cb) {
    REG_CBS(ctask, MSG_TYPE_CLOSE, cb);
}
void ctask_recvfrom(ctask_ctx *ctask, ctask_recvfrom_cb cb) {
    REG_CBS(ctask, MSG_TYPE_RECVFROM, cb);
}
//void ctask_request_rpc(ctask_ctx *ctask, ctask_req_rpc_cb cb) {
//    ctask->req_cbs[REQ_TYPE_RPC] = cb;
//}
void ctask_timeout(ctask_ctx *ctask, uint32_t ms, int32_t once, void(*_timeout)(ctask_ctx *ctask, void *arg),
    void(*_arg_free)(void *arg), void *arg) {
    uint64_t sess = createid();
    _map_timeout_add(ctask->timeout, sess, ms, once, _timeout, _arg_free, arg);
    task_timeout(ctask->task, sess, ms);
}
