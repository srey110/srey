#ifndef UEV_H_
#define UEV_H_

#include "event/event.h"
#include "event/skpool.h"
#include "thread.h"
#include "mutex.h"

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
typedef enum UEV_CMDS
{
    CMD_STOP = 0x00,
    CMD_DISCONN,
    CMD_LSN,
    CMD_CONN,
    CMD_ADD,
    CMD_SEND,

    CMD_TOTAL,
}UEV_CMDS;
typedef struct cmd_ctx
{
    int32_t cmd;
    SOCKET fd;
    void *data;
    size_t len;
}cmd_ctx;
typedef struct map_element
{
    SOCKET fd;
    struct sock_ctx *sock;
}map_element;
typedef struct watcher_ctx
{
    int32_t index;
    int32_t evfd; 
    int32_t nevent;
    uint32_t npipes;
#if defined(EV_KQUEUE)
    int32_t nsize;//当前 changelist 数量
    int32_t nchange;//实际数量
    events_t *changelist;
#endif
    events_t *events;
    struct pip_ctx *pipes;
    ev_ctx *ev;
    struct hashmap *element;
    pthread_t thevent;
    skpool_ctx pool;
}watcher_ctx;

typedef void(*event_cb)(watcher_ctx *watcher, struct sock_ctx *skctx, int32_t ev, int32_t *stop);
typedef struct sock_ctx
{
    SOCKET fd;
    int32_t type;
    int32_t events;
    event_cb ev_cb;
}sock_ctx;

int32_t _add_event(watcher_ctx *watcher, SOCKET fd, int32_t *events, int32_t ev, void *arg);
void _del_event(watcher_ctx *watcher, SOCKET fd, int32_t *events, int32_t ev, void *arg);

void _cmd_send(watcher_ctx *watcher, uint32_t index, cmd_ctx *cmd);
void _cmd_connect(ev_ctx *ctx, SOCKET fd, sock_ctx *skctx);
void _cmd_listen(watcher_ctx *watcher, SOCKET fd, sock_ctx *skctx);
void _cmd_add(watcher_ctx *watcher, SOCKET fd, uint64_t hs, cbs_ctx *cbs, ud_cxt *ud);

void _on_cmd_stop(watcher_ctx *watcher, cmd_ctx *cmd, int32_t *stop);
void _on_cmd_disconn(watcher_ctx *watcher, cmd_ctx *cmd, int32_t *stop);
void _on_cmd_lsn(watcher_ctx *watcher, cmd_ctx *cmd, int32_t *stop);
void _on_cmd_conn(watcher_ctx *watcher, cmd_ctx *cmd, int32_t *stop);
void _on_cmd_send(watcher_ctx *watcher, cmd_ctx *cmd, int32_t *stop);
void _on_cmd_add(watcher_ctx *watcher, cmd_ctx *cmd, int32_t *stop);
void _add_inloop(watcher_ctx *watcher, SOCKET fd, cbs_ctx *cbs, ud_cxt *ud);

qu_bufs *_get_send_bufs(sock_ctx *skctx);
connect_cb _get_conn_cb(sock_ctx *skctx, ud_cxt *ud);
void _on_close(watcher_ctx *watcher, sock_ctx *skctx, int32_t remove);

#endif//EV_IOCP
#endif//UEV_H_
