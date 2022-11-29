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
sock_ctx *_map_getskctx(struct hashmap *map, SOCKET fd)
{
    map_element *el = _map_get(map, fd);
    if (NULL == el)
    {
        return NULL;
    }
    return el->sock;
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
    if (NULL == el)
    {
        CLOSE_SOCK(cmd->fd);
        return;
    }
    if (SOCK_STREAM == el->sock->type)
    {
        _sk_shutdown(el->sock);
    }
    else
    {
        _udp_close(watcher, el->sock);
    }
}
void _cmd_listen(watcher_ctx *watcher, SOCKET fd, sock_ctx *skctx)
{
    cmd_ctx cmd;
    cmd.cmd = CMD_LSN;
    cmd.fd = fd;
    cmd.data = skctx;
    _cmd_send(watcher, GET_POS(FD_HASH(cmd.fd), watcher->npipes), &cmd);
}
void _on_cmd_lsn(watcher_ctx *watcher, cmd_ctx *cmd, int32_t *stop)
{
    _add_lsn_inloop(watcher, cmd->fd, cmd->data);
}
void _cmd_connect(ev_ctx *ctx, SOCKET fd, sock_ctx *skctx)
{
    cmd_ctx cmd;
    cmd.cmd = CMD_CONN;
    cmd.fd = fd;
    cmd.data = skctx;
    CMD_SEND(ctx, cmd);
}
void _on_cmd_conn(watcher_ctx *watcher, cmd_ctx *cmd, int32_t *stop)
{
    _add_conn_inloop(watcher, cmd->fd, cmd->data);
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
void ev_sendto(ev_ctx *ctx, SOCKET fd, const char *host, const uint16_t port, void *data, size_t len)
{
    ASSERTAB(INVALID_SOCK != fd, ERRSTR_INVPARAM);
    cmd_ctx cmd;
    cmd.cmd = CMD_SEND;
    cmd.fd = fd;
    cmd.len = len;
    MALLOC(cmd.data, sizeof(netaddr_ctx) + len);
    netaddr_ctx *addr = (netaddr_ctx *)cmd.data;
    if (ERR_OK != netaddr_sethost(addr, host, port))
    {
        FREE(cmd.data);
        LOG_WARN("%s", ERRORSTR(ERRNO));
        return;
    }
    memcpy((char *)cmd.data + sizeof(netaddr_ctx), data, len);
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
    _add_write_inloop(watcher, el->sock, &buf);
}
void _cmd_add_acpfd(watcher_ctx *watcher, uint64_t hs, SOCKET fd, struct listener_ctx *lsn)
{
    cmd_ctx cmd;
    cmd.cmd = CMD_ADDACP;
    cmd.fd = fd;
    cmd.data = lsn;
    _cmd_send(watcher, GET_POS(hs, watcher->npipes), &cmd);
}
void _on_cmd_addacp(watcher_ctx *watcher, cmd_ctx *cmd, int32_t *stop)
{
    _add_acpfd_inloop(watcher, cmd->fd, cmd->data);
}
void _cmd_add_udp(ev_ctx *ctx, SOCKET fd, sock_ctx *skctx)
{
    cmd_ctx cmd;
    cmd.cmd = CMD_ADDUDP;
    cmd.fd = fd;
    cmd.data = skctx;
    CMD_SEND(ctx, cmd);
}
void _on_cmd_add_udp(watcher_ctx *watcher, cmd_ctx *cmd, int32_t *stop)
{
    _add_udp_inloop(watcher, cmd->fd, cmd->data);
}

#endif
