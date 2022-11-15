#ifndef WORKER_H_
#define WORKER_H_

#include "event/evpub.h"

#ifdef EV_IOCP

struct worker_ctx *worker_init(ev_ctx *ev, uint32_t nthread, uint32_t nqus);
void worker_free(struct worker_ctx *worker);

void worker_add(struct worker_ctx *worker, SOCKET fd, struct sock_ctx *skctx);
void worker_remove(struct worker_ctx *worker, SOCKET fd);

int32_t _check_canfree(struct sock_ctx *skctx);
void _free_sockctx(struct sock_ctx *skctx);
void _qu_bufs_addpost(struct sock_ctx *skctx, bufs_ctx *buf);

#endif//EV_IOCP
#endif//WORKER_H_
