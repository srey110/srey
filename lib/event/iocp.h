#ifndef IOCP_H_
#define IOCP_H_

#include "event/event.h"

#ifdef EV_IOCP

typedef void(*event_cb)(struct watcher_ctx *ctx, DWORD bytes, struct sock_ctx *sock);
typedef struct sock_ctx
{
    OVERLAPPED overlapped;
    SOCKET sock;
    HANDLE iocp;
    event_cb ev_cb;
}sock_ctx;
typedef struct watcher_ctx
{
    HANDLE iocp;
    ev_ctx *ev;
    pthread_t thevent;
}watcher_ctx;
typedef struct exfuncs_ctx
{
    BOOL(WINAPI *acceptex)(SOCKET, SOCKET, PVOID, DWORD, DWORD, DWORD, LPDWORD, LPOVERLAPPED);
    BOOL(WINAPI *connectex)(SOCKET, const struct sockaddr *, int, PVOID, DWORD, LPDWORD, LPOVERLAPPED);
    BOOL(WINAPI *disconnectex)(SOCKET, LPOVERLAPPED, DWORD, DWORD);
}exfuncs_ctx;
extern exfuncs_ctx _exfuncs;

#endif//EV_IOCP
#endif //IOCP_H_
