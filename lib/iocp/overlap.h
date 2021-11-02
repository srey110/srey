#ifndef OVERLAPID_TCP_H_
#define OVERLAPID_TCP_H_

#include "evtype.h"

#if defined(OS_WIN)

typedef struct netev_ctx
{
    HANDLE iocp;
    int32_t thcnt;
    struct thread_ctx *thiocp;
    BOOL(WINAPI *acceptex)(SOCKET, SOCKET, PVOID, DWORD, DWORD, DWORD, LPDWORD, LPOVERLAPPED);
    BOOL(WINAPI *connectex)(SOCKET, const struct sockaddr *, int, PVOID, DWORD, LPDWORD, LPOVERLAPPED);
    void (WINAPI *acceptaddrsex)(PVOID, DWORD, DWORD, DWORD, LPSOCKADDR *, LPINT, LPSOCKADDR *, LPINT);
    BOOL(WINAPI *disconnectex)(SOCKET, LPOVERLAPPED, DWORD, DWORD);
}netev_ctx;
typedef struct overlap_ctx
{
    OVERLAPPED overlapped;
    void(*overlap_cb)(struct netev_ctx *piocpctx, struct overlap_ctx *polctx, const uint32_t uibyte, const int32_t ierr);
    SOCKET sock;
}overlap_ctx;

SOCKET netev_listener(struct netev_ctx *piocpctx, struct chan_ctx *pchan,
    const uint32_t uichancnt, const char *phost, const uint16_t usport);
int32_t netev_addsock(struct netev_ctx *piocpctx, SOCKET fd);
SOCKET netev_connecter(struct netev_ctx *piocpctx, struct chan_ctx *pchan,
    const char *phost, const uint16_t usport);
struct sock_ctx *netev_enable_rw(struct netev_ctx *piocpctx, SOCKET fd, 
    struct chan_ctx *pchan, const int32_t ipostsendev);

void sock_change_chan(struct sock_ctx *psockctx, struct chan_ctx *pchan);
struct buffer_ctx *sock_recvbuf(struct sock_ctx *psockctx);
struct buffer_ctx *sock_sendbuf(struct sock_ctx *psockctx);
SOCKET sock_handle(struct sock_ctx *psockctx);
void sock_close(struct sock_ctx *psockctx);

int32_t tcp_send(struct sock_ctx *psockctx, void *pdata, const size_t uilens);
int32_t tcp_send_buf(struct sock_ctx *psockctx);
int32_t udp_send(struct sock_ctx *psockctx, void *pdata, const size_t uilens,
    const char *pip, const uint16_t uport);

int32_t _sock_can_free(struct sock_ctx *psockctx);
void _sock_free(struct sock_ctx *psockctx);

#endif
#endif//OVERLAPID_TCP_H_

