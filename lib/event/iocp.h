#ifndef IOCP_H_
#define IOCP_H_

#include "event/event.h"
#include "mutex.h"
#include "cond.h"

#ifdef EV_IOCP

#define CMD_STOP         0x01
#define CMD_ADD          0x02
#define CMD_REMOVE       0x03
#define CMD_SEND         0x04

typedef struct sock_ctx
{
    OVERLAPPED overlapped;
    int32_t flags;//±ê¼Ç    
    SOCKET sock;
    void(*ev_cb)(ev_ctx *ctx, int32_t err, DWORD bytes, struct sock_ctx *sock);
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
    thread_ctx thread;
    sender_ctx sender;
}watcher_ctx;

int32_t _iocp_send(ev_ctx *ctx, SOCKET sock, send_cb cb, void *data, size_t len, void *ud);
void _iocp_freelsn(struct listener_ctx *lsn);
void _sender_add(ev_ctx *ctx, SOCKET sock, send_cb cb);
void _sender_remove(ev_ctx *ctx, SOCKET sock);

#endif//EV_IOCP
#endif //IOCP_H_
