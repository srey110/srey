#include "containers/hashmap.h"
#include "utils/netutils.h"
#ifdef EV_IOCP
#include "event/iocp.h"
#define _CHECK_SKID        _iocp_check_skid
#define _GET_UD            _iocp_get_ud
#define _TRY_SSL_EXCHANGE  _iocp_try_ssl_exchange
#define _ADD_ACPFD_INLOOP  _iocp_add_acpfd_inloop
#else
#include "event/uev.h"
#define _CHECK_SKID        _uev_check_skid
#define _GET_UD            _uev_get_ud
#define _TRY_SSL_EXCHANGE  _uev_try_ssl_exchange
#define _ADD_ACPFD_INLOOP  _uev_add_acpfd_inloop
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

// 根据fd计算目标watcher并发送命令的便利宏
#define _SEND_CMD(ev, cmd)\
    watcher_ctx *watcher = GET_PTR((ev)->watcher, (ev)->nthreads, cmd.fd);\
    _send_cmd(watcher, &cmd)

void _send_cmd(watcher_ctx *watcher, cmd_ctx *cmd) {
    int32_t erro;
#ifdef EV_IOCP
    overlap_cmd_ctx *olcmd = &watcher->cmd;
    fsqu_push(&olcmd->qu, cmd);
    static const char trigger[1] = { 's' };
    while (0 == watcher->stop
        && SOCKET_ERROR == send(olcmd->fd, trigger, sizeof(trigger), 0)) {
        erro = ERRNO;
        ASSERTAB(IS_EAGAIN(erro), ERRORSTR(erro));
        CPU_PAUSE();
    }
#else
    char *buf;
    size_t blens;
#if CMD_PIPE_QU
    fsqu_push(&watcher->pipe.qu, cmd);
    static const char trigger[1] = { 's' };
    buf = (char *)trigger;
    blens = sizeof(trigger);
#else
    buf = (char *)cmd;
    blens = sizeof(cmd_ctx);
#endif
    while (0 == watcher->stop
           && ERR_FAILED == write(watcher->pipe.pipes[1], buf, blens)) {
        erro = ERRNO;
        ASSERTAB(ERR_RW_RETRIABLE(erro), ERRORSTR(erro));
        CPU_PAUSE();
    };
#endif//EV_IOCP
}
void _cmd_add_acpfd(watcher_ctx *watcher, SOCKET fd, struct listener_ctx *lsn) {
    cmd_ctx cmd = { 0 };
    cmd.cmd = CMD_ADDACP;
    cmd.fd = fd;
    cmd.arg = (uint64_t)lsn;
    _send_cmd(watcher, &cmd);
}
void _on_cmd_addacp(watcher_ctx *watcher, cmd_ctx *cmd) {
    struct listener_ctx *lsn = (struct listener_ctx *)cmd->arg;
    _ADD_ACPFD_INLOOP(watcher, cmd->fd, lsn);
#ifdef EV_IOCP
    // 配对 _on_accept_cb path 3 投递前 ref++ 占位：归零路径释放 lsn
    _iocp_try_freelsn(lsn);
#else
    _uev_qtn_freelsn(watcher, lsn);
#endif
}
void _cmd_connect(ev_ctx *ctx, struct sock_ctx *skctx, void *arg) {
    cmd_ctx cmd = { 0 };
    cmd.cmd = CMD_CONN;
    cmd.fd = skctx->fd;
    cmd.arg = (uint64_t)skctx;
    cmd.skid = (uint64_t)arg;
    _SEND_CMD(ctx, cmd);
}
void _on_cmd_conn(watcher_ctx *watcher, cmd_ctx *cmd) {
#ifdef EV_IOCP
    netaddr_ctx *addr = (netaddr_ctx *)cmd->skid;
    _iocp_add_conn_inloop(watcher, (sock_ctx *)cmd->arg, addr);
    FREE(addr);
#else
    _uev_add_conn_inloop(watcher, (sock_ctx *)cmd->arg);
#endif
}
void _cmd_add(watcher_ctx *watcher, sock_ctx *skctx) {
    cmd_ctx cmd = { 0 };
    cmd.cmd = CMD_ADD;
    cmd.arg = (uint64_t)skctx;
    _send_cmd(watcher, &cmd);
}
void _on_cmd_add(watcher_ctx *watcher, cmd_ctx *cmd) {
    sock_ctx *skctx = (sock_ctx *)cmd->arg;
#ifdef EV_IOCP
    _iocp_add_fd_inloop(watcher, skctx);
#else
    _uev_add_fd_inloop(watcher, skctx);
#endif
}
#ifndef EV_IOCP
void _cmd_listen(watcher_ctx *watcher, sock_ctx *skctx) {
    cmd_ctx cmd = { 0 };
    cmd.cmd = CMD_LSN;
    cmd.arg = (uint64_t)skctx;
    _send_cmd(watcher, &cmd);
}
void _on_cmd_lsn(watcher_ctx *watcher, cmd_ctx *cmd) {
    _uev_add_lsn_inloop(watcher, (sock_ctx *)cmd->arg);
}
void _cmd_unlisten(watcher_ctx *watcher, SOCKET fd, struct listener_ctx *lsn) {
    cmd_ctx cmd = { 0 };
    cmd.cmd = CMD_UNLSN;
    cmd.fd = fd;
    cmd.arg = (uint64_t)lsn;
    _send_cmd(watcher, &cmd);
}
void _on_cmd_unlsn(watcher_ctx *watcher, cmd_ctx *cmd) {
    _uev_remove_lsn(watcher, cmd->fd, (struct listener_ctx *)cmd->arg);
}
void _cmd_lsn_unref(watcher_ctx *watcher, struct listener_ctx *lsn) {
    cmd_ctx cmd = { 0 };
    cmd.cmd = CMD_LSN_UNREF;
    cmd.arg = (uint64_t)lsn;
    // cmd 不关联 fd, 任 watcher 接收即可
    _send_cmd(watcher, &cmd);
}
void _on_cmd_lsn_unref(watcher_ctx *watcher, cmd_ctx *cmd) {
    _uev_qtn_freelsn(watcher, (struct listener_ctx *)cmd->arg);
}
#endif
void _on_cmd_stop(watcher_ctx *watcher, cmd_ctx *cmd) {
    (void)cmd;
    watcher->stop = 1;
}
void ev_close(ev_ctx *ctx, SOCKET fd, uint64_t skid, int32_t immed) {
    if (INVALID_SOCK == fd) {
        return;
    }
    cmd_ctx cmd = { 0 };
    cmd.cmd = CMD_DISCONN;
    cmd.fd = fd;
    cmd.skid = skid;
    cmd.arg = (uint64_t)immed;
    _SEND_CMD(ctx, cmd);
}
void _on_cmd_disconn(watcher_ctx *watcher, cmd_ctx *cmd) {
    sock_ctx *skctx = _evpub_sockel_get(watcher, cmd->fd);
    if (NULL == skctx
        || ERR_OK != _CHECK_SKID(skctx, cmd->skid)) {
        return;
    }
#ifdef EV_IOCP
    _iocp_disconnect(skctx, (int32_t)cmd->arg);
#else
    _uev_disconnect(watcher, skctx, (int32_t)cmd->arg);
#endif
}
int32_t ev_ssl(ev_ctx *ctx, SOCKET fd, uint64_t skid, int32_t client, struct evssl_ctx *evssl) {
    if (INVALID_SOCK == fd || NULL == evssl) {
        return ERR_FAILED;
    }
#if WITH_SSL
    cmd_ctx cmd;
    cmd.cmd = CMD_SSL;
    cmd.fd = fd;
    cmd.skid = skid;
    cmd.len = (size_t)client;
    cmd.arg = (uint64_t)evssl;
    _SEND_CMD(ctx, cmd);
#else
    (void)ctx;
    (void)skid;
    (void)client;
    (void)evssl;
#endif
    return ERR_OK;
}
void _on_cmd_ssl(watcher_ctx *watcher, cmd_ctx *cmd) {
    sock_ctx *skctx = _evpub_sockel_get(watcher, cmd->fd);
    if (NULL == skctx
        || ERR_OK != _CHECK_SKID(skctx, cmd->skid)) {
        return;
    }
    _TRY_SSL_EXCHANGE(watcher, skctx, (struct evssl_ctx *)cmd->arg, (int32_t)cmd->len);
}
int32_t ev_send(ev_ctx *ctx, SOCKET fd, uint64_t skid, void *data, size_t len, int32_t copy) {
    if (INVALID_SOCK == fd || 0 == len) {
        if (!copy) {
            FREE(data);
        }
        return ERR_FAILED;
    }
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
    return ERR_OK;
}
void _on_cmd_send(watcher_ctx *watcher, cmd_ctx *cmd) {
    void *data = (void *)cmd->arg;
    sock_ctx *skctx = _evpub_sockel_get(watcher, cmd->fd);
    if (NULL == skctx
        || ERR_OK != _CHECK_SKID(skctx, cmd->skid)) {
        FREE(data);
        return;
    }
    // ev_send 仅适用于 TCP；UDP fd 用 ev_sendto（CMD_SENDTO 路径）。误用时丢弃数据避免
    // 把裸 payload 当 [netaddr_ctx + payload] 在 UDP 写路径中按错位读取地址发出。
    if (SOCK_STREAM != skctx->type) {
        LOG_ERROR("ev_send called on non-TCP fd %d, drop.", (int32_t)cmd->fd);
        FREE(data);
        return;
    }
    off_buf_ctx buf;
    buf.data = data;
    buf.lens = cmd->len;
    buf.offset = 0;
    buf.shared = NULL;
#ifdef EV_IOCP
    _iocp_add_bufs_trypost(skctx, &buf);
#else
    _uev_add_write_inloop(watcher, skctx, &buf);
#endif
}
int32_t ev_send_multi(ev_ctx *ctx, SOCKET fds[], uint64_t skids[], int32_t n,
                      void *data, size_t len, int32_t copy) {
    if (0 == len) {
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
    cmd_ctx cmd;
    cmd.cmd = CMD_SEND_MULTI;
    cmd.len = len;
    cmd.arg = (uint64_t)pack;
    for (i = 0; i < n; i++) {
        if (INVALID_SOCK == fds[i]) {
            continue;
        }
        cmd.fd = fds[i];
        cmd.skid = skids[i];
        _SEND_CMD(ctx, cmd);
    }
    return ERR_OK;
}
void _on_cmd_send_multi(watcher_ctx *watcher, cmd_ctx *cmd) {
    shared_data *pack = (shared_data *)cmd->arg;
    sock_ctx *skctx = _evpub_sockel_get(watcher, cmd->fd);
    // sock 失效或非 TCP：本 fd 这份引用归还,归 0 时释放。正常路径由 off_buf_ctx.shared 承载共享 ref 减；
    // 此处未构造 off_buf_ctx,直接 ATOMIC_ADD -1 即可
    if (NULL == skctx
        || ERR_OK != _CHECK_SKID(skctx, cmd->skid)
        || SOCK_STREAM != skctx->type) {
        if (1 == ATOMIC_ADD(&pack->ref, -1)) {
            FREE(pack->data);
            FREE(pack);
        }
        return;
    }
    off_buf_ctx buf;
    buf.data = pack->data;
    buf.lens = cmd->len;
    buf.offset = 0;
    buf.shared = pack;
#ifdef EV_IOCP
    _iocp_add_bufs_trypost(skctx, &buf);
#else
    _uev_add_write_inloop(watcher, skctx, &buf);
#endif
}
int32_t ev_sendto(ev_ctx *ctx, SOCKET fd, uint64_t skid, const char *ip, const uint16_t port,
    void *data, size_t len, int32_t copy) {
    if (INVALID_SOCK == fd) {
        if (!copy) {
            FREE(data);
        }
        return ERR_FAILED;
    }
    cmd_ctx cmd;
    cmd.cmd = CMD_SENDTO;
    cmd.fd = fd;
    cmd.skid = skid;
    cmd.len = len;
    char *buf;
    MALLOC(buf, sizeof(netaddr_ctx) + len);
    netaddr_ctx *addr = (netaddr_ctx *)buf;
    if (ERR_OK != netaddr_set(addr, ip, port)) {
        FREE(buf);
        if (!copy) {
            FREE(data);
        }
        LOG_WARN("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    memcpy(buf + sizeof(netaddr_ctx), data, len);
    cmd.arg = (uint64_t)buf;
    _SEND_CMD(ctx, cmd);
    if (!copy) {
        FREE(data);
    }
    return ERR_OK;
}
void _on_cmd_sendto(watcher_ctx *watcher, cmd_ctx *cmd) {
    void *data = (void *)cmd->arg;
    sock_ctx *skctx = _evpub_sockel_get(watcher, cmd->fd);
    if (NULL == skctx
        || ERR_OK != _CHECK_SKID(skctx, cmd->skid)) {
        FREE(data);
        return;
    }
    // ev_sendto 仅适用于 UDP；TCP fd 用 ev_send（CMD_SEND 路径）。误用时丢弃数据避免
    // 把 [netaddr_ctx + payload] 整段经 TCP 发送路径发出（netaddr 前缀污染对端协议解析）。
    if (SOCK_DGRAM != skctx->type) {
        LOG_ERROR("ev_sendto called on non-UDP fd %d, drop.", (int32_t)cmd->fd);
        FREE(data);
        return;
    }
    off_buf_ctx buf;
    buf.data = data;
    buf.lens = cmd->len;
    buf.offset = 0;
    buf.shared = NULL;
#ifdef EV_IOCP
    _iocp_add_bufs_trysendto(skctx, &buf);
#else
    _uev_add_write_inloop(watcher, skctx, &buf);
#endif
}
// UDP 多播 4 个公开 API 走同一 cmd 投递路径,差异只在 udp_opt_arg 字段填充
static int32_t _send_cmd_udp_opt(ev_ctx *ctx, SOCKET fd, uint64_t skid, udp_opt_arg *arg) {
    cmd_ctx cmd;
    cmd.cmd = CMD_UDP_OPT;
    cmd.fd = fd;
    cmd.skid = skid;
    cmd.len = 0;
    cmd.arg = (uint64_t)arg;
    _SEND_CMD(ctx, cmd);
    return ERR_OK;
}
int32_t ev_udp_join(ev_ctx *ctx, SOCKET fd, uint64_t skid,
                    const char *group_ip, const char *iface_str) {
    if (INVALID_SOCK == fd || NULL == group_ip) {
        return ERR_FAILED;
    }
    udp_opt_arg *arg;
    MALLOC(arg, sizeof(udp_opt_arg));
    arg->op = UDP_OPT_JOIN;
    arg->ttl = 0;
    arg->loop = 0;
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
    MALLOC(arg, sizeof(udp_opt_arg));
    arg->op = UDP_OPT_LEAVE;
    arg->ttl = 0;
    arg->loop = 0;
    safe_fill_str(arg->group_ip, sizeof(arg->group_ip), group_ip);
    safe_fill_str(arg->iface_str, sizeof(arg->iface_str), iface_str);
    return _send_cmd_udp_opt(ctx, fd, skid, arg);
}
int32_t ev_udp_ttl(ev_ctx *ctx, SOCKET fd, uint64_t skid, uint8_t ttl) {
    if (INVALID_SOCK == fd) {
        return ERR_FAILED;
    }
    udp_opt_arg *arg;
    MALLOC(arg, sizeof(udp_opt_arg));
    arg->op = UDP_OPT_TTL;
    arg->ttl = ttl;
    arg->loop = 0;
    arg->group_ip[0] = '\0';
    arg->iface_str[0] = '\0';
    return _send_cmd_udp_opt(ctx, fd, skid, arg);
}
int32_t ev_udp_loop(ev_ctx *ctx, SOCKET fd, uint64_t skid, int32_t enable) {
    if (INVALID_SOCK == fd) {
        return ERR_FAILED;
    }
    udp_opt_arg *arg;
    MALLOC(arg, sizeof(udp_opt_arg));
    arg->op = UDP_OPT_LOOP;
    arg->ttl = 0;
    arg->loop = enable ? 1 : 0;
    arg->group_ip[0] = '\0';
    arg->iface_str[0] = '\0';
    return _send_cmd_udp_opt(ctx, fd, skid, arg);
}
// 事件线程内执行 UDP 多播 setsockopt：先取 sock family,按 IPv4/IPv6 分支调对应 IP_*/IPV6_* 选项;
// Windows 路径下 IPv6 iface_str 忽略走默认接口(if_nametoindex 需 iphlpapi.lib,不引入依赖)
void _on_cmd_udp_opt(watcher_ctx *watcher, cmd_ctx *cmd) {
    udp_opt_arg *arg = (udp_opt_arg *)cmd->arg;
    sock_ctx *skctx = _evpub_sockel_get(watcher, cmd->fd);
    if (NULL == skctx || ERR_OK != _CHECK_SKID(skctx, cmd->skid)) {
        FREE(arg);
        return;
    }
    if (SOCK_DGRAM != skctx->type) {
        LOG_ERROR("ev_udp_* called on non-UDP fd %d, drop.", (int32_t)cmd->fd);
        FREE(arg);
        return;
    }
    int32_t family = sock_family(cmd->fd);
    if (ERR_FAILED == family) {
        LOG_ERROR("sock_family(fd=%d) failed: %s", (int32_t)cmd->fd, ERRORSTR(ERRNO));
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
            rtn = setsockopt(cmd->fd, IPPROTO_IP, opt, (const char *)&mreq, sizeof(mreq));
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
            rtn = setsockopt(cmd->fd, IPPROTO_IPV6, opt, (const char *)&mreq, sizeof(mreq));
        }
        break;
    case UDP_OPT_TTL:
        if (AF_INET == family) {
            uint8_t ttl = arg->ttl;
            rtn = setsockopt(cmd->fd, IPPROTO_IP, IP_MULTICAST_TTL, (const char *)&ttl, sizeof(ttl));
        } else if (AF_INET6 == family) {
            int32_t hops = (int32_t)arg->ttl;
            rtn = setsockopt(cmd->fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, (const char *)&hops, sizeof(hops));
        }
        break;
    case UDP_OPT_LOOP:
        if (AF_INET == family) {
            uint8_t loop = (uint8_t)(arg->loop ? 1 : 0);
            rtn = setsockopt(cmd->fd, IPPROTO_IP, IP_MULTICAST_LOOP, (const char *)&loop, sizeof(loop));
        } else if (AF_INET6 == family) {
            int32_t loop = arg->loop ? 1 : 0;
            rtn = setsockopt(cmd->fd, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, (const char *)&loop, sizeof(loop));
        }
        break;
    }
    if (0 != rtn) {
        LOG_ERROR("UDP opt %d failed on fd %d: %s", (int32_t)arg->op, (int32_t)cmd->fd, ERRORSTR(ERRNO));
    }
    FREE(arg);
}
static void _cmd_set_ud(ev_ctx *ctx, SOCKET fd, uint64_t skid, int32_t type, uint64_t val) {
    if (INVALID_SOCK == fd) {
        return;
    }
    cmd_ctx cmd;
    cmd.cmd = CMD_SETUD;
    cmd.fd = fd;
    cmd.skid = skid;
    cmd.len = (size_t)type;
    cmd.arg = val;
    _SEND_CMD(ctx, cmd);
}
void ev_ud_pktype(ev_ctx *ctx, SOCKET fd, uint64_t skid, subtype_t pktype) {
    _cmd_set_ud(ctx, fd, skid, UD_PKTYPE, pktype);
}
void ev_ud_status(ev_ctx *ctx, SOCKET fd, uint64_t skid, uint8_t status) {
    _cmd_set_ud(ctx, fd, skid, UD_STATUS, status);
}
void ev_ud_sess(ev_ctx *ctx, SOCKET fd, uint64_t skid, uint64_t sess) {
    _cmd_set_ud(ctx, fd, skid, UD_SESS, sess);
}
void ev_ud_handle(ev_ctx *ctx, SOCKET fd, uint64_t skid, name_t handle) {
    _cmd_set_ud(ctx, fd, skid, UD_HANDLE, handle);
}
void ev_ud_context(ev_ctx *ctx, SOCKET fd, uint64_t skid, void *extra) {
    _cmd_set_ud(ctx, fd, skid, UD_CONTEXT, (uint64_t)extra);
}
void ev_ud_seccontext(ev_ctx *ctx, SOCKET fd, uint64_t skid, void *extra) {
    _cmd_set_ud(ctx, fd, skid, UD_SECCONTEXT, (uint64_t)extra);
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
    sock_ctx *skctx = _evpub_sockel_get(watcher, cmd->fd);
    if (NULL == skctx
        || ERR_OK != _CHECK_SKID(skctx, cmd->skid)) {
        return;
    }
    _set_ud(_GET_UD(skctx), (int32_t)cmd->len, cmd->arg);
}
