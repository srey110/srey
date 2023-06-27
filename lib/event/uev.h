#ifndef UEV_H_
#define UEV_H_

#include "event/event.h"
#include "event/skpool.h"
#include "thread.h"
#include "mutex.h"

#ifndef EV_IOCP

#if defined(EV_EPOLL)
    typedef struct epoll_event events_t;
    #define EV_STR "EPOLL"
#elif defined(EV_KQUEUE)
    typedef struct kevent events_t;
    typedef struct kevent changes_t;
    #define EV_STR "KQUEUE"
    #define COMMIT_NCHANGES
#elif defined(EV_EVPORT)
    typedef port_event_t events_t;
    #define EV_STR "EVPORT"
    #define MANUAL_ADD
#elif defined(EV_POLLSET)
    typedef struct pollfd events_t;
    #define EV_STR "POLLSET"
    #define MANUAL_REMOVE
    #define NO_UDATA
#elif defined(EV_DEVPOLL)
    typedef struct pollfd events_t;
    typedef struct pollfd changes_t;
    #define EV_STR "DEVPOLL"
    #define MANUAL_REMOVE
    #define COMMIT_NCHANGES
    #define NO_UDATA
#endif

struct conn_ctx;
struct listener_ctx;
typedef enum EVENTS {
    EVENT_READ  = 0x01,
    EVENT_WRITE = 0x02,
}EVENTS;
typedef enum UEV_CMDS {
    CMD_STOP = 0x00,
    CMD_DISCONN,
    CMD_LSN,
    CMD_CONN,
    CMD_ADDACP,
    CMD_ADDUDP,
    CMD_SEND,
    CMD_SETUD_TYPSTAT,
    CMD_SETUD_DATA,

    CMD_TOTAL,
}UEV_CMDS;
typedef struct cmd_ctx {
    uint8_t cmd;
    uint8_t flag;
    SOCKET fd;
    void *data;
    size_t len;
    uint64_t skid;
    uint64_t sess;
}cmd_ctx;
typedef struct watcher_ctx {
    int32_t index;
    int32_t stop;
    int32_t evfd; 
    int32_t nevents;
    uint32_t npipes;
#ifdef COMMIT_NCHANGES
    int32_t nsize;//changes大小
    int32_t nchanges;//数量
    changes_t *changes;
#endif
    events_t *events;
    struct pip_ctx *pipes;
    ev_ctx *ev;
    struct hashmap *element;
    pthread_t thevent;
    skpool_ctx pool;
}watcher_ctx;

typedef void(*event_cb)(watcher_ctx *watcher, struct sock_ctx *skctx, int32_t ev);
typedef struct sock_ctx {
    SOCKET fd;
    int32_t type;
    int32_t events;
    event_cb ev_cb;
}sock_ctx;

int32_t _add_event(watcher_ctx *watcher, SOCKET fd, int32_t *events, int32_t ev, void *arg);
void _del_event(watcher_ctx *watcher, SOCKET fd, int32_t *events, int32_t ev, void *arg);

void _send_cmd(watcher_ctx *watcher, uint32_t index, cmd_ctx *cmd);
void _cmd_connect(ev_ctx *ctx, SOCKET fd, sock_ctx *skctx);
void _cmd_listen(watcher_ctx *watcher, SOCKET fd, sock_ctx *skctx);
void _cmd_add_acpfd(watcher_ctx *watcher, uint64_t hs, SOCKET fd, struct listener_ctx *lsn);
void _cmd_add_udp(ev_ctx *ctx, SOCKET fd, sock_ctx *skctx);

void _on_cmd_stop(watcher_ctx *watcher, cmd_ctx *cmd);
void _on_cmd_disconn(watcher_ctx *watcher, cmd_ctx *cmd);
void _on_cmd_lsn(watcher_ctx *watcher, cmd_ctx *cmd);
void _on_cmd_conn(watcher_ctx *watcher, cmd_ctx *cmd);
void _on_cmd_send(watcher_ctx *watcher, cmd_ctx *cmd);
void _on_cmd_addacp(watcher_ctx *watcher, cmd_ctx *cmd);
void _on_cmd_add_udp(watcher_ctx *watcher, cmd_ctx *cmd);
void _on_cmd_setud_typstat(watcher_ctx *watcher, cmd_ctx *cmd);
void _on_cmd_setud_data(watcher_ctx *watcher, cmd_ctx *cmd);

void _add_lsn_inloop(watcher_ctx *watcher, SOCKET fd, sock_ctx *skctx);
void _add_conn_inloop(watcher_ctx *watcher, SOCKET fd, sock_ctx *skctx);
void _add_acpfd_inloop(watcher_ctx *watcher, SOCKET fd, struct listener_ctx *lsn);
int32_t _add_write_inloop(watcher_ctx *watcher, sock_ctx *skctx, off_buf_ctx *buf, cmd_ctx *cmd);
void _add_udp_inloop(watcher_ctx *watcher, SOCKET fd, sock_ctx *skctx);

void _add_fd(watcher_ctx *watcher, sock_ctx *skctx);
sock_ctx *_map_getskctx(watcher_ctx *watcher, SOCKET fd);
void _sk_shutdown(sock_ctx *skctx);
void _free_udp(sock_ctx *skctx);
void _disconnect(watcher_ctx *watcher, sock_ctx *skctx, cmd_ctx *cmd);
void _setud_typstat(sock_ctx *skctx, char *typsta);
void _setud_data(sock_ctx *skctx, void *data);
void _freelsn(struct listener_ctx *lsn);
ud_cxt *_get_ud(sock_ctx *skctx);
int32_t _check_skid(sock_ctx *skctx, const uint64_t skid);

#endif//EV_IOCP
#endif//UEV_H_
