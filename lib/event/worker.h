#ifndef WORKER_H_
#define WORKER_H_

#include "event/evpub.h"

#ifdef EV_IOCP

struct worker_ctx *worker_init(ev_ctx *ev, uint32_t nthread, uint32_t nqus);
void worker_free(struct worker_ctx *worker);

struct sock_ctx *worker_newsk(struct worker_ctx *worker, SOCKET fd, cbs_ctx *cbs, ud_cxt *ud);
void worker_add(struct worker_ctx *worker, SOCKET fd, struct sock_ctx *skctx);
void worker_remove(struct worker_ctx *worker, SOCKET fd);

int32_t _check_canfree(struct sock_ctx *skctx);
void _add_bufs_trypost(struct sock_ctx *skctx, bufs_ctx *buf);
void _add_bufs_trysendto(struct sock_ctx *skctx, bufs_ctx *buf);
void _free_udp(struct sock_ctx *skctx);
int32_t _sock_type(struct sock_ctx *skctx);

#endif//EV_IOCP
#endif//WORKER_H_
