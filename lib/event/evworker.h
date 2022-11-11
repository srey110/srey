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

struct sock_ctx * _new_sockctx(struct ev_ctx *ctx, SOCKET sock);
void _free_sockctx(struct sock_ctx *sock);
void _invalid_sockctx(struct sock_ctx *sock);
struct buffer_ctx *_get_buffer_r(struct sock_ctx *sock);

int32_t _post_recv(struct sock_ctx *sock);
int32_t _post_send(struct sock_ctx *sock);

#endif//EVWORKER_H_
