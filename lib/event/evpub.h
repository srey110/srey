#ifndef EVENT_PUB_H_
#define EVENT_PUB_H_

#include "queue.h"
#include "array.h"
#include "mutex.h"

#define INIT_EVENTS_CNT         512
#define MAX_RECV_SIZE           ONEK  * 4
#define MAX_SEND_SIZE           ONEK  * 4

typedef struct bufs_ctx
{
    void *data;
    size_t len;
    size_t offset;
}bufs_ctx;
QUEUE_DECL(bufs_ctx, qu_bufs);
QUEUE_DECL(struct listener_ctx *, qu_lsn);
typedef struct ud_cxt
{
    void *data;
}ud_cxt;
typedef struct ev_ctx
{
    volatile int32_t stop;
    uint32_t nthreads;
    mutex_ctx mulsn;
    qu_lsn qulsn;
    struct watcher_ctx *watcher;
    struct eworker_ctx *worker;
}ev_ctx;
#define COPY_UD(dst, src)\
do {\
    if (NULL != (src)){\
        (dst) = *(src);\
    }else{\
        ZERO(&(dst), sizeof(ud_cxt));\
    }\
} while (0)

typedef void(*accept_cb)(struct ev_ctx *ctx, SOCKET sock, ud_cxt *ud);
typedef void(*close_cb)(struct ev_ctx *ctx, SOCKET sock, ud_cxt *ud);
typedef void(*recv_cb)(struct ev_ctx *ctx, SOCKET sock, struct buffer_ctx *buf, size_t len, ud_cxt *ud);
typedef void(*connect_cb)(struct ev_ctx *ctx, SOCKET sock, ud_cxt *ud);//sock INVALID_SOCK Ê§°Ü
typedef void(*send_cb)(struct ev_ctx *ctx, SOCKET sock, size_t len, ud_cxt *ud);
typedef void(*free_ud)(ud_cxt *ud);

#endif//EVENT_PUB_H_
