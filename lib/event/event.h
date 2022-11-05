#ifndef EVENT_H_
#define EVENT_H_

#include "thread.h"
#include "buffer.h"
#include "queue.h"
#include "netaddr.h"
#include "netutils.h"
#include "loger.h"
#include "utils.h"

#if defined(EV_IOCP)
    typedef HANDLE evhandle_t;
#else
    typedef int32_t evhandle_t;
    typedef struct cmd_ctx
    {
        int32_t cmd;
        struct sock_ctx *sock;
    }cmd_ctx;
    QUEUE_DECL(cmd_ctx, qu_cmd);
    QUEUE_DECL(struct sock_ctx *, qu_close);
    #if defined(EV_EPOLL)
        typedef struct epoll_event events_t;
    #elif defined(EV_KQUEUE)
        typedef struct kevent events_t;
    #elif defined(EV_EVPORT)
        typedef port_event_t events_t;
    #endif
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
#endif
    int32_t flags;//±ê¼Ç
    uint32_t events;
    SOCKET sock;
    uint64_t id;
    void(*ev_cb)(watcher_ctx *watcher, struct sock_ctx *sock, uint32_t ev, int32_t *stop);
}sock_ctx;
typedef struct ev_ctx
{
    uint32_t nthreads;
    watcher_ctx *watcher;
}ev_ctx;

void ev_init(ev_ctx *ctx, uint32_t nthreads);
void ev_free(ev_ctx *ctx);

#endif //EVENT_H_
