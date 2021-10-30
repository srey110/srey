#ifndef OVERLAPID_TCP_H_
#define OVERLAPID_TCP_H_

#include "evtype.h"

#if defined(OS_WIN)

typedef struct netev_ctx
{
    HANDLE iocp;
    int32_t threadcnt;
    struct thread_ctx *thread;
    BOOL(WINAPI *acceptex)(SOCKET, SOCKET, PVOID, DWORD, DWORD, DWORD, LPDWORD, LPOVERLAPPED);
    BOOL(WINAPI *connectex)(SOCKET, const struct sockaddr *, int, PVOID, DWORD, LPDWORD, LPOVERLAPPED);
    void (WINAPI *acceptaddrsex)(PVOID, DWORD, DWORD, DWORD, LPSOCKADDR *, LPINT, LPSOCKADDR *, LPINT);
}netev_ctx;
typedef struct overlap_ctx
{
    OVERLAPPED overlapped;
    void(*overlap_cb)(struct netev_ctx *piocpctx, struct overlap_ctx *polctx, const uint32_t uibyte, const int32_t ierr);
    SOCKET sock;
}overlap_ctx;

SOCKET netev_listener(struct netev_ctx *piocpctx, struct chan_ctx *pchan,
    const uint8_t uchancnt, const char *phost, const uint16_t usport);
SOCKET netev_connecter(struct netev_ctx *piocpctx, struct chan_ctx *pchan,
    const char *phost, const uint16_t usport);
int32_t netev_addsock(struct netev_ctx *piocpctx, SOCKET fd);
int32_t netev_enable_rw(SOCKET fd, struct chan_ctx *pchan, struct buffer_ctx *precvbuf);

#endif
#endif//OVERLAPID_TCP_H_

