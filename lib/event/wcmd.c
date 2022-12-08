#include "event/iocp.h"
#include "loger.h"
#include "hashmap.h"

#ifdef EV_IOCP

#define DELAY_TIMEOUT     15
#define _SEND_CMD(ev, cmd)\
do {\
    uint64_t hs = FD_HASH(cmd.fd);\
    watcher_ctx *watcher = GET_PTR((ev)->watcher, (ev)->nthreads, hs);\
    _send_cmd(watcher, GET_POS(hs, watcher->ncmd), &cmd);\
} while (0)
static inline map_element *_map_get(watcher_ctx *watcher, SOCKET fd)
{
    map_element key;
    key.fd = fd;
    return hashmap_get(watcher->element, &key);
}
void _send_cmd(watcher_ctx *watcher, uint32_t index, cmd_ctx *cmd)
{
    overlap_cmd_ctx *ol = &watcher->cmd[index];
    mutex_lock(&ol->lck);
    qu_cmd_push(&ol->qu, cmd);
    mutex_unlock(&ol->lck);
    static char trigger[1] = { 's' };
    ASSERTAB(1 == send(ol->fd, trigger, sizeof(trigger), 0), ERRORSTR(ERRNO));
}
void _on_cmd_stop(watcher_ctx *watcher, cmd_ctx *cmd)
{
    watcher->stop = 1;
}
void _cmd_add(watcher_ctx *watcher, sock_ctx *skctx, uint64_t hs)
{
    cmd_ctx cmd;
    cmd.cmd = CMD_ADD;
    cmd.data = skctx;
    _send_cmd(watcher, GET_POS(hs, watcher->ncmd), &cmd);
}
void _on_cmd_add(watcher_ctx *watcher, cmd_ctx *cmd)
{
    _add_fd(watcher, cmd->data);
}
void _cmd_add_acpfd(watcher_ctx *watcher, SOCKET fd, struct listener_ctx *lsn, uint64_t hs)
{
    cmd_ctx cmd;
    cmd.cmd = CMD_ADDACP;
    cmd.fd = fd;
    cmd.data = lsn;
    _send_cmd(watcher, GET_POS(hs, watcher->ncmd), &cmd);
}
void _on_cmd_addacp(watcher_ctx *watcher, cmd_ctx *cmd)
{
    _add_acpfd_inloop(watcher, cmd->fd, cmd->data);
}
void _cmd_remove(watcher_ctx *watcher, SOCKET fd, uint64_t hs)
{
    cmd_ctx cmd;
    cmd.cmd = CMD_REMOVE;
    cmd.fd = fd;
    _send_cmd(watcher, GET_POS(hs, watcher->ncmd), &cmd);
}
void _remove(watcher_ctx *watcher, sock_ctx *skctx)
{
    _remove_fd(watcher, skctx->fd);
    if (0 == _check_canfree(skctx))
    {
        if (SOCK_STREAM == skctx->type)
        {
            pool_push(&watcher->pool, skctx);
        }
        else
        {
            _free_udp(skctx);
        }
    }
    else
    {
        delay_ctx delay;
        delay.timeout = watcher->ntime + DELAY_TIMEOUT;
        delay.sock = skctx;
        arr_delay_push_back(&watcher->delay, &delay);
    }
}
void _on_cmd_remove(watcher_ctx *watcher, cmd_ctx *cmd)
{
    map_element *el = _map_get(watcher, cmd->fd);
    if (NULL == el)
    {
        return;
    }
    _remove(watcher, el->sock);
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
    _SEND_CMD(ctx, cmd);
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
    _SEND_CMD(ctx, cmd);
}
void _on_cmd_send(watcher_ctx *watcher, cmd_ctx *cmd)
{
    map_element *el = _map_get(watcher, cmd->fd);
    if (NULL == el)
    {
        FREE(cmd->data);
        return;
    }
    bufs_ctx buf;
    buf.data = cmd->data;
    buf.len = cmd->len;
    buf.offset = 0;
    if (SOCK_STREAM == el->sock->type)
    {
        _add_bufs_trypost(el->sock, &buf);
    }
    else
    {
        _add_bufs_trysendto(el->sock, &buf);
    }
}
void ev_close(ev_ctx *ctx, SOCKET fd)
{
    ASSERTAB(INVALID_SOCK != fd, ERRSTR_INVPARAM);
    cmd_ctx cmd;
    cmd.cmd = CMD_DISCONN;
    cmd.fd = fd;
    _SEND_CMD(ctx, cmd);
}
void _on_cmd_disconn(watcher_ctx *watcher, cmd_ctx *cmd)
{
    map_element *el = _map_get(watcher, cmd->fd);
    if (NULL == el)
    {
        CLOSE_SOCK(cmd->fd);
        return;
    }
    if (SOCK_STREAM == el->sock->type)
    {
        _sk_shutdown(el->sock);
        return;
    }
    CLOSE_SOCK(cmd->fd);
}

#endif// EV_IOCP
