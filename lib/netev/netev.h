#ifndef NETEV_H_
#define NETEV_H_

#include "mutex.h"
#include "loger.h"
#include "utils.h"
#include "netutils.h"
#include "thread.h"
#include "evtype.h"
#include "netaddr.h"
#include "buffer.h"
#include "queue.h"
#include "tw.h"

#if defined(OS_WIN)
    #define NETEV_IOCP
#elif defined(OS_LINUX)
    #define NETEV_EPOLL
#elif defined(OS_BSD)
    #define NETEV_KQUEUE
#elif defined(OS_SUN)
    #define NETEV_EVPORT
#endif

#define MAX_RECVFROM_IOV_SIZE   4096
#define MAX_RECV_IOV_SIZE       4096
#define MAX_RECV_IOV_COUNT      4
#define MAX_SEND_IOV_SIZE       4096
#define MAX_SEND_IOV_COUNT      16
#ifndef NETEV_IOCP
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
    SOCKET socks[2];
    events_t *events;
    mutex_ctx lock_qucmd;
    struct queue_ctx qu_cmd;
    struct queue_ctx qu_close;
    char trigger[ONEK];
#endif
    struct netev_ctx *netev;
    struct thread_ctx thev;
};
struct udp_msg_ctx
{
    uint16_t port;
    size_t size;
    char ip[IP_LENS];
};
struct netev_ctx
{
    volatile atomic_t id;
    int32_t thcnt;
    struct tw_ctx *tw;
    struct watcher_ctx *watcher;
};
struct sock_ctx
{
#ifdef NETEV_IOCP
    OVERLAPPED overlapped;
#else
    int32_t flags;//±ê¼Ç
#endif
    int32_t events;
    uint32_t id;
    SOCKET sock;
    void(*ev_cb)(struct watcher_ctx *, struct sock_ctx *, int32_t, int32_t *);
};
typedef void(*accept_cb)(struct sock_ctx *, void *);
typedef void(*connect_cb)(struct sock_ctx *, int32_t, void *);
typedef void(*read_cb)(struct sock_ctx *, struct buffer_ctx *, size_t, const char*, uint16_t, void *);
typedef void(*write_cb)(struct sock_ctx *, size_t, void *);
typedef void(*close_cb)(struct sock_ctx *, void *);

struct netev_ctx *netev_new(struct tw_ctx *ptw, const uint32_t uthcnt);
void netev_free(struct netev_ctx *pctx);
void netev_loop(struct netev_ctx *pctx); 

struct listener_ctx *netev_listener(struct netev_ctx *pctx,
    const char *phost, const uint16_t usport, accept_cb acp_cb, void *pdata);
struct sock_ctx *netev_add_sock(struct netev_ctx *pctx, SOCKET sock, int32_t itype, int32_t ifamily);
struct sock_ctx *netev_connecter(struct netev_ctx *pctx, uint32_t utimeout,
    const char *phost, const uint16_t usport, connect_cb conn_cb, void *pdata);
int32_t netev_enable_rw(struct netev_ctx *pctx, struct sock_ctx *psock,
    read_cb r_cb, write_cb w_cb, close_cb c_cb, void *pdata);

uint32_t sock_id(struct sock_ctx *psock);
SOCKET sock_handle(struct sock_ctx *psock);
int32_t sock_type(struct sock_ctx *psock);
struct buffer_ctx *sock_buffer_r(struct sock_ctx *psock);
struct buffer_ctx *sock_buffer_w(struct sock_ctx *psock);
int32_t sock_send(struct sock_ctx *psock, void *pdata, const size_t uilens);
int32_t sock_send_buffer(struct sock_ctx *psock);
int32_t sock_sendto(struct sock_ctx *psock, const char *phost, uint16_t uport, 
    void *pdata, const size_t uilens);
void sock_close(struct sock_ctx *psock);
void sock_free(struct sock_ctx *psock);
void listener_free(struct listener_ctx *plsn);
//AF_INET  AF_INET6    SOCK_STREAM  SOCK_DGRAM
static inline SOCKET sock_create(int32_t ifamily, int32_t itype)
{
#ifdef NETEV_IOCP
    return WSASocket(ifamily, itype, SOCK_STREAM == itype ? IPPROTO_TCP : IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED);
#else
    return socket(ifamily, itype, 0);
#endif
};
SOCKET sock_listen(union netaddr_ctx *paddr);
SOCKET sock_udp_bind(const char *phost, const uint16_t usport);

int32_t _netev_threadcnt(const uint32_t uthcnt);
struct watcher_ctx *_netev_get_watcher(struct netev_ctx *pctx, SOCKET fd);
void _netev_add(struct watcher_ctx *pwatcher, struct sock_ctx *psock, int32_t iev);
#ifndef NETEV_IOCP
int32_t _uev_add(struct watcher_ctx *pwatcher, struct sock_ctx *psock, int32_t iev);
void _uev_del(struct watcher_ctx *pwatcher, struct sock_ctx *psock, int32_t iev);
void _uev_cmd_close(struct watcher_ctx *pwatcher, struct sock_ctx *psock);
void _add_close_qu(struct watcher_ctx *pwatcher, struct sock_ctx *psock);
void _uev_sock_close(struct sock_ctx *psock);
#endif
static inline size_t _udp_data_lens(void *pdata)
{
    return ((struct udp_msg_ctx *)pdata)->size;
}

#endif//NETEV_H_
