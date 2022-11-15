#include "event/uev.h"
#include "hashmap.h"
#include "loger.h"

#ifndef EV_IOCP

#define CMD_SEND(ev, cmd)\
do {\
    uint64_t hs = FD_HASH(cmd.fd);\
    watcher_ctx *watcher = (1 == (ev)->nthreads) ? (ev)->watcher : (&(ev)->watcher[hs % (ev)->nthreads]);\
    _cmd_send(watcher, hs % watcher->npipes, &cmd);\
} while (0)

typedef struct cmd_update_ctx
{
    freeud_cb fud_cb;
    ud_cxt ud;
}cmd_update_ctx;

static inline map_element *_map_get(struct hashmap *map, SOCKET fd)
{
    map_element key;
    key.fd = fd;
    return hashmap_get(map, &key);
}
void _on_cmd_stop(watcher_ctx *watcher, cmd_ctx *cmd, int32_t *stop)
{
    *stop = 1;
}
void ev_update(ev_ctx *ctx, SOCKET fd, freeud_cb fud_cb, ud_cxt *ud)
{
    ASSERTAB(INVALID_SOCK != fd, ERRSTR_INVPARAM);
    cmd_update_ctx *update;
    MALLOC(update, sizeof(cmd_update_ctx));
    update->fud_cb = fud_cb;
    COPY_UD(update->ud, ud);
    cmd_ctx cmd;
    cmd.cmd = CMD_UPDATE;
    cmd.fd = fd;
    cmd.data = update;
    CMD_SEND(ctx, cmd);
}
void _on_cmd_update(watcher_ctx *watcher, cmd_ctx *cmd, int32_t *stop)
{
    cmd_update_ctx *update = (cmd_update_ctx *)cmd->data;
    map_element *el = _map_get(watcher->element, cmd->fd);
    if (NULL == el)
    {
        if (NULL != update->fud_cb)
        {
            update->fud_cb(&update->ud);
        }
    }
    else
    {
        _update_ud(el->sock, &update->ud);
    }
    FREE(update);
}
void ev_close(ev_ctx *ctx, SOCKET fd)
{
    ASSERTAB(INVALID_SOCK != fd, ERRSTR_INVPARAM);
    cmd_ctx cmd;
    cmd.cmd = CMD_DISCONN;
    cmd.fd = fd;
    CMD_SEND(ctx, cmd);
}
void _on_cmd_disconn(watcher_ctx *watcher, cmd_ctx *cmd, int32_t *stop)
{
    map_element *el = _map_get(watcher->element, cmd->fd);
    shutdown(cmd->fd, SHUT_RD);
    if (NULL == el)
    {
        CLOSE_SOCK(cmd->fd);
    }
}
void _cmd_listen(watcher_ctx *watcher, SOCKET fd, sock_ctx *skctx)
{
    ASSERTAB(INVALID_SOCK != fd, ERRSTR_INVPARAM);
    cmd_ctx cmd;
    cmd.cmd = CMD_LSN;
    cmd.fd = fd;
    cmd.data = skctx;
    _cmd_send(watcher, (FD_HASH(cmd.fd)) % watcher->npipes, &cmd);
}
void _on_cmd_lsn(watcher_ctx *watcher, cmd_ctx *cmd, int32_t *stop)
{
    sock_ctx *skctx = (sock_ctx *)cmd->data;
    if (ERR_OK != _add_event(watcher, cmd->fd, &skctx->events, EVENT_READ, skctx))
    {
        LOG_WARN("%s", "add listen socket in loop error.");
    }
}
void _cmd_connect(ev_ctx *ctx, SOCKET fd, sock_ctx *skctx)
{
    ASSERTAB(INVALID_SOCK != fd, ERRSTR_INVPARAM);
    cmd_ctx cmd;
    cmd.cmd = CMD_CONN;
    cmd.fd = fd;
    cmd.data = skctx;
    CMD_SEND(ctx, cmd);
}
void _on_cmd_conn(watcher_ctx *watcher, cmd_ctx *cmd, int32_t *stop)
{
    sock_ctx *skctx = (sock_ctx *)cmd->data;
    if (ERR_OK != _add_event(watcher, cmd->fd, &skctx->events, EVENT_WRITE, skctx))
    {
        ud_cxt ud;
        connect_cb conn_cb = _get_conn_cb(skctx, &ud);
        conn_cb(watcher->ev, INVALID_SOCK, &ud);
        CLOSE_SOCK(cmd->fd);
        FREE(skctx);
    }
}
void ev_send(ev_ctx *ctx, SOCKET fd, void *data, size_t len, int32_t copy)
{
    ASSERTAB(INVALID_SOCK != fd, ERRSTR_INVPARAM);
    cmd_ctx cmd;
    cmd.cmd = CMD_SEND;
    cmd.fd = fd;
    cmd.len = len;
    if (copy)
    {
        MALLOC(cmd.data, len);
        memcpy(cmd.data, data, len);
    }
    else
    {
        cmd.data = data;
    }
    CMD_SEND(ctx, cmd);
}
void _on_cmd_send(watcher_ctx *watcher, cmd_ctx *cmd, int32_t *stop)
{
    map_element *el = _map_get(watcher->element, cmd->fd);
    if (NULL == el)
    {
        FREE(cmd->data);
        return;
    }
    bufs_ctx buf;
    buf.data = cmd->data;
    buf.len = cmd->len;
    buf.offset = 0;
    qu_bufs_push(_get_send_buf(el->sock), &buf);
    if (!(el->sock->events & EVENT_WRITE))
    {
        _add_event(watcher, cmd->fd, &el->sock->events, EVENT_WRITE, el->sock);
    }
}
void _cmd_add(ev_ctx *ctx, SOCKET fd, sock_ctx *skctx)
{
    ASSERTAB(INVALID_SOCK != fd, ERRSTR_INVPARAM);
    cmd_ctx cmd;
    cmd.cmd = CMD_ADD;
    cmd.fd = fd;
    cmd.data = skctx;
    CMD_SEND(ctx, cmd);
}
void _on_cmd_add(watcher_ctx *watcher, cmd_ctx *cmd, int32_t *stop)
{
    sock_ctx *skctx = (sock_ctx *)cmd->data;
    if (ERR_OK != _add_event(watcher, cmd->fd, &skctx->events, EVENT_READ, skctx))
    {
        _on_close(watcher, skctx, 0);
        return;
    }
    map_element el;
    el.fd = skctx->fd;
    el.sock = skctx;
    ASSERTAB(NULL == hashmap_set(watcher->element, &el), "socket repeat.");
}

#endif
