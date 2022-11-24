#ifndef WORKER_H_
#define WORKER_H_

#include "event/evpub.h"

#ifdef EV_IOCP

struct worker_ctx *worker_init(ev_ctx *ev, uint32_t nthread, uint32_t nqus);
void worker_free(struct worker_ctx *worker);

struct runner_ctx *worker_get_runner(struct worker_ctx *worker, uint64_t hs);
struct sock_ctx *runner_newsk(struct runner_ctx *runner, SOCKET fd, cbs_ctx *cbs, ud_cxt *ud);
void runner_freesk(struct runner_ctx *runner, struct sock_ctx *skctx);
void runner_addsk(struct runner_ctx *runner, SOCKET fd, struct sock_ctx *skctx, uint64_t hs);
void runner_removesk(struct runner_ctx *runner, SOCKET fd, uint64_t hs);
void worker_removesk(struct worker_ctx *worker, SOCKET fd);

int32_t _check_canfree(struct sock_ctx *skctx);
void _add_bufs_trypost(struct sock_ctx *skctx, bufs_ctx *buf);
void _add_bufs_trysendto(struct sock_ctx *skctx, bufs_ctx *buf);
void _free_udp(struct sock_ctx *skctx);
int32_t _sock_type(struct sock_ctx *skctx);
void _sk_shutdown(struct sock_ctx *skctx);

#endif//EV_IOCP
#endif//WORKER_H_
