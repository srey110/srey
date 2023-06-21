#include "event/iocp.h"
#include "loger.h"
#include "hashmap.h"

#ifdef EV_IOCP

#define _SEND_CMD(ev, cmd)\
do {\
    uint64_t hs = FD_HASH(cmd.fd);\
    watcher_ctx *watcher = GET_PTR((ev)->watcher, (ev)->nthreads, hs);\
    _send_cmd(watcher, GET_POS(hs, watcher->ncmd), &cmd);\
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
    mutex_lock(&olcmd->lck);
    qu_cmd_push(&olcmd->qu, cmd);
    mutex_unlock(&olcmd->lck);
    static char trigger[1] = { 's' };
    ASSERTAB(1 == send(olcmd->fd, trigger, sizeof(trigger), 0), ERRORSTR(ERRNO));
}
void _on_cmd_stop(watcher_ctx *watcher, cmd_ctx *cmd) {
    watcher->stop = 1;
}
void _cmd_add(watcher_ctx *watcher, sock_ctx *skctx, uint64_t hs) {
    cmd_ctx cmd;
    cmd.cmd = CMD_ADD;
    cmd.data = skctx;
    _send_cmd(watcher, GET_POS(hs, watcher->ncmd), &cmd);
}
void _on_cmd_add(watcher_ctx *watcher, cmd_ctx *cmd) {
    _add_fd(watcher, cmd->data);
}
void _cmd_add_acpfd(watcher_ctx *watcher, SOCKET fd, struct listener_ctx *lsn, uint64_t hs) {
    cmd_ctx cmd;
    cmd.cmd = CMD_ADDACP;
    cmd.fd = fd;
    cmd.data = lsn;
    _send_cmd(watcher, GET_POS(hs, watcher->ncmd), &cmd);
}
void _on_cmd_addacp(watcher_ctx *watcher, cmd_ctx *cmd) {
    _add_acpfd_inloop(watcher, cmd->fd, cmd->data);
}
void _cmd_remove(watcher_ctx *watcher, SOCKET fd, uint64_t hs) {
    cmd_ctx cmd;
    cmd.cmd = CMD_REMOVE;
    cmd.fd = fd;
    _send_cmd(watcher, GET_POS(hs, watcher->ncmd), &cmd);
}
void _on_cmd_remove(watcher_ctx *watcher, cmd_ctx *cmd) {
    sock_ctx *el = _map_get(watcher, cmd->fd);
    if (NULL == el) {
        return;
    }
    _remove_fd(watcher, cmd->fd);
    if (SOCK_STREAM == el->type) {
        pool_push(&watcher->pool, el);
    } else {
        _free_udp(el);
    }
}
void ev_send(ev_ctx *ctx, SOCKET fd, void *data, size_t len, uint8_t synflag, int32_t copy) {
    ASSERTAB(INVALID_SOCK != fd, ERRSTR_INVPARAM);
    cmd_ctx cmd;
    cmd.cmd = CMD_SEND;
    cmd.flag = synflag;
    cmd.fd = fd;
    cmd.len = len;
    if (copy) {
        MALLOC(cmd.data, len);
        memcpy(cmd.data, data, len);
    } else {
        cmd.data = data;
    }
    _SEND_CMD(ctx, cmd);
}
void ev_sendto(ev_ctx *ctx, SOCKET fd, const char *host, const uint16_t port,
    void *data, size_t len, uint8_t synflag) {
    ASSERTAB(INVALID_SOCK != fd, ERRSTR_INVPARAM);
    cmd_ctx cmd;
    cmd.cmd = CMD_SEND;
    cmd.flag = synflag;
    cmd.fd = fd;
    cmd.len = len;
    MALLOC(cmd.data, sizeof(netaddr_ctx) + len);
    netaddr_ctx *addr = (netaddr_ctx *)cmd.data;
    if (ERR_OK != netaddr_sethost(addr, host, port)) {
        FREE(cmd.data);
        LOG_WARN("%s", ERRORSTR(ERRNO));
        return;
    }
    memcpy((char *)cmd.data + sizeof(netaddr_ctx), data, len);
    _SEND_CMD(ctx, cmd);
}
void _on_cmd_send(watcher_ctx *watcher, cmd_ctx *cmd) {
    sock_ctx *el = _map_get(watcher, cmd->fd);
    if (NULL == el) {
        FREE(cmd->data);
        return;
    }
    off_buf_ctx buf;
    buf.data = cmd->data;
    buf.len = cmd->len;
    buf.offset = 0;
    if (SOCK_STREAM == el->type) {
        _add_bufs_trypost(el, &buf, cmd->flag);
    } else {
        _add_bufs_trysendto(el, &buf, cmd->flag);
    }
}
void ev_close(ev_ctx *ctx, SOCKET fd) {
    ASSERTAB(INVALID_SOCK != fd, ERRSTR_INVPARAM);
    cmd_ctx cmd;
    cmd.cmd = CMD_DISCONN;
    cmd.fd = fd;
    _SEND_CMD(ctx, cmd);
}
void _on_cmd_disconn(watcher_ctx *watcher, cmd_ctx *cmd) {
    sock_ctx *el = _map_get(watcher, cmd->fd);
    if (NULL == el) {
        CLOSE_SOCK(cmd->fd);
        return;
    }
    _set_error(el);
    if (SOCK_STREAM == el->type) {
        _sk_shutdown(el);
    }
    CancelIoEx((HANDLE)cmd->fd, NULL);
}
void ev_setud_typstat(ev_ctx *ctx, SOCKET fd, int8_t pktype, int8_t status) {
    ASSERTAB(INVALID_SOCK != fd, ERRSTR_INVPARAM);
    cmd_ctx cmd;
    cmd.cmd = CMD_SETUD_TYPSTAT;
    cmd.fd = fd;
    cmd.len = 0;
    _set_ud_typstat_cmd((char *)&cmd.len, pktype, status);
    _SEND_CMD(ctx, cmd);
}
void _on_cmd_setud_typstat(watcher_ctx *watcher, cmd_ctx *cmd) {
    sock_ctx *el = _map_get(watcher, cmd->fd);
    if (NULL == el) {
        return;
    }
    _setud_typstat(el, (char *)&cmd->len);
}
void ev_setud_data(ev_ctx *ctx, SOCKET fd, void *data) {
    ASSERTAB(INVALID_SOCK != fd, ERRSTR_INVPARAM);
    cmd_ctx cmd;
    cmd.cmd = CMD_SETUD_DATA;
    cmd.fd = fd;
    cmd.data = data;
    _SEND_CMD(ctx, cmd);
}
void _on_cmd_setud_data(watcher_ctx *watcher, cmd_ctx *cmd) {
    sock_ctx *el = _map_get(watcher, cmd->fd);
    if (NULL == el) {
        return;
    }
    _setud_data(el, cmd->data);
}

#endif// EV_IOCP
