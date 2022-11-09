#ifndef IOCP_H_
#define IOCP_H_

#include "event/event.h"

#ifdef EV_IOCP

#define CMD_STOP         0x01
#define CMD_ADD          0x02
#define CMD_REMOVE       0x03
#define CMD_SEND         0x04
#define CMD_CLOSE        0x05

typedef void(*event_cb)(ev_ctx *ctx, DWORD bytes, struct sock_ctx *sock);
typedef struct sock_ctx
{
    OVERLAPPED overlapped;
    SOCKET sock;
    HANDLE wait_handle;
    HANDLE ev_handle;
    ev_ctx *ev;
    event_cb ev_cb;
}sock_ctx;
typedef struct sender_ctx
{
    int32_t wait;
    ev_ctx *ev;
    qu_cmd qucmd;
    cond_ctx cond;
    mutex_ctx mutex;
    thread_ctx thsend;
}sender_ctx;
typedef struct watcher_ctx
{
    thread_ctx thevent;
    sender_ctx sender;
}watcher_ctx;
typedef struct exfuncs_ctx
{
    BOOL(WINAPI *acceptex)(SOCKET, SOCKET, PVOID, DWORD, DWORD, DWORD, LPDWORD, LPOVERLAPPED);
    BOOL(WINAPI *connectex)(SOCKET, const struct sockaddr *, int, PVOID, DWORD, LPDWORD, LPOVERLAPPED);
    BOOL(WINAPI *disconnectex)(SOCKET, LPOVERLAPPED, DWORD, DWORD);
}exfuncs_ctx;
extern exfuncs_ctx _exfuncs;

void _freelsn(struct listener_ctx *lsn);
watcher_ctx *_get_watcher(ev_ctx *ctx, SOCKET fd);
void _send_cmd(watcher_ctx *watcher, cmd_ctx *cmd);
void _cmd_add(ev_ctx *ctx, SOCKET sock, send_cb cb, void *ud);
void _cmd_remove(ev_ctx *ctx, SOCKET sock);
int32_t _post_send(ev_ctx *ctx, SOCKET sock, send_cb cb, void *data, size_t len, void *ud);
void _post_disconn(ev_ctx *ctx, SOCKET sock);

#endif//EV_IOCP
#endif //IOCP_H_
