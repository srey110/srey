#ifndef EVENT_H_ 
#define EVENT_H_

#include "event/evpub.h"
#include "event/evssl.h"

void ev_init(ev_ctx *ctx, uint32_t nthreads);
void ev_free(ev_ctx *ctx);

int32_t ev_listen(ev_ctx *ctx, struct evssl_ctx *evssl, const char *host, const uint16_t port, cbs_ctx *cbs, ud_cxt *ud);
SOCKET ev_connect(ev_ctx *ctx, struct evssl_ctx *evssl, const char *host, const uint16_t port, cbs_ctx *cbs, ud_cxt *ud);
SOCKET ev_udp(ev_ctx *ctx, const char *host, const uint16_t port, cbs_ctx *cbs, ud_cxt *ud);

void ev_send(ev_ctx *ctx, SOCKET fd, void *data, size_t len, int32_t copy);
void ev_sendto(ev_ctx *ctx, SOCKET fd, const char *host, const uint16_t port, void *data, size_t len);
void ev_close(ev_ctx *ctx, SOCKET fd);

void ev_setud_pktype(ev_ctx *ctx, SOCKET fd, uint8_t pktype);
void ev_setud_data(ev_ctx *ctx, SOCKET fd, void *data);

#endif//EVENT_H_
