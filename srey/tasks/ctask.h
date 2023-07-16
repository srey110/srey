#ifndef CTASK_H_
#define CTASK_H_

#include "lib.h"

typedef enum request_type{
    REQ_TYPE_RPC = 0x00,

    REQ_TYPE_CNT
}request_type;

typedef struct ctask_ctx ctask_ctx;
typedef void(*ctask_startup_cb)(ctask_ctx *ctask);
typedef void(*ctask_closing_cb)(ctask_ctx *ctask);
typedef void(*ctask_connect_cb)(ctask_ctx *ctask, pack_type pktype, SOCKET fd, uint64_t skid, int32_t erro);
typedef void(*ctask_handshake_cb)(ctask_ctx *ctask, pack_type pktype, SOCKET fd, uint64_t skid);
typedef void(*ctask_accept_cb)(ctask_ctx *ctask, pack_type pktype, SOCKET fd, uint64_t skid);
typedef void(*ctask_recv_cb)(ctask_ctx *ctask, pack_type pktype, SOCKET fd, uint64_t skid, void *data, size_t size);
typedef void(*ctask_send_cb)(ctask_ctx *ctask, pack_type pktype, SOCKET fd, uint64_t skid, size_t size);
typedef void(*ctask_close_cb)(ctask_ctx *ctask, pack_type pktype, SOCKET fd, uint64_t skid);
typedef void(*ctask_recvfrom_cb)(ctask_ctx *ctask, pack_type pktype, SOCKET fd, uint64_t skid, netaddr_ctx *addr, void *data, size_t size);

//typedef cJSON *(*ctask_req_rpc_cb)(ctask_ctx *ctask, int32_t method, cJSON *args);//{"type":0, dst, src, sess, method, args[],....}

int32_t ctask_register(srey_ctx *ctx, ctask_ctx *parent, name_t name, uint16_t maxcnt, uint16_t maxmsgqulens,
    ctask_startup_cb startup, void(*_arg_free)(void *arg), void *arg);

task_ctx *ctask_task(ctask_ctx *ctask);
void *ctask_arg(ctask_ctx *ctask);

void ctask_closing(ctask_ctx *ctask, ctask_closing_cb cb);
void ctask_connect(ctask_ctx *ctask, ctask_connect_cb cb);
void ctask_handshake(ctask_ctx *ctask, ctask_handshake_cb cb);
void ctask_accept(ctask_ctx *ctask, ctask_accept_cb cb);
void ctask_recv(ctask_ctx *ctask, ctask_recv_cb cb);
void ctask_send(ctask_ctx *ctask, ctask_send_cb cb);
void ctask_close(ctask_ctx *ctask, ctask_close_cb cb);
void ctask_recvfrom(ctask_ctx *ctask, ctask_recvfrom_cb cb);
void ctask_timeout(ctask_ctx *ctask, uint32_t ms, int32_t once, void(*_timeout)(ctask_ctx *ctask, void *arg),
    void(*_arg_free)(void *arg), void *arg);

//void ctask_request_rpc(ctask_ctx *ctask, ctask_req_rpc_cb cb);

#endif//CTASK_H_
