#include "service/srpc.h"
#include "service/srey.h"
#include "service/synsl.h"
#include "proto/http.h"
#include "cjson/cJSON.h"
#include "hashmap.h"

static inline uint64_t _map_rpc_hash(const void *item, uint64_t seed0, uint64_t seed1) {
    return hash(((rpc_ctx *)item)->method, strlen(((rpc_ctx *)item)->method));
}
static inline int _map_rpc_compare(const void *a, const void *b, void *ud) {
    return strcmp(((rpc_ctx *)a)->method, ((rpc_ctx *)b)->method);
}
static inline rpc_cb _map_rpc_get(task_ctx *task, const char *name) {
    rpc_ctx key;
    size_t nlens = strlen(name);
    strcpy(key.method, name);
    key.method[nlens] = '\0';
    rpc_ctx *rpc = (rpc_ctx *)hashmap_get(task->maprpc, (void *)&key);
    if (NULL == rpc) {
        return NULL;
    }
    return rpc->rpc;
}
void _rpc_new(task_ctx *task) {
    task->maprpc = hashmap_new_with_allocator(_malloc, _realloc, _free,
                                              sizeof(rpc_ctx), 0, 0, 0,
                                              _map_rpc_hash, _map_rpc_compare, NULL, NULL);
}
void _rpc_free(task_ctx *task) {
    hashmap_free(task->maprpc);
}
static inline cJSON *_rpc_call(task_ctx *task, cJSON *request) {
    cJSON *val = cJSON_GetObjectItemCaseSensitive(request, "method");
    if (NULL == val
        || !cJSON_IsString(val)
        || strlen(val->string) >= RPC_NAME_LENS) {
        return NULL;
    }
    rpc_cb _cb = _map_rpc_get(task, val->valuestring);
    if (NULL == _cb) {
        return NULL;
    }
    val = cJSON_GetObjectItemCaseSensitive(request, "args");
    return _cb(task, val);
}
void _ctask_rpc(task_ctx *task, message_ctx *msg) {
    cJSON *request = cJSON_ParseWithLength(msg->data, msg->size);
    if (NULL == request) {
        LOG_WARN("json parse error. %s.", cJSON_GetErrorPtr());
        return;
    }
    cJSON *rtn = _rpc_call(task, request);
    cJSON_Delete(request);
    task_ctx *dst = srey_task_grab(task->srey, msg->src);
    if (NULL == dst) {
        if (NULL != rtn) {
            cJSON_Delete(rtn);
        }
        return;
    }
    if (NULL == rtn) {
        srey_response(dst, msg->sess, ERR_FAILED, NULL, 0, 0);
    } else {
        char *json = cJSON_PrintUnformatted(rtn);
        cJSON_Delete(rtn);
        srey_response(dst, msg->sess, ERR_OK, json, strlen(json), 0);
    }
    srey_task_ungrab(dst);
}
void rpc_register(task_ctx *task, const char *method, rpc_cb _cb) {
    rpc_ctx rpc;
    size_t nlens = strlen(method);
    ASSERTAB(nlens < sizeof(rpc.method), "name too long.");
    strcpy(rpc.method, method);
    rpc.method[nlens] = '\0';
    rpc.rpc = _cb;
    hashmap_set(task->maprpc, &rpc);
}
static inline int32_t _dump_args(cJSON *jarg, const char *fomat, va_list args) {
    size_t flens = strlen(fomat);
    if (0 == flens) {
        return ERR_OK;
    }
    for (size_t i = 0; i < flens; i++) {
        switch (fomat[i]) {
        case ' ':
            break;
        case 'j': {
            char *val = va_arg(args, char*);
            if (NULL == val) {
                cJSON_AddItemToArray(jarg, cJSON_CreateNull());
            } else {
                cJSON_AddItemToArray(jarg, (cJSON *)val);
            }
            break;
        }
        case 's': {
            char *val = va_arg(args, char*);
            if (NULL == val) {
                cJSON_AddItemToArray(jarg, cJSON_CreateNull());
            } else {
                cJSON *item = cJSON_CreateString(val);
                if (NULL != item) {
                    cJSON_AddItemToArray(jarg, item);
                } else {
                    return ERR_FAILED;
                }
            }
            break;
        }
        case 'i': {
            int32_t val = va_arg(args, int32_t);
            cJSON_AddItemToArray(jarg, cJSON_CreateNumber(val));
            break;
        }
        case 'I': {
            long val = va_arg(args, long);
            cJSON_AddItemToArray(jarg, cJSON_CreateNumber(val));
            break;
        }
        case 'u': {
            uint32_t val = va_arg(args, uint32_t);
            cJSON_AddItemToArray(jarg, cJSON_CreateNumber(val));
            break;
        }
        case 'U': {
            unsigned long val = va_arg(args, unsigned long);
            cJSON_AddItemToArray(jarg, cJSON_CreateNumber(val));
            break;
        }
        case 'f': {
            double val = va_arg(args, double);
            cJSON_AddItemToArray(jarg, cJSON_CreateNumber(val));
            break;
        }
        case 'b': {
            int32_t val = va_arg(args, int32_t);
            cJSON_AddItemToArray(jarg, cJSON_CreateBool(0 != val));
            break;
        }
        case 'n': {
            cJSON_AddItemToArray(jarg, cJSON_CreateNull());
            break;
        }
        default:
            LOG_WARN("unknown format.");
            return ERR_FAILED;
        }
    }
    return ERR_OK;
}
static inline char *_format_method(const char *method, const char *fomat, va_list args) {
    cJSON *request = cJSON_CreateObject();
    if (NULL == request) {
        return NULL;
    }
    if (NULL == cJSON_AddStringToObject(request, "method", method)) {
        cJSON_Delete(request);
        return NULL;
    }
    cJSON *jarg = cJSON_AddArrayToObject(request, "args");
    if (NULL == jarg) {
        cJSON_Delete(request);
        return NULL;
    }
    if (ERR_OK != _dump_args(jarg, fomat, args)) {
        cJSON_Delete(request);
        return NULL;
    }
    char *json = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    return json;
}
cJSON *rpc_format_args(const char *fomat, ...) {
    cJSON *resp = cJSON_CreateArray();
    va_list args;
    va_start(args, fomat);
    int32_t rtn = _dump_args(resp, fomat, args);
    va_end(args);
    if (ERR_OK != rtn) {
        cJSON_Delete(resp);
        return NULL;
    }
    return resp;
}
void rpc_call(task_ctx *dst, const char *method, const char *fomat, ...) {
    va_list args;
    va_start(args, fomat);
    char *req = _format_method(method, fomat, args);
    va_end(args);
    if (NULL == req) {
        return;
    }
    srey_call(dst, REQ_TYPE_RPC, req, strlen(req), 0);
}
void *rpc_request(task_ctx *dst, task_ctx *src, int32_t *erro, size_t *lens, const char *method, const char *fomat, ...) {
    va_list args;
    va_start(args, fomat);
    char *req = _format_method(method, fomat, args);
    va_end(args);
    if (NULL == req) {
        *erro = ERR_FAILED;
        return NULL;
    }
    return syn_request(dst, src, REQ_TYPE_RPC, req, strlen(req), 0, erro, lens);
}
void rpc_net_call(task_ctx *task, name_t dst, SOCKET fd, uint64_t skid,
    const char *method, const char *fomat, ...) {
    va_list args;
    va_start(args, fomat);
    char *jreq = _format_method(method, fomat, args);
    va_end(args);
    if (NULL == jreq) {
        return;
    }
    char url[64];
    SNPRINTF(url, sizeof(url) - 1, "/rpc_call?dst=%d", dst);
    buffer_ctx buf;
    buffer_init(&buf);
    http_pack_req(&buf, "POST", url);
    http_pack_head(&buf, "Server", "Srey");
    http_pack_head(&buf, "Connection", "Keep-Alive");
    http_pack_head(&buf, "Content-Type", "application/json");
    http_pack_content(&buf, jreq, strlen(jreq));
    FREE(jreq);
    size_t lens = buffer_size(&buf);
    MALLOC(jreq, lens);
    ASSERTAB(lens == buffer_copyout(&buf, 0, jreq, lens), "buffer_copyout error.");
    ev_send(&task->srey->netev, fd, skid, jreq, lens, 0);
    buffer_free(&buf);
}
void *rpc_net_request(task_ctx *task, name_t dst, SOCKET fd, uint64_t skid,
    size_t *lens, const char *method, const char *fomat, ...) {
    va_list args;
    va_start(args, fomat);
    char *jreq = _format_method(method, fomat, args);
    va_end(args);
    if (NULL == jreq) {
        return NULL;
    }
    char url[64];
    SNPRINTF(url, sizeof(url) - 1, "/rpc_request?dst=%d", dst);
    buffer_ctx buf;
    buffer_init(&buf);
    http_pack_req(&buf, "POST", url);
    http_pack_head(&buf, "Server", "Srey");
    http_pack_head(&buf, "Connection", "Keep-Alive");
    http_pack_head(&buf, "Content-Type", "application/json");
    http_pack_content(&buf, jreq, strlen(jreq));
    FREE(jreq);
    size_t blens = buffer_size(&buf);
    MALLOC(jreq, blens);
    ASSERTAB(blens == buffer_copyout(&buf, 0, jreq, blens), "buffer_copyout error.");
    struct http_pack_ctx *resp = syn_send(task, fd, skid, createid(), jreq, blens, &blens, 0);
    buffer_free(&buf);
    if (NULL == resp
        || 0 != http_chunked(resp)) {
        return NULL;
    }
    buf_ctx *hstatus = http_status(resp);
    if (!buf_compare(&hstatus[1], "200", strlen("200"))) {
        return NULL;
    }
    return http_data(resp, lens);
}
