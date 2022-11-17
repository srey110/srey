#include "event/uev.h"
#include "hashmap.h"
#include "loger.h"

#ifndef EV_IOCP

#define CMD_SEND(ev, cmd)\
do {\
    uint64_t hs = FD_HASH(cmd.fd);\
    watcher_ctx *watcher = GET_PTR((ev)->watcher, (ev)->nthreads, hs);\
    _cmd_send(watcher, GET_POS(hs, watcher->npipes), &cmd);\
} while (0)

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
    _cmd_send(watcher, GET_POS(FD_HASH(cmd.fd), watcher->npipes), &cmd);
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
    qu_bufs_push(_get_send_bufs(el->sock), &buf);
    if (!(el->sock->events & EVENT_WRITE))
    {
        _add_event(watcher, cmd->fd, &el->sock->events, EVENT_WRITE, el->sock);
    }
}
void _cmd_add(watcher_ctx *watcher, SOCKET fd, uint64_t hs, cbs_ctx *cbs, ud_cxt *ud)
{
    ASSERTAB(INVALID_SOCK != fd, ERRSTR_INVPARAM);
    cmd_ctx cmd;
    cmd.cmd = CMD_ADD;
    cmd.fd = fd;
    cmd.data = cbs;
    cmd.len = (size_t)ud;
    _cmd_send(watcher, GET_POS(hs, watcher->npipes), &cmd);
}
void _on_cmd_add(watcher_ctx *watcher, cmd_ctx *cmd, int32_t *stop)
{
    _add_inloop(watcher, cmd->fd, (cbs_ctx *)cmd->data, (ud_cxt *)cmd->len);
}
void _add_inloop(watcher_ctx *watcher, SOCKET fd, cbs_ctx *cbs, ud_cxt *ud)
{
    sock_ctx *skctx = pool_pop(&watcher->pool, fd, cbs, ud);
    if (ERR_OK != _add_event(watcher, fd, &skctx->events, EVENT_READ, skctx))
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
