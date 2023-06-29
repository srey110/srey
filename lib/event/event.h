#ifndef EVENT_H_ 
#define EVENT_H_

#include "event/evpub.h"
#include "event/evssl.h"

void ev_init(ev_ctx *ctx, uint32_t nthreads);
void ev_free(ev_ctx *ctx);
//0.0.0.0 - ::  127.0.0.1 - ::1
int32_t ev_listen(ev_ctx *ctx, struct evssl_ctx *evssl, const char *ip, const uint16_t port,
    cbs_ctx *cbs, ud_cxt *ud);
SOCKET ev_connect(ev_ctx *ctx, struct evssl_ctx *evssl, const char *ip, const uint16_t port,
    cbs_ctx *cbs, ud_cxt *ud, sock_status status, uint64_t *skid);
SOCKET ev_udp(ev_ctx *ctx, const char *ip, const uint16_t port,
    cbs_ctx *cbs, ud_cxt *ud, uint64_t *skid);

void ev_send(ev_ctx *ctx, SOCKET fd, uint64_t skid,
    void *data, size_t len, int32_t copy, sock_status status);
int32_t ev_sendto(ev_ctx *ctx, SOCKET fd, uint64_t skid,
    const char *ip, const uint16_t port, void *data, size_t len, sock_status status);
void ev_close(ev_ctx *ctx, SOCKET fd, uint64_t skid, int32_t nomsg);

void ev_setud_typstat(ev_ctx *ctx, SOCKET fd, uint64_t skid, int8_t pktype, int8_t status);
void ev_setud_data(ev_ctx *ctx, SOCKET fd, uint64_t skid, void *data);

#endif//EVENT_H_
