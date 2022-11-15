#ifndef EVENT_H_ 
#define EVENT_H_

#include "event/evpub.h"
#include "netaddr.h"

//接口
void ev_init(ev_ctx *ctx, uint32_t nthreads);
void ev_free(ev_ctx *ctx);

int32_t ev_listen(ev_ctx *ctx, const char *host, const uint16_t port, cbs_ctx *cbs, ud_cxt *ud);
int32_t ev_connect(ev_ctx *ctx, const char *host, const uint16_t port, cbs_ctx *cbs, ud_cxt *ud);

void ev_send(ev_ctx *ctx, SOCKET fd, void *data, size_t len, int32_t copy);
void ev_close(ev_ctx *ctx, SOCKET fd);

void _freelsn(struct listener_ctx *lsn);

//公共函数
void _qu_bufs_clear(qu_bufs *bufs);
int32_t _set_sockops(SOCKET fd);
SOCKET _create_sock(int32_t family);
SOCKET _listen(netaddr_ctx *addr);
int32_t _sock_read(SOCKET fd, void *buf, size_t len, void *arg);
int32_t _sock_send(SOCKET fd, qu_bufs *sendbufs, mutex_ctx *lck, size_t *nsend, void *arg);

#endif//EVENT_H_
