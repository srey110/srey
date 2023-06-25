#ifndef EVPUB_H_
#define EVPUB_H_

#include "utils.h"
#include "queue.h"
#include "mutex.h"
#include "buffer.h"
#include "netaddr.h"
#include "structs.h"

typedef enum syn_type {
    SYN_NONE = 0x00,
    SYN_ONCE,
    SYN_SLICE,
    SYN_ENDSLICE
}syn_type;
QUEUE_DECL(off_buf_ctx, qu_off_buf);
QUEUE_DECL(struct listener_ctx *, qu_lsn);
typedef struct ev_ctx {
    uint32_t nthreads;
#ifdef EV_IOCP
    uint32_t nacpex;
    struct acceptex_ctx *acpex;
#endif
    struct watcher_ctx *watcher;
    qu_lsn qulsn;
    mutex_ctx qulsnlck;
}ev_ctx;
struct evssl_ctx;

//回调函数 accept_cb connect_cb 返回失败则不加进事件循环
typedef int32_t(*accept_cb)(ev_ctx *ev, SOCKET fd, ud_cxt *ud);
typedef int32_t(*connect_cb)(ev_ctx *ev, SOCKET fd, int32_t err, ud_cxt *ud);
typedef void(*recv_cb)(ev_ctx *ev, SOCKET fd, buffer_ctx *buf, size_t size, ud_cxt *ud);
typedef void(*recvfrom_cb)(ev_ctx *ev, SOCKET fd, char *buf, size_t size, netaddr_ctx *addr, ud_cxt *ud);
typedef void(*send_cb)(ev_ctx *ev, SOCKET fd, size_t size, ud_cxt *ud);
typedef void(*close_cb)(ev_ctx *ev, SOCKET fd, ud_cxt *ud);
typedef void(*free_udcb)(ud_cxt *ud);
typedef struct cbs_ctx {
    accept_cb acp_cb;
    connect_cb conn_cb;
    recv_cb r_cb;
    recvfrom_cb rf_cb;
    close_cb c_cb;
    send_cb s_cb;
    free_udcb ud_free;
}cbs_ctx;

#define FD_HASH(fd) hash((const char *)&(fd), sizeof(fd))
#define GET_PTR(p, n, hs) ((1 == (n)) ? (p) : &((p)[(hs) % (n)]))
#define GET_POS(hs, n) ((hs) % (n))

//公共函数
void _bufs_clear(qu_off_buf *bufs);
int32_t _set_sockops(SOCKET fd);
//SOCK_DGRAM  SOCK_STREAM  AF_INET  AF_INET6
SOCKET _create_sock(int32_t type, int32_t family);
SOCKET _listen(netaddr_ctx *addr);
SOCKET _udp(netaddr_ctx *addr);
int32_t _sock_read(SOCKET fd, IOV_TYPE *iov, uint32_t niov, void *arg, size_t *readed);
int32_t _sock_send(SOCKET fd, qu_off_buf *buf_s, size_t *nsend, void *arg);
uint64_t _sock_id(void);
void _set_ud_typstat_cmd(char *typsta, int8_t pktype, int8_t status);
void _set_ud_typstat(char *typsta, ud_cxt *ud);
void _send_result(ud_cxt *ud, uint64_t sess, int32_t err);

#endif//EVPUB_H_
