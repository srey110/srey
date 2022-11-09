#ifndef EVENT_H_
#define EVENT_H_

#include "buffer.h"
#include "netaddr.h"
#include "netutils.h"
#include "loger.h"
#include "utils.h"
#include "queue.h"

#define INIT_EVENTS_CNT         512
#define MAX_RECV_IOV_SIZE       ONEK  * 4

typedef struct cmd_ctx
{
    char cmd;
    SOCKET sock;
    void *data;
    void *ud;
    size_t len;
}cmd_ctx;
QUEUE_DECL(cmd_ctx, qu_cmd);
QUEUE_DECL(struct listener_ctx *, qu_lsn);
typedef struct ev_ctx
{
    volatile int32_t stop;
    uint32_t nthreads;
#ifdef EV_IOCP
    HANDLE iocp;
#endif
    mutex_ctx mulsn;
    qu_lsn qulsn;
    struct watcher_ctx *watcher;
}ev_ctx;

typedef void(*accept_cb)(ev_ctx *ctx, SOCKET sock, void *ud);
typedef void(*close_cb)(ev_ctx *ctx, SOCKET sock, void *ud);
typedef void(*recv_cb)(ev_ctx *ctx, SOCKET sock, buffer_ctx *buf, size_t len, void *ud);
typedef void(*connect_cb)(ev_ctx *ctx, SOCKET sock, void *ud);//sock INVALID_SOCK Ê§°Ü
typedef void(*send_cb)(ev_ctx *ctx, SOCKET sock, size_t len, void *ud);

void ev_init(ev_ctx *ctx, uint32_t nthreads);
void ev_free(ev_ctx *ctx);

int32_t ev_listener(ev_ctx *ctx, const char *host, const uint16_t port, accept_cb cb, void *ud);
int32_t ev_connecter(ev_ctx *ctx, const char *host, const uint16_t port, connect_cb conn_cb, void *ud);

int32_t ev_loop(ev_ctx *ctx, SOCKET sock, recv_cb r_cb, close_cb c_cb, send_cb s_cb, void *ud);
void ev_send(ev_ctx *ctx, SOCKET sock, void *data, size_t len);
void ev_close(ev_ctx *ctx, SOCKET sock);

SOCKET _ev_sock(int32_t family);
SOCKET _ev_listen(netaddr_ctx *addr);

#endif //EVENT_H_
