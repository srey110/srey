#include "containers/hashmap.h"
#include "utils/netutils.h"
#ifdef EV_IOCP
#include "event/iocp.h"
#else
#include "event/uev.h"
#endif

#if WITH_SSL
// ev_send_ssl 的命令载荷:明文尾包与升级参数走同一条命令。
// data 非 NULL 表示载荷仍归命令所有,交给发送队列后置 NULL
typedef struct sendssl_ctx {
    int32_t client;
    uint64_t skid;
    void *data;
    struct evssl_ctx *evssl;
}sendssl_ctx;
#endif
// ev_ud_* 四个 setter 只差字段名与 cast: 展开成"props 回调 + 对外入口"一对。
// 值走 number 而非 data,故不需要 fcb
#define DEF_UD_SETTER(field, type)  \
static int32_t _cmd_ud_##field(struct watcher_ctx *watcher, struct evsock_ctx *evsk, \
    void *data, uint64_t number) { \
    (void)watcher; \
    (void)data; \
    _evpub_get_ud(evsk)->field = (type)number; \
    return 0; \
} \
int32_t ev_ud_##field(ev_ctx *ctx, sock_ctx *sk, type field) { \
    return ev_props(ctx, sk, _cmd_ud_##field, NULL, NULL, field); \
}

int32_t _evpub_checkid(struct evsock_ctx *evsk, const uint64_t skid) {
#ifdef EV_IOCP
    return _iocp_check_skid(evsk, skid);
#else
    return _uev_check_skid(evsk, skid);
#endif
}
ud_cxt *_evpub_get_ud(struct evsock_ctx *evsk) {
#ifdef EV_IOCP
    return _iocp_get_ud(evsk);
#else
    return _uev_get_ud(evsk);
#endif
}
// 取 sk 所属 watcher。index 在建对象时就写定,越界说明拿到的 sock_ctx 没经过框架的创建路径
// (未初始化的栈变量、或已 sock_set_invalid 却仍被使用)。这里断言而不是静默纠正:
// 投错 watcher 只表现为"命令没生效",是最难查的一类
static inline watcher_ctx *_cmd_get_watcher(ev_ctx *ctx, sock_ctx *sk) {
    ASSERTAB(sk->index >= 0 && sk->index < (int32_t)ctx->nthreads, "sock index out of range.");
    return &ctx->watcher[sk->index];
}
#ifdef NO_CMD_PIPE
static inline void _cmd_note_trigger(watcher_ctx *watcher, pip_ctx *pip) {
    int32_t erro;
#ifdef EV_KQUEUE
    changes_t kev;
    EV_SET(&kev, 0, EVFILT_USER, 0, NOTE_TRIGGER, 0, &pip->skpip);
    while (0 == ATOMIC_GET(&watcher->stop)//while 防止 "EINTR 系统调用被信号打断了,什么都没做"。Windows侧无
           && ERR_FAILED == kevent(watcher->evfd, &kev, 1, NULL, 0, NULL)) {
        erro = ERRNO;
        ASSERTAB(EINTR == erro, ERRORSTR(erro));
    }
#else
    #error "Unsupported!"
#endif
}
#endif
// 唤醒只管叫醒事件线程,命令在无界队列里,消费端抽的是队列到空,故唤醒数与命令数不必对齐。
// 标志为 1 时不投唤醒,CAS 抢到才投;标志只能靠 CAS 判——MSVC 没有足序的原子读。
// 消费端清 0 必须在抽队列之前,颠倒会丢唤醒。Unix 是命令通道就绪时清 0 再抽,在途最多一个字节,
// 管道填不满,故写失败只可能是 EINTR;IOCP 是事件线程醒着时把标志拿在 1、进等待前清 0 再抽
// (见 _iocp_loop_event),满批返回时上个命令包可能还没取出,在途可多于一个,无害:消费端抽到空为止。
// stop 置位后不再唤醒也不丢命令:watcher 已在退出路径上,队列残留由 ev_free 的 drain 收
void _send_cmd(watcher_ctx *watcher, cmd_ctx *cmd) {
#ifdef EV_IOCP
    overlap_cmd_ctx *olcmd = &watcher->cmd;
    cmdq_push(&olcmd->qu, cmd);
    if (ATOMIC_CAS(&olcmd->wake_pending, 0, 1)
        && 0 == ATOMIC_GET(&watcher->stop)) {
        int32_t posted = (int32_t)PostQueuedCompletionStatus(watcher->iocp, 0, 0, &olcmd->ol_r.overlapped);
        ASSERTAB(0 != posted, ERRORSTR(ERRNO));
    }
#else
    pip_ctx *pip = &watcher->pipe;
    cmdq_push(&pip->qu, cmd);
    if (ATOMIC_CAS(&pip->wake_pending, 0, 1)) {
#ifdef NO_CMD_PIPE
        _cmd_note_trigger(watcher, pip);
#else
        int32_t erro;
        static const char trigger[1] = { 's' };
        while (0 == ATOMIC_GET(&watcher->stop)//while 防止 "EINTR 系统调用被信号打断了,什么都没做"。Windows侧无
               && ERR_FAILED == write(pip->pipes[1], trigger, sizeof(trigger))) {
            erro = ERRNO;
            ASSERTAB(EINTR == erro, ERRORSTR(erro));
        }
#endif// NO_CMD_PIPE
    }
#endif//EV_IOCP
}
void _cmd_add_acpfd(watcher_ctx *watcher, SOCKET fd, struct listener_ctx *lsn) {
    cmd_ctx cmd = { 0 };
    cmd.cmd = CMD_ADDACP;
    cmd.sk.fd = fd;
    cmd.args.lsn = lsn;
    _send_cmd(watcher, &cmd);
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
static inline void _add_conn_inloop(watcher_ctx *watcher, struct evsock_ctx *evsk,
    netaddr_ctx *addr) {
#ifdef EV_IOCP
    _iocp_add_conn_inloop(watcher, evsk, addr);
#else
    (void)addr;
    _uev_add_conn_inloop(watcher, evsk);
#endif
}
void _cmd_connect(ev_ctx *ctx, struct evsock_ctx *evsk, netaddr_ctx *addr) {
    watcher_ctx *watcher = _cmd_get_watcher(ctx, &evsk->sk);
    if (_evpub_inloop(watcher)) {
        _add_conn_inloop(watcher, evsk, addr);
        return;
    }
    cmd_ctx cmd = { 0 };
    cmd.cmd = CMD_CONN;
    cmd.args.conn.evsk = evsk;
    if (NULL != addr) {
        cmd.args.conn.addr = *addr;
    }
    _send_cmd(watcher, &cmd);
}
void _on_cmd_conn(watcher_ctx *watcher, cmd_ctx *cmd) {
    _add_conn_inloop(watcher, cmd->args.conn.evsk, &cmd->args.conn.addr);
}
static inline void _add_fd_inloop(watcher_ctx *watcher, evsock_ctx *evsk) {
#ifdef EV_IOCP
    _iocp_add_fd_inloop(watcher, evsk);
#else
    _uev_add_fd_inloop(watcher, evsk);
#endif
}
void _cmd_add(watcher_ctx *watcher, evsock_ctx *evsk) {
    if (_evpub_inloop(watcher)) {
        _add_fd_inloop(watcher, evsk);
        return;
    }
    cmd_ctx cmd = { 0 };
    cmd.cmd = CMD_ADD;
    cmd.args.evsk = evsk;
    _send_cmd(watcher, &cmd);
}
void _on_cmd_add(watcher_ctx *watcher, cmd_ctx *cmd) {
    _add_fd_inloop(watcher, cmd->args.evsk);
}
#ifndef EV_IOCP
void _cmd_listen(watcher_ctx *watcher, evsock_ctx *evsk) {
    if (_evpub_inloop(watcher)) {
        _uev_add_lsn_inloop(watcher, evsk);
        return;
    }
    cmd_ctx cmd = { 0 };
    cmd.cmd = CMD_LSN;
    cmd.args.evsk = evsk;
    _send_cmd(watcher, &cmd);
}
void _on_cmd_lsn(watcher_ctx *watcher, cmd_ctx *cmd) {
    _uev_add_lsn_inloop(watcher, cmd->args.evsk);
}
// 不带 fd:该 listener 在本 watcher 上的 fd 由 watcher 自己持有,理由见 _uev_remove_lsn
void _cmd_unlisten(watcher_ctx *watcher, struct listener_ctx *lsn) {
    if (_evpub_inloop(watcher)) {
        _uev_remove_lsn(watcher, lsn);
        return;
    }
    cmd_ctx cmd = { 0 };
    cmd.cmd = CMD_UNLSN;
    cmd.args.lsn = lsn;
    _send_cmd(watcher, &cmd);
}
void _on_cmd_unlsn(watcher_ctx *watcher, cmd_ctx *cmd) {
    _uev_remove_lsn(watcher, cmd->args.lsn);
}
void _cmd_lsn_unref(watcher_ctx *watcher, struct listener_ctx *lsn) {
    if (_evpub_inloop(watcher)) {
        _uev_qtn_freelsn(watcher, lsn);
        return;
    }
    cmd_ctx cmd = { 0 };
    cmd.cmd = CMD_LSN_UNREF;
    cmd.args.lsn = lsn;
    // cmd 不关联 fd, 任 watcher 接收即可
    _send_cmd(watcher, &cmd);
}
void _on_cmd_lsn_unref(watcher_ctx *watcher, cmd_ctx *cmd) {
    _uev_qtn_freelsn(watcher, cmd->args.lsn);
}
#endif
void _on_cmd_stop(watcher_ctx *watcher, cmd_ctx *cmd) {
    (void)cmd;
#ifdef EV_IOCP
    _iocp_disconnect_all(watcher);
#endif
    ATOMIC_SET_RELAXED(&watcher->stop, 1);
}
static inline void _cmd_props(watcher_ctx *watcher, sock_ctx *sk,
                          props_cb ppcb, free_cb fcb, void *data, uint64_t number) {
    evsock_ctx *evsk = _evpub_sockel_get(watcher, sk->fd);
    if (NULL == evsk
        || ERR_OK != _evpub_checkid(evsk, sk->skid)) {
        UD_FREE(fcb, data);
        return;
    }
    if (ppcb(watcher, evsk, data, number)) {
        UD_FREE(fcb, data);
    }
}
int32_t ev_props(ev_ctx *ctx, sock_ctx *sk,
                 props_cb ppcb, free_cb fcb, void *data, uint64_t number) {
    if (sock_is_invalid(sk) || NULL == ppcb) {
        UD_FREE(fcb, data);
        return ERR_FAILED;
    }
    watcher_ctx *watcher = _cmd_get_watcher(ctx, sk);
    if (_evpub_inloop(watcher)) {
        _cmd_props(watcher, sk, ppcb, fcb, data, number);
        return ERR_OK;
    }
    cmd_ctx cmd = { 0 };
    cmd.cmd = CMD_PROPS;
    cmd.sk = *sk;
    cmd.args.props.ppcb = ppcb;
    cmd.args.props.fcb = fcb;
    cmd.args.props.number = number;
    cmd.args.props.data = data;
    _send_cmd(watcher, &cmd);
    return ERR_OK;
}
void _on_cmd_props(struct watcher_ctx *watcher, cmd_ctx *cmd) {
    _cmd_props(watcher, &cmd->sk,
        cmd->args.props.ppcb, cmd->args.props.fcb,
        cmd->args.props.data, cmd->args.props.number);
}
static int32_t _ev_close(struct watcher_ctx *watcher, struct evsock_ctx *evsk,
    void *data, uint64_t number) {
    (void)data;
    (void)number;
    _evpub_disconnect(watcher, evsk);
    return 0;
}
int32_t ev_close(ev_ctx *ctx, sock_ctx *sk) {
    return ev_props(ctx, sk, _ev_close, NULL, NULL, 0);
}
#if WITH_SSL
static int32_t _ev_keyupdate(struct watcher_ctx *watcher, struct evsock_ctx *evsk,
    void *data, uint64_t number) {
    (void)data;
#ifdef EV_IOCP
    _iocp_keyupdate(watcher, evsk, (int32_t)number);
#else
    _uev_keyupdate(watcher, evsk, (int32_t)number);
#endif
    return 0;
}
#endif
int32_t ev_keyupdate(ev_ctx *ctx, sock_ctx *sk, int32_t updatetype) {
#if WITH_SSL
    return ev_props(ctx, sk, _ev_keyupdate, NULL, NULL, (uint64_t)updatetype);
#else
    (void)ctx;
    (void)sk;
    (void)updatetype;
    return ERR_FAILED;
#endif
}
#if WITH_SSL
static int32_t _ev_ssl(struct watcher_ctx *watcher, struct evsock_ctx *evsk,
    void *data, uint64_t number) {
#ifdef EV_IOCP
    _iocp_try_ssl_exchange(watcher, evsk, data, (int32_t)number);
#else
    _uev_try_ssl_exchange(watcher, evsk, data, (int32_t)number);
#endif
    return 0;
}
#endif
int32_t ev_ssl(ev_ctx *ctx, sock_ctx *sk, int32_t client, struct evssl_ctx *evssl) {
#if WITH_SSL
    if (NULL == evssl) {
        return ERR_FAILED;
    }
    return ev_props(ctx, sk, _ev_ssl, NULL, evssl, client);
#else
    (void)ctx;
    (void)sk;
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
static inline void _ev_bufs_send(struct watcher_ctx *watcher, struct evsock_ctx *evsk, off_buf_ctx *buf) {
#ifdef EV_IOCP
    _iocp_add_bufs_trypost(watcher, evsk, buf);
#else
    _uev_add_bufs_send(watcher, evsk, buf);
#endif
}
static int32_t _ev_send(struct watcher_ctx *watcher, struct evsock_ctx *evsk,
    void *data, uint64_t number) {
    if (SOCK_STREAM != evsk->type) {
        LOG_ERROR("ev_send called on non-TCP fd %d, drop.", (int32_t)evsk->sk.fd);
        return 1;
    }
    off_buf_ctx buf = { 0 };
    buf.data = data;
    buf.lens = number;
    _ev_bufs_send(watcher, evsk, &buf);
    return 0;
}
int32_t ev_send(ev_ctx *ctx, sock_ctx *sk, void *data, size_t len, int32_t copy) {
    if (sock_is_invalid(sk) || EMPTYPTR(data, len)) {
        CHECK_COPY_FREE(data, copy);
        return ERR_FAILED;
    }
    return ev_props(ctx, sk, _ev_send, _free,
        _cmd_cpy_buf(data, len, copy), len);
}
#if WITH_SSL
static void _ev_sendssl_free(void *arg) {
    sendssl_ctx *req = arg;
    FREE(req->data);
    FREE(req);
}
// 尾包入队与武装升级必须落在同一次命令回调内:拆成两条命令投,发起方在两者之间被抢占,
// 对端的握手报文就会被当明文协议解。先入队后武装,这一包在握手期禁发闸看来就是普通 ev_send
static int32_t _ev_send_ssl(struct watcher_ctx *watcher, struct evsock_ctx *evsk,
    void *data, uint64_t number) {
    if (SOCK_STREAM != evsk->type) {
        LOG_ERROR("ev_send_ssl called on non-TCP fd %d, drop.", (int32_t)evsk->sk.fd);
        return 1;
    }
    sendssl_ctx *req = data;
    SOCKET fd = evsk->sk.fd;
    off_buf_ctx buf = { 0 };
    buf.data = req->data;
    buf.lens = (size_t)number;
    req->data = NULL;// 载荷已易主:进队列或被 _evpub_off_buf_release 收掉
    _ev_bufs_send(watcher, evsk, &buf);
    // 入队内部可能已关掉连接,旧指针不能再用:重新查表并校验 skid,口径同 _on_cmd_props
    evsk = _evpub_sockel_get(watcher, fd);
    if (NULL != evsk
        && ERR_OK == _evpub_checkid(evsk, req->skid)) {
        _ev_ssl(watcher, evsk, req->evssl, (uint64_t)req->client);
    }
    return 1;
}
#endif
int32_t ev_send_ssl(ev_ctx *ctx, sock_ctx *sk, void *data, size_t len,
    int32_t copy, int32_t client, struct evssl_ctx *evssl) {
#if WITH_SSL
    if (sock_is_invalid(sk)
        || EMPTYPTR(data, len)
        || NULL == evssl) {
        CHECK_COPY_FREE(data, copy);
        return ERR_FAILED;
    }
    sendssl_ctx *req;
    MALLOC(req, sizeof(sendssl_ctx));
    req->client = client;
    req->skid = sk->skid;
    req->data = _cmd_cpy_buf(data, len, copy);
    req->evssl = evssl;
    return ev_props(ctx, sk, _ev_send_ssl, _ev_sendssl_free, req, len);
#else
    (void)ctx;
    (void)sk;
    (void)len;
    (void)client;
    (void)evssl;
    CHECK_COPY_FREE(data, copy);
    return ERR_FAILED;
#endif
}
static int32_t _ev_send_multi(struct watcher_ctx *watcher, struct evsock_ctx *evsk,
    void *data, uint64_t number) {
    if (SOCK_STREAM != evsk->type) {
        LOG_ERROR("ev_send_multi called on non-TCP fd %d, drop.", (int32_t)evsk->sk.fd);
        return 1;
    }
    shared_data *pack = data;
    off_buf_ctx buf = { 0 };
    buf.data = pack->data;
    buf.lens = (size_t)number;
    buf.shared = pack;
    _ev_bufs_send(watcher, evsk, &buf);
    return 0;
}
int32_t ev_send_multi(ev_ctx *ctx, sock_ctx sks[], int32_t n,
                      void *data, size_t len, int32_t copy) {
    if (EMPTYPTR(data, len)) {
        CHECK_COPY_FREE(data, copy);
        return ERR_FAILED;
    }
    // 先扫一遍有效 fd 数,决定 pack->ref 初值
    int32_t valid = 0;
    int32_t i;
    for (i = 0; i < n; i++) {
        if (!sock_is_invalid(&sks[i])) {
            valid++;
        }
    }
    if (0 == valid) {
        CHECK_COPY_FREE(data, copy);
        return ERR_FAILED;
    }
    shared_data *pack;
    MALLOC(pack, sizeof(shared_data));
    pack->data = _cmd_cpy_buf(data, len, copy);
    ATOMIC_SET_RELAXED(&pack->ref, valid);
    cmd_ctx cmd = { 0 };
    cmd.cmd = CMD_PROPS;
    cmd.args.props.ppcb = _ev_send_multi;
    cmd.args.props.fcb = _evpub_share_data_free;
    cmd.args.props.data = pack;
    cmd.args.props.number = len;
    watcher_ctx *watcher;
    // 每个有效 fd 投递一次,与 pack->ref 初值 valid 配平：每条命令最终经 _on_cmd_props
    // 或 ev_free 的 drain(case CMD_PROPS)各减一次 ref,归零时释放 pack
    for (i = 0; i < n; i++) {
        if (sock_is_invalid(&sks[i])) {
            continue;
        }
        cmd.sk = sks[i];
        watcher = _cmd_get_watcher(ctx, &sks[i]);
        if (_evpub_inloop(watcher)) {
            _on_cmd_props(watcher, &cmd);
        } else {
            _send_cmd(watcher, &cmd);
        }
    }
    return ERR_OK;
}
int32_t ev_sendto(ev_ctx *ctx, sock_ctx *sk, const char *ip, const uint16_t port,
    void *data, size_t len, int32_t copy) {
    netaddr_ctx addr;
    if (ERR_OK != netaddr_set(&addr, ip, port)) {
        CHECK_COPY_FREE(data, copy);
        LOG_WARN("ev_sendto %s:%d, not a valid ip.", ip, port);
        return ERR_FAILED;
    }
    return ev_sendto_addr(ctx, sk, &addr, data, len, copy);
}
static inline void _cmd_sendto(watcher_ctx *watcher, sock_ctx *sk, sendto_ctx *buf) {
    evsock_ctx *evsk = _evpub_sockel_get(watcher, sk->fd);
    if (NULL == evsk
        || ERR_OK != _evpub_checkid(evsk, sk->skid)) {
        FREE(buf->data);
        return;
    }
    // ev_sendto 仅适用于 UDP；TCP fd 用 ev_send（CMD_PROPS 路径）。误用时丢弃数据
    if (SOCK_DGRAM != evsk->type) {
        LOG_ERROR("ev_sendto called on non-UDP fd %d, drop.", (int32_t)sk->fd);
        FREE(buf->data);
        return;
    }
    _evpub_add_bufs_sendto(watcher, evsk, buf, 0);
}
int32_t ev_sendto_addr(ev_ctx *ctx, sock_ctx *sk, netaddr_ctx *addr,
    void *data, size_t len, int32_t copy) {
    if (sock_is_invalid(sk) || NULL == addr || EMPTYPTR(data, len)) {
        CHECK_COPY_FREE(data, copy);
        return ERR_FAILED;
    }
    watcher_ctx *watcher = _cmd_get_watcher(ctx, sk);
    if (_evpub_inloop(watcher)) {
        sendto_ctx sbuf;
        sbuf.len = len;
        sbuf.addr = *addr;
        sbuf.data = _cmd_cpy_buf(data, len, copy);
        _cmd_sendto(watcher, sk, &sbuf);
        return ERR_OK;
    }
    cmd_ctx cmd = { 0 };
    cmd.cmd = CMD_SENDTO;
    cmd.sk = *sk;
    cmd.args.sendto.len = len;
    cmd.args.sendto.addr = *addr;
    cmd.args.sendto.data = _cmd_cpy_buf(data, len, copy);
    // 入队即成功：未被消费时 sendto.data 由 ev_free 的 drain(case CMD_SENDTO)释放
    _send_cmd(watcher, &cmd);
    return ERR_OK;
}
void _on_cmd_sendto(watcher_ctx *watcher, cmd_ctx *cmd) {
    _cmd_sendto(watcher, &cmd->sk, &cmd->args.sendto);
}
// 事件线程内执行 UDP 多播 setsockopt：先取 sock family,按 IPv4/IPv6 分支调对应 IP_*/IPV6_* 选项;
// Windows 路径下 IPv6 iface_str 忽略走默认接口(if_nametoindex 需 iphlpapi.lib,不引入依赖);
// 走 ev_props 通用命令,arg 由 ev_props 统一 UD_FREE,此处恒返回 1
static int32_t _udp_opt_cb(struct watcher_ctx *watcher, struct evsock_ctx *evsk,
    void *data, uint64_t number) {
    (void)watcher;
    (void)number;
    udp_opt_arg *arg = data;
    if (SOCK_DGRAM != evsk->type) {
        LOG_ERROR("ev_udp_* called on non-UDP fd %d, drop.", (int32_t)evsk->sk.fd);
        return 1;
    }
    int32_t family = sock_family(evsk->sk.fd);
    if (ERR_FAILED == family) {
        LOG_ERROR("sock_family(fd=%d) failed: %s", (int32_t)evsk->sk.fd, ERRORSTR(ERRNO));
        return 1;
    }
    if (AF_INET != family
        && AF_INET6 != family) {
        LOG_ERROR("ev_udp_* on fd %d: unsupported family %d.", (int32_t)evsk->sk.fd, family);
        return 1;
    }
    // 此后所有落到末尾汇总日志的失败都来自 setsockopt(errno 有效);
    // inet_pton 一类不设 errno 的失败各自就地报错并直接返回
    int32_t rtn = ERR_FAILED;
    switch (arg->op) {
    case UDP_OPT_JOIN:
    case UDP_OPT_LEAVE:
        // 用哪套选项由组地址的 family 定。同族的主判在 _ev_udp_group(调用方线程,能回传 ERR_FAILED),
        // 这里只是兜底:两处之间 fd 可能被关掉并复用成另一族
        if (ERR_OK != is_ipv6(arg->group_ip)) {
            struct ip_mreq mreq = { 0 };
            if (1 != inet_pton(AF_INET, arg->group_ip, &mreq.imr_multiaddr)) {
                LOG_ERROR("inet_pton(IPv4 %s) failed.", arg->group_ip);
                return 1;
            }
            if (AF_INET != family) {
                LOG_ERROR("group %s is IPv4 but fd %d bound as IPv6.", arg->group_ip, (int32_t)evsk->sk.fd);
                return 1;
            }
            if ('\0' != arg->iface_str[0]
                && 1 != inet_pton(AF_INET, arg->iface_str, &mreq.imr_interface)) {
                LOG_WARN("inet_pton(iface %s) failed,fallback INADDR_ANY", arg->iface_str);
                mreq.imr_interface.s_addr = htonl(INADDR_ANY);
            } else if ('\0' == arg->iface_str[0]) {
                mreq.imr_interface.s_addr = htonl(INADDR_ANY);
            }
            int32_t opt = (UDP_OPT_JOIN == arg->op) ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP;
            rtn = setsockopt(evsk->sk.fd, IPPROTO_IP, opt, (const char *)&mreq, sizeof(mreq));
        } else {
            if (AF_INET6 != family) {
                LOG_ERROR("group %s is IPv6 but fd %d bound as IPv4.", arg->group_ip, (int32_t)evsk->sk.fd);
                return 1;
            }
            struct ipv6_mreq mreq = { 0 };
            if (1 != inet_pton(AF_INET6, arg->group_ip, &mreq.ipv6mr_multiaddr)) {
                LOG_ERROR("inet_pton(IPv6 %s) failed.", arg->group_ip);
                return 1;
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
            rtn = setsockopt(evsk->sk.fd, IPPROTO_IPV6, opt, (const char *)&mreq, sizeof(mreq));
        }
        break;
    case UDP_OPT_TTL:
        if (AF_INET == family) {
            // IPv4 这两个选项的 optval 宽度两边不一样,只能各按各的来:Winsock 要 4 字节,
            // 给 1 字节直接 WSAEFAULT;BSD/macOS 的 ip(4) 约定是 u_char。IPv6 侧无此分歧,统一 4 字节
#ifdef EV_IOCP
            int32_t ttl = (int32_t)arg->ttl;
#else
            uint8_t ttl = arg->ttl;
#endif
            rtn = setsockopt(evsk->sk.fd, IPPROTO_IP, IP_MULTICAST_TTL, (const char *)&ttl, sizeof(ttl));
        } else if (AF_INET6 == family) {
            int32_t hops = (int32_t)arg->ttl;
            rtn = setsockopt(evsk->sk.fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, (const char *)&hops, sizeof(hops));
        }
        break;
    case UDP_OPT_LOOP:
        if (AF_INET == family) {
            // optval 宽度分歧同 UDP_OPT_TTL
#ifdef EV_IOCP
            int32_t loop = arg->loop ? 1 : 0;
#else
            uint8_t loop = (uint8_t)(arg->loop ? 1 : 0);
#endif
            rtn = setsockopt(evsk->sk.fd, IPPROTO_IP, IP_MULTICAST_LOOP, (const char *)&loop, sizeof(loop));
        } else if (AF_INET6 == family) {
            int32_t loop = arg->loop ? 1 : 0;
            rtn = setsockopt(evsk->sk.fd, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, (const char *)&loop, sizeof(loop));
        }
        break;
    }
    if (0 != rtn) {
        LOG_ERROR("UDP opt %d failed on fd %d: %s", (int32_t)arg->op, (int32_t)evsk->sk.fd, ERRORSTR(ERRNO));
    }
    return 1;
}
// UDP 多播 4 个公开 API 走同一 cmd 投递路径,差异只在 udp_opt_arg 字段填充
// JOIN 与 LEAVE 除了 op 完全一样，合到一处
static int32_t _ev_udp_group(ev_ctx *ctx, sock_ctx *sk, udp_opt_type op,
                             const char *group_ip, const char *iface_str) {
    if (sock_is_invalid(sk) || NULL == group_ip) {
        return ERR_FAILED;
    }
    // 组地址可解析、且与 socket 同族,两项都是调用方契约,放这儿判才能把失败真的回传;
    // 事件线程那侧同样的判定只作兜底——两处之间 fd 有可能被关掉并复用成另一族
    if (ERR_OK != is_ipaddr(group_ip)) {
        LOG_ERROR("udp group %s is not a valid ip.", group_ip);
        return ERR_FAILED;
    }
    int32_t family = sock_family(sk->fd);
    int32_t grpv6 = (ERR_OK == is_ipv6(group_ip));
    if ((0 != grpv6 && AF_INET6 != family)
        || (0 == grpv6 && AF_INET != family)) {
        LOG_ERROR("udp group %s family mismatch with fd %d.", group_ip, (int32_t)sk->fd);
        return ERR_FAILED;
    }
    udp_opt_arg *arg;
    CALLOC(arg, 1, sizeof(udp_opt_arg));
    arg->op = op;
    // 装不下不能截断后继续：截出来的地址要么 setsockopt 报个看不懂的错，要么加入了别的组
    if (ERR_OK != safe_fill_str(arg->group_ip, sizeof(arg->group_ip), group_ip)
        || ERR_OK != safe_fill_str(arg->iface_str, sizeof(arg->iface_str), iface_str)) {
        LOG_ERROR("udp group ip / iface too long: %s, %s.", group_ip, EMPTYSTR(iface_str) ? "" : iface_str);
        FREE(arg);
        return ERR_FAILED;
    }
    return ev_props(ctx, sk, _udp_opt_cb, _free, arg, 0);
}
int32_t ev_udp_join(ev_ctx *ctx, sock_ctx *sk, const char *group_ip, const char *iface_str) {
    return _ev_udp_group(ctx, sk, UDP_OPT_JOIN, group_ip, iface_str);
}
int32_t ev_udp_leave(ev_ctx *ctx, sock_ctx *sk, const char *group_ip, const char *iface_str) {
    return _ev_udp_group(ctx, sk, UDP_OPT_LEAVE, group_ip, iface_str);
}
int32_t ev_udp_ttl(ev_ctx *ctx, sock_ctx *sk, uint8_t ttl) {
    if (sock_is_invalid(sk)) {
        return ERR_FAILED;
    }
    udp_opt_arg *arg;
    CALLOC(arg, 1, sizeof(udp_opt_arg));
    arg->op = UDP_OPT_TTL;
    arg->ttl = ttl;
    return ev_props(ctx, sk, _udp_opt_cb, _free, arg, 0);
}
int32_t ev_udp_loop(ev_ctx *ctx, sock_ctx *sk, int32_t enable) {
    if (sock_is_invalid(sk)) {
        return ERR_FAILED;
    }
    udp_opt_arg *arg;
    CALLOC(arg, 1, sizeof(udp_opt_arg));
    arg->op = UDP_OPT_LOOP;
    arg->loop = enable ? 1 : 0;
    return ev_props(ctx, sk, _udp_opt_cb, _free, arg, 0);
}
DEF_UD_SETTER(pktype, subtype_t)
DEF_UD_SETTER(status, uint8_t)
DEF_UD_SETTER(sess, uint64_t)
DEF_UD_SETTER(handle, name_t)
static int32_t _cmd_ud_context(struct watcher_ctx *watcher, struct evsock_ctx *evsk,
    void *data, uint64_t number) {
    (void)watcher;
    (void)number;
    _evpub_get_ud(evsk)->context = data;
    return 0;
}
int32_t ev_ud_context(ev_ctx *ctx, sock_ctx *sk, void *extra, free_cb fcb) {
    return ev_props(ctx, sk, _cmd_ud_context, fcb, extra, 0);
}
void _cmd_drain_free(cmd_ctx *cmd) {
    evsock_ctx *evsk;
    void *data;
    switch ((ev_cmds)cmd->cmd) {
    case CMD_SENDTO:
        data = cmd->args.sendto.data;
        FREE(data);
        break;
    case CMD_CONN:
        evsk = cmd->args.conn.evsk;
        _evpub_sk_free(evsk);
        break;
    case CMD_ADD:
        evsk = cmd->args.evsk;
        if (SOCK_STREAM == evsk->type) {
            _evpub_sk_free(evsk);
        } else {
#ifdef EV_IOCP
            _iocp_free_udp(evsk);
#else
            _uev_free_udp(evsk);
#endif
        }
        break;
    case CMD_ADDACP:
        // 这条的 fd 是刚 accept 出来、还没注册进任何结构的连接，除命令自身外无人持有，必须在此关掉；
        // 减 ref 配对的是投递前那次 ++ 占位，归零即释放 lsn
        CLOSE_SOCK(cmd->sk.fd);
#ifdef EV_IOCP
        _iocp_try_freelsn(cmd->args.lsn);
#else
        _uev_try_freelsn(cmd->args.lsn);
#endif
        break;
    case CMD_PROPS:
        UD_FREE(cmd->args.props.fcb, cmd->args.props.data);
        break;
#ifndef EV_IOCP
    case CMD_UNLSN:
    case CMD_LSN_UNREF:
        _uev_try_freelsn(cmd->args.lsn);
        break;
    case CMD_LSN:
        // evsk 指向 lsn->lsnsock[i]，归 listener 所有；lsn 本身随后由 _uev_free_alllsn 释放
        break;
#endif
    case CMD_STOP:
    case CMD_TOTAL:
        break;
    }
}
