#ifndef IOCP_H_
#define IOCP_H_

#include "event/event.h"
#include "event/skpool.h"
#include "thread.h"

#ifdef EV_IOCP

struct listener_ctx;
typedef enum WEV_CMDS {
    CMD_STOP = 0x00,
    CMD_DISCONN,
    CMD_ADD,
    CMD_ADDACP,
    CMD_REMOVE,
    CMD_SEND,
    CMD_SETUD_TYPSTAT,
    CMD_SETUD_DATA,

    CMD_TOTAL,
}WEV_CMDS;
typedef void(*event_cb)(void *arg, struct sock_ctx *skctx, DWORD bytes);
typedef struct sock_ctx {
    OVERLAPPED overlapped;
    int32_t type;
    SOCKET fd;
    event_cb ev_cb;
}sock_ctx;
typedef struct cmd_ctx {
    uint8_t cmd;
    uint8_t flag;
    SOCKET fd;
    void *data;
    size_t len;
    uint64_t skid;
    uint64_t sess;
}cmd_ctx;
QUEUE_DECL(cmd_ctx, qu_cmd);
typedef struct overlap_cmd_ctx {
    sock_ctx ol_r;
    DWORD bytes;
    DWORD flag;
    SOCKET fd;
    qu_cmd qu;
    mutex_ctx lck;
    IOV_TYPE wsabuf;
}overlap_cmd_ctx;
typedef struct watcher_ctx {
    int32_t index;
    int32_t stop;
    uint32_t ncmd;
    HANDLE iocp;
    ev_ctx *ev;
    overlap_cmd_ctx *cmd;
    struct hashmap *element;
    pthread_t thevent;
    skpool_ctx pool;
}watcher_ctx;
typedef struct acceptex_ctx {
    int32_t index;
    int32_t stop;
    ev_ctx *ev;
    HANDLE iocp;
    pthread_t thacp;
}acceptex_ctx;
typedef struct exfuncs_ctx {
    BOOL(WINAPI *acceptex)(SOCKET, SOCKET, PVOID, DWORD, DWORD, DWORD, LPDWORD, LPOVERLAPPED);
    BOOL(WINAPI *connectex)(SOCKET, const struct sockaddr *, int, PVOID, DWORD, LPDWORD, LPOVERLAPPED);
}exfuncs_ctx;
extern exfuncs_ctx _exfuncs;

void _send_cmd(watcher_ctx *watcher, uint32_t index, cmd_ctx *cmd);
void _cmd_add(watcher_ctx *watcher, sock_ctx *skctx, uint64_t hs);
void _cmd_add_acpfd(watcher_ctx *watcher, SOCKET fd, struct listener_ctx *lsn, uint64_t hs);
void _cmd_remove(watcher_ctx *watcher, SOCKET fd, uint64_t hs);

void _on_cmd_stop(watcher_ctx *watcher, cmd_ctx *cmd);
void _on_cmd_add(watcher_ctx *watcher, cmd_ctx *cmd);
void _on_cmd_addacp(watcher_ctx *watcher, cmd_ctx *cmd);
void _on_cmd_remove(watcher_ctx *watcher, cmd_ctx *cmd); 
void _on_cmd_send(watcher_ctx *watcher, cmd_ctx *cmd);
void _on_cmd_disconn(watcher_ctx *watcher, cmd_ctx *cmd);
void _on_cmd_setud_typstat(watcher_ctx *watcher, cmd_ctx *cmd);
void _on_cmd_setud_data(watcher_ctx *watcher, cmd_ctx *cmd);

void _add_fd(watcher_ctx *watcher, sock_ctx *skctx);
void _remove_fd(watcher_ctx *watcher, SOCKET fd);
int32_t _join_iocp(watcher_ctx *watcher, SOCKET fd);
void _add_acpfd_inloop(watcher_ctx *watcher, SOCKET fd, struct listener_ctx *lsn);
int32_t _post_recv(sock_ctx *skctx, DWORD  *bytes, DWORD  *flag, IOV_TYPE *wsabuf, DWORD niov);
int32_t _add_bufs_trypost(sock_ctx *skctx, off_buf_ctx *buf, uint8_t synflag);
int32_t _add_bufs_trysendto(sock_ctx *skctx, off_buf_ctx *buf, uint8_t synflag);
void _sk_shutdown(sock_ctx *skctx);
void _disconnect(sock_ctx *skctx);
void _setud_typstat(sock_ctx *skctx, char *typsta);
void _setud_data(sock_ctx *skctx, void *data);
void _free_udp(sock_ctx *skctx);
void _freelsn(struct listener_ctx *lsn);
ud_cxt *_get_ud(sock_ctx *skctx);
int32_t _check_skid(sock_ctx *skctx, const uint64_t skid);

#endif//EV_IOCP
#endif//IOCP_H_
