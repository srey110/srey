#ifndef UEV_H_
#define UEV_H_

#include "event/event.h"

#ifndef EV_IOCP

#if defined(EV_EPOLL)
typedef struct epoll_event events_t;
#elif defined(EV_KQUEUE)
typedef struct kevent events_t;
#elif defined(EV_EVPORT)
typedef port_event_t events_t;
#endif

typedef enum EVENTS
{
    EVENT_READ = 0x01,
    EVENT_WRITE = 0x02,
}EVENTS;
typedef enum SOCK_FLAGS
{
    FLAG_LSN = 0,
    FLAG_CONN,
    FLAG_CMD,
    FLAG_RW,

    FLAG_TOTAL,
}SOCK_FLAGS;
typedef struct sock_ctx
{
    SOCKET sock;
    int32_t events;
    int32_t flag;//标记
}sock_ctx;
typedef struct pipcmd_ctx
{
    int32_t cmd;
    sock_ctx *sock;
}pipcmd_ctx;
QUEUE_DECL(pipcmd_ctx, pip_cmd);
QUEUE_DECL(SOCKET, qu_error);
typedef struct watcher_ctx
{
    int32_t evfd;
    int32_t pipes[2];
    int32_t ncmd;
#if defined(EV_KQUEUE)
    int32_t nsize;//当前 changelist 数量
    int32_t nchange;//实际数量
    events_t *changelist;
#endif
    int32_t nevent;
    events_t *events;
    ev_ctx *ev;
    pthread_t thevent;    
    pip_cmd pipcmd;
    mutex_ctx pipcmdlck;
}watcher_ctx;

int32_t _add_event(watcher_ctx *watcher, SOCKET fd, int32_t *events, int32_t ev, void *arg);
void _del_event(watcher_ctx *watcher, SOCKET fd, int32_t *events, int32_t ev, void *arg);
void _pipcmd_send(watcher_ctx *watcher, pipcmd_ctx *cmd);

void _on_accept_cb(watcher_ctx *watcher, sock_ctx *skctx, int32_t ev);
void _on_connect_cb(watcher_ctx *watcher, sock_ctx *skctx, int32_t ev);
void _on_cmd_cb(watcher_ctx *watcher, sock_ctx *skctx, int32_t ev);
void _on_cmd_rw(watcher_ctx *watcher, sock_ctx *skctx, int32_t ev);

connect_cb _get_connect_ud(sock_ctx *sock, ud_cxt **ud);
void _post_listen_rand(ev_ctx *ev, sock_ctx *sock);
void _post_listen(watcher_ctx *watcher, sock_ctx *sock);
void _post_connect(ev_ctx *ev, sock_ctx *sock);

#endif
#endif //UEV_H_
