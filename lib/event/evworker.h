#ifndef EVWORKER_H_
#define EVWORKER_H_

#include "cond.h"
#include "mutex.h"
#include "thread.h"
#include "queue.h"
#include "event/evpub.h"

struct eworker_ctx *eworker_init(struct ev_ctx *ev, uint32_t nthread, uint32_t nqus);
void eworker_free(struct eworker_ctx *ctx);

void ewcmd_accept(struct eworker_ctx *ctx, SOCKET fd, accept_cb cb, ud_cxt *ud);
void ewcmd_connect(struct eworker_ctx *ctx, SOCKET fd, connect_cb cb, ud_cxt *ud);

void ewcmd_canread(struct eworker_ctx *ctx, SOCKET fd);
void ewcmd_canwrite(struct eworker_ctx *ctx, SOCKET fd);
void ewcmd_error(struct eworker_ctx *ctx, SOCKET fd);

struct sock_ctx * _new_sockctx(struct ev_ctx *ctx, SOCKET sock);
void _reset_sockctx(struct sock_ctx *skctx, SOCKET sock);
void _close_sockctx(struct sock_ctx *skctx);
void _free_sockctx(struct sock_ctx *skctx);
struct buffer_ctx *_get_recv_buf(struct sock_ctx *skctx);
qu_bufs *_get_send_buf(struct sock_ctx *skctx);

int32_t _post_recv(ev_ctx *ev, struct sock_ctx *sock);
int32_t _post_send(ev_ctx *ev, struct sock_ctx *sock);

#endif//EVWORKER_H_
