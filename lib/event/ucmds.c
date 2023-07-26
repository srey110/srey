#include "event/uev.h"
#include "ds/hashmap.h"

#ifndef EV_IOCP

#define _SEND_CMD(ev, cmd)\
do {\
    watcher_ctx *watcher = GET_PTR((ev)->watcher, (ev)->nthreads, cmd.fd);\
    _send_cmd(watcher, GET_POS(cmd.fd, watcher->npipes), &cmd);\
} while (0)

sock_ctx *_map_get(watcher_ctx *watcher, SOCKET fd) {
    sock_ctx key;
    key.fd = fd;
    sock_ctx *pkey = &key;
    void **tmp = (void **)hashmap_get(watcher->element, &pkey);
    return NULL == tmp ? NULL : *tmp;
}
void _on_cmd_stop(watcher_ctx *watcher, cmd_ctx *cmd) {
    watcher->stop = 1;
}
void _cmd_listen(watcher_ctx *watcher, SOCKET fd, sock_ctx *skctx) {
    cmd_ctx cmd;
    cmd.cmd = CMD_LSN;
    cmd.fd = fd;
    cmd.arg = (uint64_t)skctx;
    _send_cmd(watcher, GET_POS(fd, watcher->npipes), &cmd);
}
void _on_cmd_lsn(watcher_ctx *watcher, cmd_ctx *cmd) {
    _add_lsn_inloop(watcher, cmd->fd, (sock_ctx *)cmd->arg);
}
void _cmd_unlisten(watcher_ctx *watcher, SOCKET fd, struct listener_ctx *lsn) {
    cmd_ctx cmd;
    cmd.cmd = CMD_UNLSN;
    cmd.fd = fd;
    cmd.arg = (uint64_t)lsn;
    _send_cmd(watcher, GET_POS(fd, watcher->npipes), &cmd);
}
void _on_cmd_unlsn(watcher_ctx *watcher, cmd_ctx *cmd) {
    _remove_lsn(watcher, cmd->fd, (struct listener_ctx *)cmd->arg);
}
void _cmd_connect(ev_ctx *ctx, SOCKET fd, sock_ctx *skctx) {
    cmd_ctx cmd;
    cmd.cmd = CMD_CONN;
    cmd.fd = fd;
    cmd.arg = (uint64_t)skctx;
    _SEND_CMD(ctx, cmd);
}
void _on_cmd_conn(watcher_ctx *watcher, cmd_ctx *cmd) {
    _add_conn_inloop(watcher, cmd->fd, (sock_ctx *)cmd->arg);
}
void ev_send(ev_ctx *ctx, SOCKET fd, uint64_t skid, void *data, size_t len, int32_t copy) {
    cmd_ctx cmd;
    cmd.cmd = CMD_SEND;
    cmd.fd = fd;
    cmd.skid = skid;
    cmd.len = len;
    if (copy) {
        char *buf;
        MALLOC(buf, len);
        memcpy(buf, data, len);
        cmd.arg = (uint64_t)buf;
    } else {
        cmd.arg = (uint64_t)data;
    }
    _SEND_CMD(ctx, cmd);
}
int32_t ev_sendto(ev_ctx *ctx, SOCKET fd, uint64_t skid,
    const char *ip, const uint16_t port, void *data, size_t len) {
    cmd_ctx cmd;
    cmd.cmd = CMD_SEND;
    cmd.fd = fd;
    cmd.skid = skid;
    cmd.len = len;
    char *buf;
    MALLOC(buf, sizeof(netaddr_ctx) + len);
    netaddr_ctx *addr = (netaddr_ctx *)buf;
    if (ERR_OK != netaddr_set(addr, ip, port)) {
        FREE(buf);
        LOG_WARN("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    memcpy(buf + sizeof(netaddr_ctx), data, len);
    cmd.arg = (uint64_t)buf;
    _SEND_CMD(ctx, cmd);
    return ERR_OK;
}
void _on_cmd_send(watcher_ctx *watcher, cmd_ctx *cmd) {
    void *data = (void *)cmd->arg;
    sock_ctx *skctx = _map_get(watcher, cmd->fd);
    if (NULL == skctx
        || ERR_OK != _check_skid(skctx, cmd->skid)) {
        FREE(data);
        return;
    }
    off_buf_ctx buf;
    buf.data = data;
    buf.len = cmd->len;
    buf.offset = 0;
    _add_write_inloop(watcher, skctx, &buf);
}
void ev_close(ev_ctx *ctx, SOCKET fd, uint64_t skid) {
    cmd_ctx cmd;
    cmd.cmd = CMD_DISCONN;
    cmd.fd = fd;
    cmd.skid = skid;
    _SEND_CMD(ctx, cmd);
}
void _on_cmd_disconn(watcher_ctx *watcher, cmd_ctx *cmd) {
    sock_ctx *skctx = _map_get(watcher, cmd->fd);
    if (NULL == skctx
        || ERR_OK != _check_skid(skctx, cmd->skid)) {
        return;
    }
    _disconnect(watcher, skctx);
}
void _cmd_add_acpfd(watcher_ctx *watcher, SOCKET fd, struct listener_ctx *lsn) {
    cmd_ctx cmd;
    cmd.cmd = CMD_ADDACP;
    cmd.fd = fd;
    cmd.arg = (uint64_t)lsn;
    _send_cmd(watcher, GET_POS(fd, watcher->npipes), &cmd);
}
void _on_cmd_addacp(watcher_ctx *watcher, cmd_ctx *cmd) {
    _add_acpfd_inloop(watcher, cmd->fd, (struct listener_ctx *)cmd->arg);
}
void _cmd_add_udp(ev_ctx *ctx, SOCKET fd, sock_ctx *skctx) {
    cmd_ctx cmd;
    cmd.cmd = CMD_ADDUDP;
    cmd.fd = fd;
    cmd.arg = (uint64_t)skctx;
    _SEND_CMD(ctx, cmd);
}
void _on_cmd_add_udp(watcher_ctx *watcher, cmd_ctx *cmd) {
    _add_udp_inloop(watcher, cmd->fd, (sock_ctx *)cmd->arg);
}
void _ev_set_ud(ev_ctx *ctx, SOCKET fd, uint64_t skid, int32_t type, uint64_t val) {
    cmd_ctx cmd;
    cmd.cmd = CMD_SETUD;
    cmd.fd = fd;
    cmd.skid = skid;
    cmd.len = (size_t)type;
    cmd.arg = val;
    _SEND_CMD(ctx, cmd);
}
void _on_cmd_setud(watcher_ctx *watcher, cmd_ctx *cmd) {
    sock_ctx *skctx = _map_get(watcher, cmd->fd);
    if (NULL == skctx
        || ERR_OK != _check_skid(skctx, cmd->skid)) {
        return;
    }
    _set_ud(_get_ud(skctx), (int32_t)cmd->len, cmd->arg);
}

#endif
