#include "containers/hashmap.h"
#include "utils/netutils.h"
#ifdef EV_IOCP
#include "event/iocp.h"
#else
#include "event/uev.h"
#endif

int32_t _evpub_checkid(struct sock_ctx *skctx, const uint64_t skid) {
#ifdef EV_IOCP
    return _iocp_check_skid(skctx, skid);
#else
    return _uev_check_skid(skctx, skid);
#endif
}
ud_cxt *_evpub_get_ud(struct sock_ctx *skctx) {
#ifdef EV_IOCP
    return _iocp_get_ud(skctx);
#else
    return _uev_get_ud(skctx);
#endif
}
int32_t _send_cmd(watcher_ctx *watcher, cmd_ctx *cmd) {
    int32_t erro;
#ifdef EV_IOCP
    overlap_cmd_ctx *olcmd = &watcher->cmd;
    fsqu_push(&olcmd->qu, cmd);
    static const char trigger[1] = { 's' };
    while (0 == ATOMIC_GET(&watcher->stop)
        && SOCKET_ERROR == send(olcmd->fd, trigger, sizeof(trigger), 0)) {
        erro = ERRNO;
        ASSERTAB(IS_EAGAIN(erro), ERRORSTR(erro));
        CPU_PAUSE();
    }
#else
#if CMD_PIPE_QU
    fsqu_push(&watcher->pipe.qu, cmd);
    static const char trigger[1] = { 's' };
    while (0 == ATOMIC_GET(&watcher->stop)
           && ERR_FAILED == write(watcher->pipe.pipes[1], trigger, sizeof(trigger))) {
        erro = ERRNO;
        ASSERTAB(ERR_RW_RETRIABLE(erro), ERRORSTR(erro));
        CPU_PAUSE();
    }
#else
    while (0 == ATOMIC_GET(&watcher->stop)) {
        if (ERR_FAILED != write(watcher->pipe.pipes[1], cmd, sizeof(cmd_ctx))) {
            return ERR_OK;
        }
        erro = ERRNO;
        ASSERTAB(ERR_RW_RETRIABLE(erro), ERRORSTR(erro));
        CPU_PAUSE();
    }
    return ERR_FAILED;
#endif
#endif//EV_IOCP
    return ERR_OK;
}
int32_t _cmd_add_acpfd(watcher_ctx *watcher, SOCKET fd, struct listener_ctx *lsn) {
    cmd_ctx cmd = { 0 };
    cmd.cmd = CMD_ADDACP;
    cmd.sk.fd = fd;
    cmd.args.lsn = lsn;
    return _send_cmd(watcher, &cmd);
}
void _on_cmd_addacp(watcher_ctx *watcher, cmd_ctx *cmd) {
#ifdef EV_IOCP
    _iocp_add_acpfd_inloop(watcher, cmd->sk.fd, cmd->args.lsn);
    // 配对 _on_accept_cb path 3 投递前 ref++ 占位：归零路径释放 lsn
    _iocp_try_freelsn(cmd->args.lsn);
#else
    _uev_add_acpfd_inloop(watcher, cmd->sk.fd, cmd->args.lsn);
    _uev_qtn_freelsn(watcher, cmd->args.lsn);
#endif
}
int32_t _cmd_connect(ev_ctx *ctx, struct sock_ctx *skctx, netaddr_ctx *addr) {
    cmd_ctx cmd = { 0 };
    cmd.cmd = CMD_CONN;
    cmd.sk.fd = skctx->fd;
    cmd.args.conn.skctx = skctx;
    if (NULL != addr) {
        cmd.args.conn.addr = *addr;
    }
    return _send_cmd(GET_PTR(ctx->watcher, ctx->nthreads, cmd.sk.fd), &cmd);
}
void _on_cmd_conn(watcher_ctx *watcher, cmd_ctx *cmd) {
#ifdef EV_IOCP
    _iocp_add_conn_inloop(watcher, cmd->args.conn.skctx, &cmd->args.conn.addr);
#else
    _uev_add_conn_inloop(watcher, cmd->args.conn.skctx);
#endif
}
int32_t _cmd_add(watcher_ctx *watcher, sock_ctx *skctx) {
    cmd_ctx cmd = { 0 };
    cmd.cmd = CMD_ADD;
    cmd.args.skctx = skctx;
    return _send_cmd(watcher, &cmd);
}
void _on_cmd_add(watcher_ctx *watcher, cmd_ctx *cmd) {
#ifdef EV_IOCP
    _iocp_add_fd_inloop(watcher, cmd->args.skctx);
#else
    _uev_add_fd_inloop(watcher, cmd->args.skctx);
#endif
}
#ifndef EV_IOCP
int32_t _cmd_listen(watcher_ctx *watcher, sock_ctx *skctx) {
    cmd_ctx cmd = { 0 };
    cmd.cmd = CMD_LSN;
    cmd.args.skctx = skctx;
    return _send_cmd(watcher, &cmd);
}
void _on_cmd_lsn(watcher_ctx *watcher, cmd_ctx *cmd) {
    _uev_add_lsn_inloop(watcher, cmd->args.skctx);
}
int32_t _cmd_unlisten(watcher_ctx *watcher, SOCKET fd, struct listener_ctx *lsn) {
    cmd_ctx cmd = { 0 };
    cmd.cmd = CMD_UNLSN;
    cmd.sk.fd = fd;
    cmd.args.lsn = lsn;
    return _send_cmd(watcher, &cmd);
}
void _on_cmd_unlsn(watcher_ctx *watcher, cmd_ctx *cmd) {
    _uev_remove_lsn(watcher, cmd->sk.fd, cmd->args.lsn);
}
int32_t _cmd_lsn_unref(watcher_ctx *watcher, struct listener_ctx *lsn) {
    cmd_ctx cmd = { 0 };
    cmd.cmd = CMD_LSN_UNREF;
    cmd.args.lsn = lsn;
    // cmd 不关联 fd, 任 watcher 接收即可
    return _send_cmd(watcher, &cmd);
}
void _on_cmd_lsn_unref(watcher_ctx *watcher, cmd_ctx *cmd) {
    _uev_qtn_freelsn(watcher, cmd->args.lsn);
}
#endif
void _on_cmd_stop(watcher_ctx *watcher, cmd_ctx *cmd) {
    (void)cmd;
    ATOMIC_SET(&watcher->stop, 1);
}
static inline int32_t _ev_props(ev_ctx *ctx, cmd_ctx *cmd) {
    if (ERR_OK != _send_cmd(GET_PTR(ctx->watcher, ctx->nthreads, cmd->sk.fd), cmd)) {
        UD_FREE(cmd->args.props.fcb, cmd->args.props.data);
        return ERR_FAILED;
    }
    return ERR_OK;
}
int32_t ev_props(ev_ctx *ctx, SOCKET fd, uint64_t skid,
                 props_cb ppcb, free_cb fcb, void *data, uint64_t number) {
    if (INVALID_SOCK == fd || NULL == ppcb) {
        UD_FREE(fcb, data);
        return ERR_FAILED;
    }
    cmd_ctx cmd = { 0 };
    cmd.cmd = CMD_PROPS;
    cmd.sk.fd = fd;
    cmd.sk.skid = skid;
    cmd.args.props.ppcb = ppcb;
    cmd.args.props.fcb = fcb;
    cmd.args.props.number = number;
    cmd.args.props.data = data;
    return _ev_props(ctx, &cmd);
}
void _on_cmd_props(struct watcher_ctx *watcher, cmd_ctx *cmd) {
    sock_ctx *skctx = _evpub_sockel_get(watcher, cmd->sk.fd);
    if (NULL == skctx
        || ERR_OK != _evpub_checkid(skctx, cmd->sk.skid)) {
        UD_FREE(cmd->args.props.fcb, cmd->args.props.data);
        return;
    }
    if (cmd->args.props.ppcb(watcher, skctx, cmd->args.props.data, cmd->args.props.number)) {
        UD_FREE(cmd->args.props.fcb, cmd->args.props.data);
    }
}
static int32_t _ev_close(struct watcher_ctx *watcher, struct sock_ctx *skctx,
    void *data, uint64_t number) {
    (void)data;
#ifdef EV_IOCP
    (void)watcher;
    _iocp_disconnect(skctx, (int32_t)number);
#else
    _uev_disconnect(watcher, skctx, (int32_t)number);
#endif
    return 0;
}
int32_t ev_close(ev_ctx *ctx, SOCKET fd, uint64_t skid, int32_t immed) {
    return ev_props(ctx, fd, skid, _ev_close, NULL, NULL, immed);
}
#if WITH_SSL
static int32_t _ev_ssl(struct watcher_ctx *watcher, struct sock_ctx *skctx,
    void *data, uint64_t number) {
#ifdef EV_IOCP
    _iocp_try_ssl_exchange(watcher, skctx, data, (int32_t)number);
#else
    _uev_try_ssl_exchange(watcher, skctx, data, (int32_t)number);
#endif
    return 0;
}
#endif
int32_t ev_ssl(ev_ctx *ctx, SOCKET fd, uint64_t skid, int32_t client, struct evssl_ctx *evssl) {
#if WITH_SSL
    if (NULL == evssl) {
        return ERR_FAILED;
    }
    return ev_props(ctx, fd, skid, _ev_ssl, NULL, evssl, client);
#else
    (void)ctx;
    (void)fd;
    (void)skid;
    (void)client;
    (void)evssl;
    return ERR_FAILED;
#endif
}
static inline void *_cmd_cpy_buf(void *data, size_t len, int32_t copy) {
    if (!copy) {
        return data;
    }
    void *buf;
    MALLOC(buf, len);
    memcpy(buf, data, len);
    return buf;
}
static inline void _ev_bufs_send(struct watcher_ctx *watcher, struct sock_ctx *skctx, off_buf_ctx *buf) {
#ifdef EV_IOCP
    (void)watcher;
    _iocp_add_bufs_trypost(skctx, buf);
#else
    _uev_add_bufs_send(watcher, skctx, buf);
#endif
}
static int32_t _ev_send(struct watcher_ctx *watcher, struct sock_ctx *skctx,
    void *data, uint64_t number) {
    if (SOCK_STREAM != skctx->type) {
        LOG_ERROR("ev_send called on non-TCP fd %d, drop.", (int32_t)skctx->fd);
        return 1;
    }
    off_buf_ctx buf = { 0 };
    buf.data = data;
    buf.lens = number;
    _ev_bufs_send(watcher, skctx, &buf);
    return 0;
}
int32_t ev_send(ev_ctx *ctx, SOCKET fd, uint64_t skid, void *data, size_t len, int32_t copy) {
    if (INVALID_SOCK == fd || EMPTYPTR(data, len)) {
        if (!copy) {
            FREE(data);
        }
        return ERR_FAILED;
    }
    return ev_props(ctx, fd, skid, _ev_send, _free,
        _cmd_cpy_buf(data, len, copy), len);
}
static int32_t _ev_send_multi(struct watcher_ctx *watcher, struct sock_ctx *skctx,
    void *data, uint64_t number) {
    if (SOCK_STREAM != skctx->type) {
        LOG_ERROR("ev_send_multi called on non-TCP fd %d, drop.", (int32_t)skctx->fd);
        return 1;
    }
    shared_data *pack = data;
    off_buf_ctx buf = { 0 };
    buf.data = pack->data;
    buf.lens = (size_t)number;
    buf.shared = pack;
    _ev_bufs_send(watcher, skctx, &buf);
    return 0;
}
int32_t ev_send_multi(ev_ctx *ctx, SOCKET fds[], uint64_t skids[], int32_t n,
                      void *data, size_t len, int32_t copy) {
    if (EMPTYPTR(data, len)) {
        if (!copy) {
            FREE(data);
        }
        return ERR_FAILED;
    }
    // 先扫一遍有效 fd 数,决定 pack->ref 初值
    int32_t valid = 0;
    int32_t i;
    for (i = 0; i < n; i++) {
        if (INVALID_SOCK != fds[i]) {
            valid++;
        }
    }
    if (0 == valid) {
        if (!copy) {
            FREE(data);
        }
        return ERR_FAILED;
    }
    shared_data *pack;
    MALLOC(pack, sizeof(shared_data));
    pack->data = _cmd_cpy_buf(data, len, copy);
    ATOMIC_SET(&pack->ref, valid);
    cmd_ctx cmd = { 0 };
    cmd.cmd = CMD_PROPS;
    cmd.args.props.ppcb = _ev_send_multi;
    cmd.args.props.fcb = _evpub_share_data_free;
    cmd.args.props.data = pack;
    cmd.args.props.number = len;
    // 全部 fd 都要投递一遍；_ev_props 失败时会在其失败分支对 pack 做一次 ref--,
    // 提前 return 会让后面还没投递的 fd 漏掉这次 ref--,导致 pack 永远归不了零、无法释放
    int32_t ok = 0;
    for (i = 0; i < n; i++) {
        if (INVALID_SOCK == fds[i]) {
            continue;
        }
        cmd.sk.fd = fds[i];
        cmd.sk.skid = skids[i];
        if (ERR_OK == _ev_props(ctx, &cmd)) {
            ok = 1;
        }
    }
    return ok ? ERR_OK : ERR_FAILED;
}
int32_t ev_sendto(ev_ctx *ctx, SOCKET fd, uint64_t skid, const char *ip, const uint16_t port,
    void *data, size_t len, int32_t copy) {
    netaddr_ctx addr;
    if (ERR_OK != netaddr_set(&addr, ip, port)) {
        if (!copy) {
            FREE(data);
        }
        LOG_WARN("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    return ev_sendto_addr(ctx, fd, skid, &addr, data, len, copy);
}
int32_t ev_sendto_addr(ev_ctx *ctx, SOCKET fd, uint64_t skid, netaddr_ctx *addr,
    void *data, size_t len, int32_t copy) {
    if (INVALID_SOCK == fd || NULL == addr || EMPTYPTR(data, len)) {
        if (!copy) {
            FREE(data);
        }
        return ERR_FAILED;
    }
    cmd_ctx cmd = { 0 };
    cmd.cmd = CMD_SENDTO;
    cmd.sk.fd = fd;
    cmd.sk.skid = skid;
    cmd.args.sendto.len = len;
    cmd.args.sendto.addr = *addr;
    cmd.args.sendto.data = _cmd_cpy_buf(data, len, copy);
    if (ERR_OK != _send_cmd(GET_PTR(ctx->watcher, ctx->nthreads, cmd.sk.fd), &cmd)) {
        FREE(cmd.args.sendto.data);
        return ERR_FAILED;
    }
    return ERR_OK;
}
void _on_cmd_sendto(watcher_ctx *watcher, cmd_ctx *cmd) {
    sock_ctx *skctx = _evpub_sockel_get(watcher, cmd->sk.fd);
    if (NULL == skctx
        || ERR_OK != _evpub_checkid(skctx, cmd->sk.skid)) {
        FREE(cmd->args.sendto.data);
        return;
    }
    // ev_sendto 仅适用于 UDP；TCP fd 用 ev_send（CMD_PROPS 路径）。误用时丢弃数据
    if (SOCK_DGRAM != skctx->type) {
        LOG_ERROR("ev_sendto called on non-UDP fd %d, drop.", (int32_t)cmd->sk.fd);
        FREE(cmd->args.sendto.data);
        return;
    }
    _evpub_add_bufs_sendto(watcher, skctx, &cmd->args.sendto, 0);
}
// 事件线程内执行 UDP 多播 setsockopt：先取 sock family,按 IPv4/IPv6 分支调对应 IP_*/IPV6_* 选项;
// Windows 路径下 IPv6 iface_str 忽略走默认接口(if_nametoindex 需 iphlpapi.lib,不引入依赖);
// 走 ev_props 通用命令,arg 由 ev_props 统一 UD_FREE,此处恒返回 1
static int32_t _udp_opt_cb(struct watcher_ctx *watcher, struct sock_ctx *skctx,
    void *data, uint64_t number) {
    (void)watcher;
    (void)number;
    udp_opt_arg *arg = data;
    if (SOCK_DGRAM != skctx->type) {
        LOG_ERROR("ev_udp_* called on non-UDP fd %d, drop.", (int32_t)skctx->fd);
        return 1;
    }
    int32_t family = sock_family(skctx->fd);
    if (ERR_FAILED == family) {
        LOG_ERROR("sock_family(fd=%d) failed: %s", (int32_t)skctx->fd, ERRORSTR(ERRNO));
        return 1;
    }
    int32_t rtn = ERR_FAILED;
    switch (arg->op) {
    case UDP_OPT_JOIN:
    case UDP_OPT_LEAVE:
        if (AF_INET == family) {
            struct ip_mreq mreq = { 0 };
            if (1 != inet_pton(AF_INET, arg->group_ip, &mreq.imr_multiaddr)) {
                LOG_ERROR("inet_pton(IPv4 %s) failed.", arg->group_ip);
                break;
            }
            if ('\0' != arg->iface_str[0]
                && 1 != inet_pton(AF_INET, arg->iface_str, &mreq.imr_interface)) {
                LOG_WARN("inet_pton(iface %s) failed,fallback INADDR_ANY", arg->iface_str);
                mreq.imr_interface.s_addr = htonl(INADDR_ANY);
            } else if ('\0' == arg->iface_str[0]) {
                mreq.imr_interface.s_addr = htonl(INADDR_ANY);
            }
            int32_t opt = (UDP_OPT_JOIN == arg->op) ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP;
            rtn = setsockopt(skctx->fd, IPPROTO_IP, opt, (const char *)&mreq, sizeof(mreq));
        } else if (AF_INET6 == family) {
            struct ipv6_mreq mreq = { 0 };
            if (1 != inet_pton(AF_INET6, arg->group_ip, &mreq.ipv6mr_multiaddr)) {
                LOG_ERROR("inet_pton(IPv6 %s) failed.", arg->group_ip);
                break;
            }
#ifdef EV_IOCP
            // Windows 不解析接口名,走默认 0;业务可用 IPV6_MULTICAST_IF 单独设
            mreq.ipv6mr_interface = 0;
#else
            if ('\0' != arg->iface_str[0]) {
                mreq.ipv6mr_interface = if_nametoindex(arg->iface_str);
                if (0 == mreq.ipv6mr_interface) {
                    LOG_WARN("if_nametoindex(%s) failed,fallback 0(default iface)", arg->iface_str);
                }
            }
#endif
            int32_t opt = (UDP_OPT_JOIN == arg->op) ? IPV6_JOIN_GROUP : IPV6_LEAVE_GROUP;
            rtn = setsockopt(skctx->fd, IPPROTO_IPV6, opt, (const char *)&mreq, sizeof(mreq));
        }
        break;
    case UDP_OPT_TTL:
        if (AF_INET == family) {
            uint8_t ttl = arg->ttl;
            rtn = setsockopt(skctx->fd, IPPROTO_IP, IP_MULTICAST_TTL, (const char *)&ttl, sizeof(ttl));
        } else if (AF_INET6 == family) {
            int32_t hops = (int32_t)arg->ttl;
            rtn = setsockopt(skctx->fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, (const char *)&hops, sizeof(hops));
        }
        break;
    case UDP_OPT_LOOP:
        if (AF_INET == family) {
            uint8_t loop = (uint8_t)(arg->loop ? 1 : 0);
            rtn = setsockopt(skctx->fd, IPPROTO_IP, IP_MULTICAST_LOOP, (const char *)&loop, sizeof(loop));
        } else if (AF_INET6 == family) {
            int32_t loop = arg->loop ? 1 : 0;
            rtn = setsockopt(skctx->fd, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, (const char *)&loop, sizeof(loop));
        }
        break;
    }
    if (0 != rtn) {
        LOG_ERROR("UDP opt %d failed on fd %d: %s", (int32_t)arg->op, (int32_t)skctx->fd, ERRORSTR(ERRNO));
    }
    return 1;
}
// UDP 多播 4 个公开 API 走同一 cmd 投递路径,差异只在 udp_opt_arg 字段填充
int32_t ev_udp_join(ev_ctx *ctx, SOCKET fd, uint64_t skid,
                    const char *group_ip, const char *iface_str) {
    if (INVALID_SOCK == fd || NULL == group_ip) {
        return ERR_FAILED;
    }
    udp_opt_arg *arg;
    CALLOC(arg, 1, sizeof(udp_opt_arg));
    arg->op = UDP_OPT_JOIN;
    safe_fill_str(arg->group_ip, sizeof(arg->group_ip), group_ip);
    safe_fill_str(arg->iface_str, sizeof(arg->iface_str), iface_str);
    return ev_props(ctx, fd, skid, _udp_opt_cb, _free, arg, 0);
}
int32_t ev_udp_leave(ev_ctx *ctx, SOCKET fd, uint64_t skid,
                     const char *group_ip, const char *iface_str) {
    if (INVALID_SOCK == fd || NULL == group_ip) {
        return ERR_FAILED;
    }
    udp_opt_arg *arg;
    CALLOC(arg, 1, sizeof(udp_opt_arg));
    arg->op = UDP_OPT_LEAVE;
    safe_fill_str(arg->group_ip, sizeof(arg->group_ip), group_ip);
    safe_fill_str(arg->iface_str, sizeof(arg->iface_str), iface_str);
    return ev_props(ctx, fd, skid, _udp_opt_cb, _free, arg, 0);
}
int32_t ev_udp_ttl(ev_ctx *ctx, SOCKET fd, uint64_t skid, uint8_t ttl) {
    if (INVALID_SOCK == fd) {
        return ERR_FAILED;
    }
    udp_opt_arg *arg;
    CALLOC(arg, 1, sizeof(udp_opt_arg));
    arg->op = UDP_OPT_TTL;
    arg->ttl = ttl;
    return ev_props(ctx, fd, skid, _udp_opt_cb, _free, arg, 0);
}
int32_t ev_udp_loop(ev_ctx *ctx, SOCKET fd, uint64_t skid, int32_t enable) {
    if (INVALID_SOCK == fd) {
        return ERR_FAILED;
    }
    udp_opt_arg *arg;
    CALLOC(arg, 1, sizeof(udp_opt_arg));
    arg->op = UDP_OPT_LOOP;
    arg->loop = enable ? 1 : 0;
    return ev_props(ctx, fd, skid, _udp_opt_cb, _free, arg, 0);
}
static int32_t _cmd_ud_pktype(struct watcher_ctx *watcher, struct sock_ctx *skctx,
    void *data, uint64_t number) {
    (void)watcher;
    (void)data;
    _evpub_get_ud(skctx)->pktype = (subtype_t)number;
    return 0;
}
int32_t ev_ud_pktype(ev_ctx *ctx, SOCKET fd, uint64_t skid, subtype_t pktype) {
    return ev_props(ctx, fd, skid, _cmd_ud_pktype, NULL, NULL, pktype);
}
static int32_t _cmd_ud_status(struct watcher_ctx *watcher, struct sock_ctx *skctx,
    void *data, uint64_t number) {
    (void)watcher;
    (void)data;
    _evpub_get_ud(skctx)->status = (uint8_t)number;
    return 0;
}
int32_t ev_ud_status(ev_ctx *ctx, SOCKET fd, uint64_t skid, uint8_t status) {
    return ev_props(ctx, fd, skid, _cmd_ud_status, NULL, NULL, status);
}
static int32_t _cmd_ud_sess(struct watcher_ctx *watcher, struct sock_ctx *skctx,
    void *data, uint64_t number) {
    (void)watcher;
    (void)data;
    _evpub_get_ud(skctx)->sess = number;
    return 0;
}
int32_t ev_ud_sess(ev_ctx *ctx, SOCKET fd, uint64_t skid, uint64_t sess) {
    return ev_props(ctx, fd, skid, _cmd_ud_sess, NULL, NULL, sess);
}
static int32_t _cmd_ud_handle(struct watcher_ctx *watcher, struct sock_ctx *skctx,
    void *data, uint64_t number) {
    (void)watcher;
    (void)data;
    _evpub_get_ud(skctx)->handle = (name_t)number;
    return 0;
}
int32_t ev_ud_handle(ev_ctx *ctx, SOCKET fd, uint64_t skid, name_t handle) {
    return ev_props(ctx, fd, skid, _cmd_ud_handle, NULL, NULL, handle);
}
static int32_t _cmd_ud_context(struct watcher_ctx *watcher, struct sock_ctx *skctx,
    void *data, uint64_t number) {
    (void)watcher;
    (void)number;
    _evpub_get_ud(skctx)->context = data;
    return 0;
}
int32_t ev_ud_context(ev_ctx *ctx, SOCKET fd, uint64_t skid, void *extra) {
    return ev_props(ctx, fd, skid, _cmd_ud_context, NULL, extra, 0);
}
