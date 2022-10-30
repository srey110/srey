#include "loger.h"
#include "utils.h"
#include "netutils.h"
#include "thread.h"
#include "netaddr.h"
#include "buffer.h"

#define MAX_RECV_IOV_SIZE       4096
#define MAX_RECV_IOV_COUNT      4
#define MAX_SEND_IOV_SIZE       4096
#define MAX_SEND_IOV_COUNT      16
#define DELAYFREE_TIME          10
#define MAX_DELAYFREE_CNT       20
#define EV_READ                 0x01
#define EV_WRITE                0x02
#define EV_CLOSE                0x04
#define EV_ACCEPT               0x08
#define EV_CONNECT              0x10

#ifndef NETEV_IOCP
    #define _FLAGS_LSN   0x01
    #define _FLAGS_CONN  0x02
    #define _FLAGS_NORM  0x04
    #define _FLAGS_CLOSE 0x08
    #if defined(NETEV_EPOLL)
        typedef struct epoll_event events_t;
    #elif defined(NETEV_KQUEUE)
        typedef struct kevent events_t;
    #elif defined(NETEV_EVPORT)
        typedef port_event_t events_t;
    #endif
#endif

struct watcher_ctx
{
#if defined(NETEV_IOCP)
    HANDLE iocp;
    int32_t err;
    DWORD bytes;
    BOOL(WINAPI *acceptex)(SOCKET, SOCKET, PVOID, DWORD, DWORD, DWORD, LPDWORD, LPOVERLAPPED);
    BOOL(WINAPI *connectex)(SOCKET, const struct sockaddr *, int, PVOID, DWORD, LPDWORD, LPOVERLAPPED);
    BOOL(WINAPI *disconnectex)(SOCKET, LPOVERLAPPED, DWORD, DWORD);
#else
    int32_t evfd;
    int32_t event_cnt;
    int32_t pipes[2];
    events_t *events;
    struct map_ctx *map;
    mutex_ctx lock_cmd;
    struct queue_ctx qu_cmd;
    struct queue_ctx qu_close;
#endif
    struct netev_ctx *netev;
    struct thread_ctx thev;
};
struct netev_ctx
{
    int32_t thcnt;
    struct tw_ctx *tw;
    uint64_t(*id_creater)(void *);
    void *id_data;
    struct watcher_ctx *watcher;
};
struct sock_ctx
{
#ifdef NETEV_IOCP
    OVERLAPPED overlapped;
#else
    int32_t flags;//±ê¼Ç
#endif
    uint32_t events;
    SOCKET sock;
    uint64_t id;
    void(*ev_cb)(struct watcher_ctx *, struct sock_ctx *, uint32_t, int32_t *);
};

