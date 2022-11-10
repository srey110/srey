#ifndef EVENT_H_
#define EVENT_H_

#include "buffer.h"
#include "netaddr.h"
#include "netutils.h"
#include "loger.h"
#include "utils.h"
#include "event/evpub.h"

void ev_init(ev_ctx *ctx, uint32_t nthreads);
void ev_free(ev_ctx *ctx);

int32_t ev_listener(ev_ctx *ctx, const char *host, const uint16_t port,
    accept_cb cb, free_ud f_cb, ud_cxt *ud);
int32_t ev_connecter(ev_ctx *ctx, const char *host, const uint16_t port, 
    connect_cb conn_cb, ud_cxt *ud);

void ev_loop(ev_ctx *ctx, SOCKET sock, recv_cb r_cb, close_cb c_cb, send_cb s_cb, ud_cxt *ud);
void ev_send(ev_ctx *ctx, SOCKET sock, void *data, size_t len, int32_t copy);
void ev_close(ev_ctx *ctx, SOCKET sock);
void ev_updateud(ev_ctx *ctx, SOCKET sock, free_ud f_cb, ud_cxt *ud);//f_cb

void _freelsn(struct listener_ctx *lsn);

SOCKET _ev_sock(int32_t family);
SOCKET _ev_listen(netaddr_ctx *addr);

#endif //EVENT_H_
