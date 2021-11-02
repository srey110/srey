#ifndef OVERLAPID_TCP_H_
#define OVERLAPID_TCP_H_

#include "evtype.h"

#if defined(OS_WIN)
typedef struct overlap_ctx
{
    OVERLAPPED overlapped;
    void(*overlap_cb)(struct netev_ctx *piocpctx, struct overlap_ctx *polctx, const uint32_t uibyte, const int32_t ierr);
    SOCKET sock;
}overlap_ctx;
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
#endif
#endif//OVERLAPID_TCP_H_

