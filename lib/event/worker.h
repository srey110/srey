#ifndef WORKER_H_
#define WORKER_H_

#include "event/evpub.h"

#ifdef EV_IOCP

struct worker_ctx *worker_init(ev_ctx *ev, uint32_t nthread, uint32_t nqus);
void worker_free(struct worker_ctx *worker);

void worker_add(struct worker_ctx *worker, SOCKET fd, struct sock_ctx *skctx, rw_cb_ctx *cbs, ud_cxt *ud);
void worker_remove(struct worker_ctx *worker, SOCKET fd);
void worker_canread(struct worker_ctx *worker, SOCKET fd);
void worker_canwrite(struct worker_ctx *worker, SOCKET fd);

int32_t _post_recv(struct sock_ctx *skctx);
int32_t _post_send(struct sock_ctx *skctx);
void _free_sockctx(struct sock_ctx *skctx);
struct buffer_ctx *_get_recv_buf(struct sock_ctx *skctx);
qu_bufs *_get_send_buf(struct sock_ctx *skctx);

#endif//EV_IOCP
#endif//WORKER_H_
