#ifndef EVENT_H_
#define EVENT_H_

#include "thread.h"
#include "buffer.h"
#include "netaddr.h"
#include "netutils.h"
#include "loger.h"
#include "utils.h"
#include "queue.h"

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
    uint32_t nthreads;
#ifdef EV_IOCP
    HANDLE iocp;
    BOOL(WINAPI *acceptex)(SOCKET, SOCKET, PVOID, DWORD, DWORD, DWORD, LPDWORD, LPOVERLAPPED);
    BOOL(WINAPI *connectex)(SOCKET, const struct sockaddr *, int, PVOID, DWORD, LPDWORD, LPOVERLAPPED);
    BOOL(WINAPI *disconnectex)(SOCKET, LPOVERLAPPED, DWORD, DWORD);
#endif
    qu_lsn qulsn;
    struct watcher_ctx *watcher;
}ev_ctx;

//data 发送的数据  len 数据长度  ERR_FAILED失败
typedef void(*accept_cb)(SOCKET sock, void *ud);
typedef void(*close_cb)(SOCKET sock, void *ud);
typedef void(*recv_cb)(SOCKET sock, buffer_ctx *buf, void *ud);
typedef void(*send_cb)(SOCKET sock, void *data, size_t len, void *ud, int32_t rest);

void ev_init(ev_ctx *ctx, uint32_t nthreads);
void ev_free(ev_ctx *ctx);
int32_t ev_loop(ev_ctx *ctx, SOCKET sock, recv_cb r_cb, close_cb c_cb, send_cb s_cb, void *ud);
int32_t ev_listener(ev_ctx *ctx, const char *host, const uint16_t port, accept_cb cb, void *ud);

void ev_send(ev_ctx *ctx, SOCKET sock, void *data, size_t len, char copy, void *ud);
void ev_close(ev_ctx *ctx, SOCKET sock);

SOCKET ev_sock(int32_t family);
SOCKET ev_listen(netaddr_ctx *addr);

#endif //EVENT_H_
