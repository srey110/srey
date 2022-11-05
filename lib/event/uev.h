#ifndef UEV_H_
#define UEV_H_

#include "event/event.h"

#ifndef EV_IOCP

#define EV_READ          0x01
#define EV_WRITE         0x02

#if defined(EV_EPOLL)
typedef struct epoll_event events_t;
#elif defined(EV_KQUEUE)
typedef struct kevent events_t;
#elif defined(EV_EVPORT)
typedef port_event_t events_t;
#endif

typedef struct watcher_ctx
{
    evhandle_t evhandle;
#ifdef EV_IOCP
    int32_t err;
    DWORD bytes;
    BOOL(WINAPI *acceptex)(SOCKET, SOCKET, PVOID, DWORD, DWORD, DWORD, LPDWORD, LPOVERLAPPED);
    BOOL(WINAPI *connectex)(SOCKET, const struct sockaddr *, int, PVOID, DWORD, LPDWORD, LPOVERLAPPED);
    BOOL(WINAPI *disconnectex)(SOCKET, LPOVERLAPPED, DWORD, DWORD);
#else
    int32_t nevent;
    events_t *events;
    int32_t pipes[2];
    mutex_ctx qucmdlck;
    qu_cmd qucmd;
    qu_close quclose;
#endif
    struct ev_ctx *ev;
    thread_ctx thread;
}watcher_ctx;

typedef struct sock_ctx
{
#ifdef EV_IOCP
    OVERLAPPED overlapped;
#else
    uint32_t events;
#endif
    int32_t flags;//±ê¼Ç    
    SOCKET sock;
    uint64_t id;
    void(*ev_cb)(watcher_ctx *watcher, struct sock_ctx *sock, uint32_t ev, int32_t *stop);
}sock_ctx;

watcher_ctx * _uev_watcher(ev_ctx *ctx, SOCKET fd);
void _uev_cmd(watcher_ctx *watcher, int32_t cmd, sock_ctx *sock);
int32_t _add_event(watcher_ctx *watcher, sock_ctx *sock, uint32_t ev);
void _del_event(watcher_ctx *watcher, sock_ctx *sock, uint32_t ev);

#endif
#endif //UEV_H_
