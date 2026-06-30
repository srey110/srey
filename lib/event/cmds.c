#include "containers/hashmap.h"
#include "utils/netutils.h"
#ifdef EV_IOCP
#include "event/iocp.h"
#define _CHECK_SKID _iocp_check_skid
#else
#include "event/uev.h"
#define _CHECK_SKID _uev_check_skid
#endif

// ud_cxt 字段设置类型枚举
typedef enum ud_type {
    UD_PKTYPE = 0x00, // 数据包类型
    UD_STATUS,        // 状态
    UD_HANDLE,        // 任务句柄
    UD_SESS,          // session
    UD_CONTEXT,       // 用户自定义数据指针
    UD_SECCONTEXT     // 子协议（如websocket）用户数据指针
}ud_type;

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
void ev_close(ev_ctx *ctx, SOCKET fd, uint64_t skid, int32_t immed) {
    if (INVALID_SOCK == fd) {
        return;
    }
    cmd_ctx cmd = { 0 };
    cmd.cmd = CMD_DISCONN;
    cmd.sk.fd = fd;
    cmd.sk.skid = skid;
    cmd.args.immed = immed;
    (void)_send_cmd(GET_PTR(ctx->watcher, ctx->nthreads, cmd.sk.fd), &cmd);
}
void _on_cmd_disconn(watcher_ctx *watcher, cmd_ctx *cmd) {
    sock_ctx *skctx = _evpub_sockel_get(watcher, cmd->sk.fd);
    if (NULL == skctx
        || ERR_OK != _CHECK_SKID(skctx, cmd->sk.skid)) {
        return;
    }
#ifdef EV_IOCP
    _iocp_disconnect(skctx, cmd->args.immed);
#else
    _uev_disconnect(watcher, skctx, cmd->args.immed);
#endif
}
int32_t ev_ssl(ev_ctx *ctx, SOCKET fd, uint64_t skid, int32_t client, struct evssl_ctx *evssl) {
    if (INVALID_SOCK == fd || NULL == evssl) {
        return ERR_FAILED;
    }
#if WITH_SSL
    cmd_ctx cmd = { 0 };
    cmd.cmd = CMD_SSL;
    cmd.sk.fd = fd;
    cmd.sk.skid = skid;
    cmd.args.ssl.client = client;
    cmd.args.ssl.evssl = evssl;
    return _send_cmd(GET_PTR(ctx->watcher, ctx->nthreads, cmd.sk.fd), &cmd);
#else
    (void)ctx;
    (void)skid;
    (void)client;
    (void)evssl;
    return ERR_FAILED;
#endif
}
void _on_cmd_ssl(watcher_ctx *watcher, cmd_ctx *cmd) {
    sock_ctx *skctx = _evpub_sockel_get(watcher, cmd->sk.fd);
    if (NULL == skctx
        || ERR_OK != _CHECK_SKID(skctx, cmd->sk.skid)) {
        return;
    }
#ifdef EV_IOCP
    _iocp_try_ssl_exchange(watcher, skctx, cmd->args.ssl.evssl, cmd->args.ssl.client);
#else
    _uev_try_ssl_exchange(watcher, skctx, cmd->args.ssl.evssl, cmd->args.ssl.client);
#endif
}
int32_t ev_send(ev_ctx *ctx, SOCKET fd, uint64_t skid, void *data, size_t len, int32_t copy) {
    if (INVALID_SOCK == fd || EMPTYPTR(data, len)) {
        if (!copy) {
            FREE(data);
        }
        return ERR_FAILED;
    }
    cmd_ctx cmd = { 0 };
    cmd.cmd = CMD_SEND;
    cmd.sk.fd = fd;
    cmd.sk.skid = skid;
    cmd.args.send.len = len;
    if (copy) {
        char *buf;
        MALLOC(buf, len);
        memcpy(buf, data, len);
        cmd.args.send.data = buf;
    } else {
        cmd.args.send.data = data;
    }
    if (ERR_OK != _send_cmd(GET_PTR(ctx->watcher, ctx->nthreads, cmd.sk.fd), &cmd)) {
        void *fbuf = cmd.args.send.data;
        FREE(fbuf);
        return ERR_FAILED;
    }
    return ERR_OK;
}
void _on_cmd_send(watcher_ctx *watcher, cmd_ctx *cmd) {
    void *data = cmd->args.send.data;
    sock_ctx *skctx = _evpub_sockel_get(watcher, cmd->sk.fd);
    if (NULL == skctx
        || ERR_OK != _CHECK_SKID(skctx, cmd->sk.skid)) {
        FREE(data);
        return;
    }
    // ev_send 仅适用于 TCP；UDP fd 用 ev_sendto（CMD_SENDTO 路径）。误用时丢弃数据避免
    // 把裸 payload 当 [netaddr_ctx + payload] 在 UDP 写路径中按错位读取地址发出。
    if (SOCK_STREAM != skctx->type) {
        LOG_ERROR("ev_send called on non-TCP fd %d, drop.", (int32_t)cmd->sk.fd);
        FREE(data);
        return;
    }
    off_buf_ctx buf = { 0 };
    buf.data = data;
    buf.lens = cmd->args.send.len;
#ifdef EV_IOCP
    _iocp_add_bufs_trypost(skctx, &buf);
#else
    _uev_add_bufs_send(watcher, skctx, &buf);
#endif
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
    if (copy) {
        MALLOC(pack->data, len);
        memcpy(pack->data, data, len);
    } else {
        pack->data = data;
    }
    ATOMIC_SET(&pack->ref, valid);
    // 给每个有效 fd 投一条 CMD_SEND_MULTI;事件线程取出后包装 off_buf{shared=pack} 入队
    cmd_ctx cmd = { 0 };
    cmd.cmd = CMD_SEND_MULTI;
    cmd.args.multi.len = len;
    cmd.args.multi.pack = pack;
    for (i = 0; i < n; i++) {
        if (INVALID_SOCK == fds[i]) {
            continue;
        }
        cmd.sk.fd = fds[i];
        cmd.sk.skid = skids[i];
        if (ERR_OK != _send_cmd(GET_PTR(ctx->watcher, ctx->nthreads, cmd.sk.fd), &cmd)) {
            if (1 == ATOMIC_ADD(&pack->ref, -1)) {
                FREE(pack->data);
                FREE(pack);
                return ERR_FAILED;
            }
        }
    }
    return ERR_OK;
}
void _on_cmd_send_multi(watcher_ctx *watcher, cmd_ctx *cmd) {
    shared_data *pack = cmd->args.multi.pack;
    sock_ctx *skctx = _evpub_sockel_get(watcher, cmd->sk.fd);
    // sock 失效或非 TCP：本 fd 这份引用归还,归 0 时释放。正常路径由 off_buf_ctx.shared 承载共享 ref 减；
    // 此处未构造 off_buf_ctx,直接 ATOMIC_ADD -1 即可
    if (NULL == skctx
        || ERR_OK != _CHECK_SKID(skctx, cmd->sk.skid)
        || SOCK_STREAM != skctx->type) {
        if (1 == ATOMIC_ADD(&pack->ref, -1)) {
            FREE(pack->data);
            FREE(pack);
        }
        return;
    }
    off_buf_ctx buf = { 0 };
    buf.data = pack->data;
    buf.lens = cmd->args.multi.len;
    buf.shared = pack;
#ifdef EV_IOCP
    _iocp_add_bufs_trypost(skctx, &buf);
#else
    _uev_add_bufs_send(watcher, skctx, &buf);
#endif
}
int32_t ev_sendto(ev_ctx *ctx, SOCKET fd, uint64_t skid, const char *ip, const uint16_t port,
    void *data, size_t len, int32_t copy) {
    if (INVALID_SOCK == fd || EMPTYPTR(data, len)) {
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
    if (ERR_OK != netaddr_set(&cmd.args.sendto.addr, ip, port)) {
        if (!copy) {
            FREE(data);
        }
        LOG_WARN("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    if (copy) {
        char *buf;
        MALLOC(buf, len);
        memcpy(buf, data, len);
        cmd.args.sendto.data = buf;
    } else {
        cmd.args.sendto.data = data;
    }
    if (ERR_OK != _send_cmd(GET_PTR(ctx->watcher, ctx->nthreads, cmd.sk.fd), &cmd)) {
        void *fbuf = cmd.args.sendto.data;
        FREE(fbuf);
        return ERR_FAILED;
    }
    return ERR_OK;
}
void _on_cmd_sendto(watcher_ctx *watcher, cmd_ctx *cmd) {
    sock_ctx *skctx = _evpub_sockel_get(watcher, cmd->sk.fd);
    if (NULL == skctx
        || ERR_OK != _CHECK_SKID(skctx, cmd->sk.skid)) {
        FREE(cmd->args.sendto.data);
        return;
    }
    // ev_sendto 仅适用于 UDP；TCP fd 用 ev_send（CMD_SEND 路径）。误用时丢弃数据
    if (SOCK_DGRAM != skctx->type) {
        LOG_ERROR("ev_sendto called on non-UDP fd %d, drop.", (int32_t)cmd->sk.fd);
        FREE(cmd->args.sendto.data);
        return;
    }
#ifdef EV_IOCP
    _iocp_add_bufs_trysendto(skctx, &cmd->args.sendto);
#else
    _uev_add_bufs_sendto(watcher, skctx, &cmd->args.sendto);
#endif
}
// UDP 多播 4 个公开 API 走同一 cmd 投递路径,差异只在 udp_opt_arg 字段填充
static int32_t _send_cmd_udp_opt(ev_ctx *ctx, SOCKET fd, uint64_t skid, udp_opt_arg *arg) {
    cmd_ctx cmd = { 0 };
    cmd.cmd = CMD_UDP_OPT;
    cmd.sk.fd = fd;
    cmd.sk.skid = skid;
    cmd.args.udpop = arg;
    if (ERR_OK != _send_cmd(GET_PTR(ctx->watcher, ctx->nthreads, cmd.sk.fd), &cmd)) {
        FREE(arg);
        return ERR_FAILED;
    }
    return ERR_OK;
}
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
    return _send_cmd_udp_opt(ctx, fd, skid, arg);
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
    return _send_cmd_udp_opt(ctx, fd, skid, arg);
}
int32_t ev_udp_ttl(ev_ctx *ctx, SOCKET fd, uint64_t skid, uint8_t ttl) {
    if (INVALID_SOCK == fd) {
        return ERR_FAILED;
    }
    udp_opt_arg *arg;
    CALLOC(arg, 1, sizeof(udp_opt_arg));
    arg->op = UDP_OPT_TTL;
    arg->ttl = ttl;
    return _send_cmd_udp_opt(ctx, fd, skid, arg);
}
int32_t ev_udp_loop(ev_ctx *ctx, SOCKET fd, uint64_t skid, int32_t enable) {
    if (INVALID_SOCK == fd) {
        return ERR_FAILED;
    }
    udp_opt_arg *arg;
    CALLOC(arg, 1, sizeof(udp_opt_arg));
    arg->op = UDP_OPT_LOOP;
    arg->loop = enable ? 1 : 0;
    return _send_cmd_udp_opt(ctx, fd, skid, arg);
}
// 事件线程内执行 UDP 多播 setsockopt：先取 sock family,按 IPv4/IPv6 分支调对应 IP_*/IPV6_* 选项;
// Windows 路径下 IPv6 iface_str 忽略走默认接口(if_nametoindex 需 iphlpapi.lib,不引入依赖)
void _on_cmd_udp_opt(watcher_ctx *watcher, cmd_ctx *cmd) {
    udp_opt_arg *arg = cmd->args.udpop;
    sock_ctx *skctx = _evpub_sockel_get(watcher, cmd->sk.fd);
    if (NULL == skctx || ERR_OK != _CHECK_SKID(skctx, cmd->sk.skid)) {
        FREE(arg);
        return;
    }
    if (SOCK_DGRAM != skctx->type) {
        LOG_ERROR("ev_udp_* called on non-UDP fd %d, drop.", (int32_t)cmd->sk.fd);
        FREE(arg);
        return;
    }
    int32_t family = sock_family(cmd->sk.fd);
    if (ERR_FAILED == family) {
        LOG_ERROR("sock_family(fd=%d) failed: %s", (int32_t)cmd->sk.fd, ERRORSTR(ERRNO));
        FREE(arg);
        return;
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
            rtn = setsockopt(cmd->sk.fd, IPPROTO_IP, opt, (const char *)&mreq, sizeof(mreq));
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
            rtn = setsockopt(cmd->sk.fd, IPPROTO_IPV6, opt, (const char *)&mreq, sizeof(mreq));
        }
        break;
    case UDP_OPT_TTL:
        if (AF_INET == family) {
            uint8_t ttl = arg->ttl;
            rtn = setsockopt(cmd->sk.fd, IPPROTO_IP, IP_MULTICAST_TTL, (const char *)&ttl, sizeof(ttl));
        } else if (AF_INET6 == family) {
            int32_t hops = (int32_t)arg->ttl;
            rtn = setsockopt(cmd->sk.fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, (const char *)&hops, sizeof(hops));
        }
        break;
    case UDP_OPT_LOOP:
        if (AF_INET == family) {
            uint8_t loop = (uint8_t)(arg->loop ? 1 : 0);
            rtn = setsockopt(cmd->sk.fd, IPPROTO_IP, IP_MULTICAST_LOOP, (const char *)&loop, sizeof(loop));
        } else if (AF_INET6 == family) {
            int32_t loop = arg->loop ? 1 : 0;
            rtn = setsockopt(cmd->sk.fd, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, (const char *)&loop, sizeof(loop));
        }
        break;
    }
    if (0 != rtn) {
        LOG_ERROR("UDP opt %d failed on fd %d: %s", (int32_t)arg->op, (int32_t)cmd->sk.fd, ERRORSTR(ERRNO));
    }
    FREE(arg);
}
static int32_t _cmd_set_ud(ev_ctx *ctx, SOCKET fd, uint64_t skid, int32_t type, uint64_t val) {
    if (INVALID_SOCK == fd) {
        return ERR_FAILED;
    }
    cmd_ctx cmd = { 0 };
    cmd.cmd = CMD_SETUD;
    cmd.sk.fd = fd;
    cmd.sk.skid = skid;
    cmd.args.setud.type = type;
    cmd.args.setud.val = val;
    return _send_cmd(GET_PTR(ctx->watcher, ctx->nthreads, cmd.sk.fd), &cmd);
}
int32_t ev_ud_pktype(ev_ctx *ctx, SOCKET fd, uint64_t skid, subtype_t pktype) {
    return _cmd_set_ud(ctx, fd, skid, UD_PKTYPE, pktype);
}
int32_t ev_ud_status(ev_ctx *ctx, SOCKET fd, uint64_t skid, uint8_t status) {
    return _cmd_set_ud(ctx, fd, skid, UD_STATUS, status);
}
int32_t ev_ud_sess(ev_ctx *ctx, SOCKET fd, uint64_t skid, uint64_t sess) {
    return _cmd_set_ud(ctx, fd, skid, UD_SESS, sess);
}
int32_t ev_ud_handle(ev_ctx *ctx, SOCKET fd, uint64_t skid, name_t handle) {
    return _cmd_set_ud(ctx, fd, skid, UD_HANDLE, handle);
}
int32_t ev_ud_context(ev_ctx *ctx, SOCKET fd, uint64_t skid, void *extra) {
    return _cmd_set_ud(ctx, fd, skid, UD_CONTEXT, (uint64_t)extra);
}
int32_t ev_ud_seccontext(ev_ctx *ctx, SOCKET fd, uint64_t skid, void *extra) {
    return _cmd_set_ud(ctx, fd, skid, UD_SECCONTEXT, (uint64_t)extra);
}
static void _set_ud(ud_cxt *ud, int32_t type, uint64_t val) {
    switch (type) {
    case UD_PKTYPE:
        ud->pktype = (subtype_t)val;
        break;
    case UD_STATUS:
        ud->status = (uint8_t)val;
        break;
    case UD_HANDLE:
        ud->handle = (name_t)val;
        break;
    case UD_SESS:
        ud->sess = val;
        break;
    case UD_CONTEXT:
        ud->context = (void *)val;
        break;
    case UD_SECCONTEXT:
        _evpub_set_secextra(ud, (void *)val);
        break;
    default:
        break;
    }
}
void _on_cmd_setud(watcher_ctx *watcher, cmd_ctx *cmd) {
    sock_ctx *skctx = _evpub_sockel_get(watcher, cmd->sk.fd);
    if (NULL == skctx
        || ERR_OK != _CHECK_SKID(skctx, cmd->sk.skid)) {
        return;
    }
#ifdef EV_IOCP
    _set_ud(_iocp_get_ud(skctx), cmd->args.setud.type, cmd->args.setud.val);
#else
    _set_ud(_uev_get_ud(skctx), cmd->args.setud.type, cmd->args.setud.val);
#endif
}
