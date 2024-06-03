#ifndef EVPUB_H_
#define EVPUB_H_

#include "utils.h"
#include "ds/queue.h"
#include "ds/sarray.h"
#include "thread/spinlock.h"
#include "buffer.h"
#include "netaddr.h"
#include "structs.h"

typedef enum sock_status {
    STATUS_NONE = 0x00,
    STATUS_SENDING = 0x01,
    STATUS_ERROR = 0x02,
    STATUS_REMOVE = 0x04,
    STATUS_CLIENT = 0x08,
    STATUS_SSLEXCHANGE = 0x10,
    STATUS_AUTHSSL = 0x20,
}sock_status;
typedef struct ev_ctx {
    uint32_t nthreads;
#ifdef EV_IOCP
    uint32_t nacpex;
    struct acceptex_ctx *acpex;
#endif
    struct watcher_ctx *watcher;
    arr_ptr_ctx arrlsn;
    spin_ctx spin;
}ev_ctx;
struct evssl_ctx;

//回调函数 accept_cb connect_cb 返回失败则不加进事件循环
typedef int32_t(*accept_cb)(ev_ctx *ev, SOCKET fd, uint64_t skid, ud_cxt *ud);
typedef int32_t(*connect_cb)(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t err, ud_cxt *ud);
typedef void(*ssl_exchanged_cb)(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client, ud_cxt *ud);
typedef void(*recv_cb)(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client, buffer_ctx *buf, size_t size, ud_cxt *ud);
typedef void(*send_cb)(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client, size_t size, ud_cxt *ud);
typedef void(*close_cb)(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client, ud_cxt *ud);
typedef void(*recvfrom_cb)(ev_ctx *ev, SOCKET fd, uint64_t skid, char *buf, size_t size, netaddr_ctx *addr, ud_cxt *ud);
typedef struct cbs_ctx {
    accept_cb acp_cb;
    connect_cb conn_cb;
    ssl_exchanged_cb exch_cb;
    recv_cb r_cb;
    send_cb s_cb;
    close_cb c_cb;
    recvfrom_cb rf_cb;
    free_cb ud_free;
}cbs_ctx;

#define GET_POS(fd, n) (fd % n)
#define GET_PTR(p, n, fd) (1 == n ? p : &p[GET_POS(fd, n)])
QUEUE_DECL(off_buf_ctx, qu_off_buf);

//公共函数
void _bufs_clear(qu_off_buf_ctx *bufs);
int32_t _set_sockops(SOCKET fd);
//SOCK_DGRAM  SOCK_STREAM  AF_INET  AF_INET6
SOCKET _create_sock(int32_t type, int32_t family);
SOCKET _listen(netaddr_ctx *addr);
SOCKET _udp(netaddr_ctx *addr);
int32_t _sock_read(SOCKET fd, IOV_TYPE *iov, uint32_t niov, void *arg, size_t *readed);
int32_t _sock_send(SOCKET fd, qu_off_buf_ctx *buf_s, size_t *nsend, void *arg);
void _ev_set_ud(ev_ctx *ctx, SOCKET fd, uint64_t skid, int32_t type, uint64_t val);
void _set_ud(ud_cxt *ud, int32_t type, uint64_t val);

#endif//EVPUB_H_
