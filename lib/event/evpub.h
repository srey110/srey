#ifndef EVPUB_H_
#define EVPUB_H_

#include "utils.h"
#include "queue.h"
#include "mutex.h"
#include "buffer.h"

#define INIT_EVENTS_CNT         512
#define INIT_SENDBUF_LEN        32

//用户数据
typedef struct ud_cxt
{
    void *data;
}ud_cxt;
typedef struct bufs_ctx
{
    void *data;
    size_t len;
    size_t offset;
}bufs_ctx;
QUEUE_DECL(bufs_ctx, qu_bufs);

QUEUE_DECL(struct listener_ctx *, qu_lsn);
typedef struct ev_ctx
{
    volatile int32_t stop;
    uint32_t nthreads;
    struct watcher_ctx *watcher;
#ifdef EV_IOCP
    struct worker_ctx *worker;
#endif
    qu_lsn qulsn;
    mutex_ctx qulsnlck;
}ev_ctx;

//回调函数 accept_cb connect_cb 返回失败则不加进事件循环
typedef int32_t(*accept_cb)(ev_ctx *ev, SOCKET fd, ud_cxt *ud);
typedef int32_t(*connect_cb)(ev_ctx *ev, SOCKET fd, ud_cxt *ud);//sock INVALID_SOCK 失败
typedef void(*recv_cb)(ev_ctx *ev, SOCKET fd, buffer_ctx *buf, size_t size, ud_cxt *ud);
typedef void(*send_cb)(ev_ctx *ev, SOCKET fd, size_t size, ud_cxt *ud);
typedef void(*close_cb)(ev_ctx *ev, SOCKET fd, ud_cxt *ud);
typedef struct cbs_ctx
{
    accept_cb acp_cb;
    connect_cb conn_cb;
    recv_cb r_cb;
    close_cb c_cb;
    send_cb s_cb;
}cbs_ctx;

#define FD_HASH(fd) hash((const char *)&(fd), sizeof(fd))
#define COPY_UD(dst, src)\
do {\
    if (NULL != (src)){\
        (dst) = *(src);\
    }else{\
        ZERO(&(dst), sizeof(ud_cxt));\
    }\
} while (0)

#endif//EVPUB_H_
