#ifndef SRPC_H_
#define SRPC_H_

#include "service/spub.h"

void rpc_register(task_ctx *task, const char *method, rpc_cb _cb);
//fomat s:string i:int I:long u:unsigned int U:unsigned long f:double b:bool n:null j:cJSON *
void rpc_call(task_ctx *dst, const char *method, const char *fomat, ...);
void *rpc_request(task_ctx *dst, task_ctx *src, int32_t *erro, size_t *lens, const char *method, const char *fomat, ...);

void rpc_net_call(task_ctx *task, name_t dst, SOCKET fd, uint64_t skid, const char *key,
    const char *method, const char *fomat, ...);
void *rpc_net_request(task_ctx *task, name_t dst, SOCKET fd, uint64_t skid, const char *key,
    size_t *lens, const char *method, const char *fomat, ...);

struct cJSON *rpc_format_args(const char *fomat, ...);

#endif//SRPC_H_
