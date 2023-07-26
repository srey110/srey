#include "event/iocp.h"
#include "ds/hashmap.h"

#ifdef EV_IOCP

#define _SEND_CMD(ev, cmd)\
do {\
    watcher_ctx *watcher = GET_PTR((ev)->watcher, (ev)->nthreads, cmd.fd);\
    _send_cmd(watcher, GET_POS(cmd.fd, watcher->ncmd), &cmd);\
} while (0)

static inline sock_ctx *_map_get(watcher_ctx *watcher, SOCKET fd) {
    sock_ctx key;
    key.fd = fd;
    sock_ctx *pkey = &key;
    void **tmp = (void **)hashmap_get(watcher->element, &pkey);
    return NULL == tmp ? NULL : *tmp;
}
void _send_cmd(watcher_ctx *watcher, uint32_t index, cmd_ctx *cmd) {
    overlap_cmd_ctx *olcmd = &watcher->cmd[index];
    spin_lock(&olcmd->spin);
    qu_cmd_push(&olcmd->qu, cmd);
    spin_unlock(&olcmd->spin);
    static char trigger[1] = { 's' };
    ASSERTAB(1 == send(olcmd->fd, trigger, sizeof(trigger), 0), ERRORSTR(ERRNO));
}
void _on_cmd_stop(watcher_ctx *watcher, cmd_ctx *cmd) {
    watcher->stop = 1;
}
void _cmd_add(watcher_ctx *watcher, sock_ctx *skctx, SOCKET fd) {
    cmd_ctx cmd;
    cmd.cmd = CMD_ADD;
    cmd.arg = (uint64_t)skctx;
    _send_cmd(watcher, GET_POS(fd, watcher->ncmd), &cmd);
}
void _on_cmd_add(watcher_ctx *watcher, cmd_ctx *cmd) {
    _add_fd(watcher, (sock_ctx *)cmd->arg);
}
void _cmd_add_acpfd(watcher_ctx *watcher, SOCKET fd, struct listener_ctx *lsn) {
    cmd_ctx cmd;
    cmd.cmd = CMD_ADDACP;
    cmd.fd = fd;
    cmd.arg = (uint64_t)lsn;
    _send_cmd(watcher, GET_POS(fd, watcher->ncmd), &cmd);
}
void _on_cmd_addacp(watcher_ctx *watcher, cmd_ctx *cmd) {
    _add_acpfd_inloop(watcher, cmd->fd, (struct listener_ctx *)cmd->arg);
}
void _cmd_remove(watcher_ctx *watcher, SOCKET fd, uint64_t skid) {
    cmd_ctx cmd;
    cmd.cmd = CMD_REMOVE;
    cmd.fd = fd;
    cmd.skid = skid;
    _send_cmd(watcher, GET_POS(fd, watcher->ncmd), &cmd);
}
void _on_cmd_remove(watcher_ctx *watcher, cmd_ctx *cmd) {
    sock_ctx *skctx = _map_get(watcher, cmd->fd);
    if (NULL == skctx
        || ERR_OK != _check_skid(skctx, cmd->skid)) {
        return;
    }
    _remove_fd(watcher, cmd->fd);
    if (SOCK_STREAM == skctx->type) {
        pool_push(&watcher->pool, skctx);
    } else {
        _free_udp(skctx);
    }
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
    if (SOCK_STREAM == skctx->type) {
        _add_bufs_trypost(skctx, &buf);
    } else {
        _add_bufs_trysendto(skctx, &buf);
    }
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
    _disconnect(skctx);
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
