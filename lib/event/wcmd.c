#include "event/iocp.h"
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
    sock_ctx *skctx = _map_get(watcher, cmd->fd);
    if (NULL == skctx) {
        return;
    }
    _remove_fd(watcher, cmd->fd);
    if (SOCK_STREAM == skctx->type) {
        pool_push(&watcher->pool, skctx);
    } else {
        _free_udp(skctx);
    }
}
void ev_send(ev_ctx *ctx, SOCKET fd, uint64_t skid, void *data, size_t len, int32_t copy,
    uint8_t synflag, uint64_t sess) {
    ASSERTAB(INVALID_SOCK != fd, ERRSTR_INVPARAM);
    cmd_ctx cmd;
    cmd.cmd = CMD_SEND;
    cmd.fd = fd;
    cmd.skid = skid;
    cmd.flag = synflag;
    cmd.sess = sess;
    cmd.len = len;
    if (copy) {
        MALLOC(cmd.data, len);
        memcpy(cmd.data, data, len);
    } else {
        cmd.data = data;
    }
    _SEND_CMD(ctx, cmd);
}
int32_t ev_sendto(ev_ctx *ctx, SOCKET fd, uint64_t skid, const char *ip, const uint16_t port, void *data, size_t len,
    uint8_t synflag, uint64_t sess) {
    ASSERTAB(INVALID_SOCK != fd, ERRSTR_INVPARAM);
    cmd_ctx cmd;
    cmd.cmd = CMD_SEND;
    cmd.fd = fd;
    cmd.skid = skid;
    cmd.flag = synflag;
    cmd.sess = sess;
    cmd.len = len;
    MALLOC(cmd.data, sizeof(netaddr_ctx) + len);
    netaddr_ctx *addr = (netaddr_ctx *)cmd.data;
    if (ERR_OK != netaddr_sethost(addr, ip, port)) {
        FREE(cmd.data);
        LOG_WARN("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    memcpy((char *)cmd.data + sizeof(netaddr_ctx), data, len);
    _SEND_CMD(ctx, cmd);
    return ERR_OK;
}
void _on_cmd_send(watcher_ctx *watcher, cmd_ctx *cmd) {
    sock_ctx *skctx = _map_get(watcher, cmd->fd);
    if (NULL == skctx
        || ERR_OK != _check_skid(skctx, cmd->skid)) {
        if (SYN_ONCE == cmd->flag) {
            _send_result(_get_ud(skctx), cmd->sess, ERR_FAILED);
        }
        FREE(cmd->data);
        return;
    }
    off_buf_ctx buf;
    buf.data = cmd->data;
    buf.len = cmd->len;
    buf.offset = 0;
    int32_t err;
    if (SOCK_STREAM == skctx->type) {
        err = _add_bufs_trypost(skctx, &buf, cmd);
    } else {
        err = _add_bufs_trysendto(skctx, &buf, cmd);
    }
    if (SYN_ONCE == cmd->flag) {
        _send_result(_get_ud(skctx), cmd->sess, err);
    }
}
void ev_close(ev_ctx *ctx, SOCKET fd, uint64_t skid, uint8_t nomsg) {
    ASSERTAB(INVALID_SOCK != fd, ERRSTR_INVPARAM);
    cmd_ctx cmd;
    cmd.cmd = CMD_DISCONN;
    cmd.fd = fd;
    cmd.skid = skid;
    cmd.flag = nomsg;
    _SEND_CMD(ctx, cmd);
}
void _on_cmd_disconn(watcher_ctx *watcher, cmd_ctx *cmd) {
    sock_ctx *skctx = _map_get(watcher, cmd->fd);
    if (NULL == skctx
        || ERR_OK != _check_skid(skctx, cmd->skid)) {
        return;
    }
    _disconnect(skctx, cmd);
}
void ev_setud_typstat(ev_ctx *ctx, SOCKET fd, uint64_t skid, int8_t pktype, int8_t status) {
    ASSERTAB(INVALID_SOCK != fd, ERRSTR_INVPARAM);
    cmd_ctx cmd;
    cmd.cmd = CMD_SETUD_TYPSTAT;
    cmd.fd = fd;
    cmd.skid = skid;
    cmd.flag = 0;
    _set_ud_typstat_cmd((char *)&cmd.flag, pktype, status);
    _SEND_CMD(ctx, cmd);
}
void _on_cmd_setud_typstat(watcher_ctx *watcher, cmd_ctx *cmd) {
    sock_ctx *skctx = _map_get(watcher, cmd->fd);
    if (NULL == skctx
        || ERR_OK != _check_skid(skctx, cmd->skid)) {
        return;
    }
    _setud_typstat(skctx, (char *)&cmd->flag);
}
void ev_setud_data(ev_ctx *ctx, SOCKET fd, uint64_t skid, void *data) {
    ASSERTAB(INVALID_SOCK != fd, ERRSTR_INVPARAM);
    cmd_ctx cmd;
    cmd.cmd = CMD_SETUD_DATA;
    cmd.fd = fd;
    cmd.skid = skid;
    cmd.data = data;
    _SEND_CMD(ctx, cmd);
}
void _on_cmd_setud_data(watcher_ctx *watcher, cmd_ctx *cmd) {
    sock_ctx *skctx = _map_get(watcher, cmd->fd);
    if (NULL == skctx
        || ERR_OK != _check_skid(skctx, cmd->skid)) {
        return;
    }
    _setud_data(skctx, cmd->data);
}

#endif// EV_IOCP
