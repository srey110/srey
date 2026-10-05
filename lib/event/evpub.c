#include "containers/hashmap.h"
#include "utils/netutils.h"
#include "utils/timer.h"
#ifdef EV_IOCP
#include "event/iocp.h"
#else
#include "event/uev.h"
#endif

TLS_DEFINE(struct watcher_ctx *, _cur_watcher, 1)

void _evpub_set_cur_watcher(struct watcher_ctx *watcher) {
    *_cur_watcher_tls() = watcher;
}
int32_t _evpub_inloop(struct watcher_ctx *watcher) {
    return watcher == *_cur_watcher_tls();
}
evsock_ctx *_evpub_sockel_get(watcher_ctx *watcher, SOCKET fd) {
    evsock_ctx key;
    key.sk.fd = fd;
    evsock_ctx *pkey = &key;
    evsock_ctx **tmp = sockel_map_get(watcher->element, &pkey);
    return NULL == tmp ? NULL : *tmp;
}
void _evpub_sockel_add(watcher_ctx *watcher, evsock_ctx *evsk) {
    ASSERTAB(NULL == sockel_map_set(watcher->element, &evsk), "socket repeat.");
    ASSERTAB(!sockel_map_oom(watcher->element), "hashmap oom.");
}
void *_evpub_sockel_remove(watcher_ctx *watcher, SOCKET fd) {
    if (INVALID_SOCK == fd) {
        return NULL;
    }
    evsock_ctx key;
    key.sk.fd = fd;
    evsock_ctx *pkey = &key;
    return (void *)sockel_map_delete(watcher->element, &pkey);
}
void _evpub_tick_add(watcher_ctx *watcher, ev_tick *tk) {
    list_push_tail(&watcher->ticks, &tk->node);
}
void _evpub_tick_remove(watcher_ctx *watcher, ev_tick *tk) {
    list_remove(&watcher->ticks, &tk->node);
}
void _evpub_tick_attach(watcher_ctx *watcher, ev_tick *tk, ev_tick_cb cb, void *ud) {
    if (NULL != tk->cb) {
        return;
    }
    tk->cb = cb;
    tk->ud = ud;
    _evpub_tick_add(watcher, tk);
}
void _evpub_tick_detach(watcher_ctx *watcher, ev_tick *tk) {
    if (NULL == tk->cb) {
        return;
    }
    tk->cb = NULL;
    _evpub_tick_remove(watcher, tk);
}
uint32_t _evpub_tick_drive(watcher_ctx *watcher, timer_ctx *timer, uint64_t *now_ms) {
    *now_ms = 0;
    uint32_t next_to = EVENT_WAIT_TIMEOUT;
    if (!list_empty(&watcher->ticks)) {
        ev_tick *tk;
        uint32_t d;
        *now_ms = timer_cur_ms(timer);
        list_foreach_safe(&watcher->ticks, it, tmp) {
            tk = UPCAST(it, ev_tick, node);
            d = tk->cb(tk->ud, *now_ms);
            if (d < next_to) {
                next_to = d;
            }
        }
        if (next_to < EVENT_TICK_MIN) {
            next_to = EVENT_TICK_MIN;
        }
    }
    return next_to;
}
int32_t _evpub_sock_type(evsock_ctx *evsk) {
    return evsk->type;
}
// 定期收缩对象池（调用方按 EVENT_CHECK_INTERVAL 节流触发，避免频繁 syscall）。
// sockel_map_size 作收缩基数：IOCP 下 cmd sock 不入 hashmap（精确），Unix 下含 1 个 cmd 管道 sock（偏差可忽略）。
void _evpub_pool_shrink(watcher_ctx *watcher, uint64_t *shrink_start, uint64_t now_ms) {
    if (!pool_shrink_due(shrink_start, now_ms)) {
        return;
    }
    pool_shrink_to(&watcher->pool, shrink_nkeep(sockel_map_size(watcher->element)));
}
void _evpub_share_data_free(void *arg) {
    shared_data_free(arg, _free);
}
void _evpub_off_buf_release(off_buf_ctx *buf) {
    if (NULL == buf->shared) {
        FREE(buf->data);
        return;
    }
    // 多播路径：N 个 buf 共享 shared->data，最后一个释放方才 FREE
    _evpub_share_data_free(buf->shared);
    buf->data = NULL;
    buf->shared = NULL;
}
void _evpub_off_buf_clear(obuf_que *bufs) {
    off_buf_ctx *buf;
    while (NULL != (buf = obuf_que_pop(bufs))) {
        _evpub_off_buf_release(buf);
    }
    obuf_que_clear(bufs);
}
void _evpub_sendto_clear(sbuf_que *bufs) {
    sendto_ctx *buf;
    while (NULL != (buf = sbuf_que_pop(bufs))) {
        FREE(buf->data);
    }
    sbuf_que_clear(bufs);
}
// 队列超上限判定,TCP / UDP 两条文案各占一支——LOG 宏会拼接 fmt,fmt 必须是字面量。
// 收元素数而非队列指针:TCP 与 UDP 的发送队列宏化后是两个类型,这里只需要个数
static inline int32_t _evpub_sendqu_full(uint32_t nqu, SOCKET fd, int32_t istcp) {
    if (0 != MAX_SENDQ_CNT
        && nqu >= MAX_SENDQ_CNT) {
        if (0 != istcp) {
            LOG_WARN("TCP send queue overflow on fd %d (>= %d), disconnect.", (int32_t)fd, MAX_SENDQ_CNT);
        } else {
            LOG_WARN("UDP send queue overflow on fd %d (>= %d), drop datagram.", (int32_t)fd, MAX_SENDQ_CNT);
        }
        return 1;
    }
    return 0;
}
// 接收缓冲堆积判定:调用点须放在 recv 回调之后——协议层在那里把成形的包 drain 走,
// 剩下的才是攒着凑不成包的数据。放回调之前会把一次收到的合法大包当成堆积
int32_t _evpub_recvbuf_full(buffer_ctx *buf_r, SOCKET fd) {
    if (0 == MAX_RECV_CASH) {
        return 0;
    }
    size_t cached = buffer_size(buf_r);
    if (cached <= (size_t)MAX_RECV_CASH) {
        return 0;
    }
    LOG_WARN("TCP recv buf overflow on fd %d: %zu bytes (> %d), disconnect.",
             (int32_t)fd, cached, (int32_t)MAX_RECV_CASH);
    return 1;
}
int32_t _evpub_sendqu_check_tcp(uint32_t nqu, int32_t status, SOCKET fd) {
    // 连接未完成时写事件表示等待 connect 而非待发数据,入队会被 connect 回调连同写事件一起删掉;
    // IOCP 侧则是 ConnectEx 未完成就 WSASend,必以 WSAENOTCONN 失败
    if (!BIT_CHECK(status, STATUS_ESTABLISHED)) {
        LOG_WARN("ev_send before connection established on fd %d, disconnect.", (int32_t)fd);
        return 0;
    }
#if WITH_SSL
    // 握手期发业务数据会打断握手
    if (BIT_CHECK(status, STATUS_AUTHSSL)
        || BIT_CHECK(status, STATUS_SSLEXCHANGE)) {
        LOG_WARN("ev_send during SSL handshake on fd %d, disconnect.", (int32_t)fd);
        return 0;
    }
#endif
    // 慢消费者保护:业务无脑写会打爆内存
    return 0 == _evpub_sendqu_full(nqu, fd, 1);
}
int32_t _evpub_sendqu_check_udp(uint32_t nqu, SOCKET fd) {
    return 0 == _evpub_sendqu_full(nqu, fd, 0);
}
void _evpub_sendqu_tda(tda_ctx *tda, size_t wb_size, SOCKET fd, int32_t istcp) {
    if (!tda_check(tda, wb_size)) {
        return;
    }
    if (0 != istcp) {
        LOG_WARN("TCP send buf growing on fd %d: %zu bytes.", (int32_t)fd, wb_size);
    } else {
        LOG_WARN("UDP send buf growing on fd %d: %zu bytes.", (int32_t)fd, wb_size);
    }
}
void _evpub_close_flush_tcp(SOCKET fd, obuf_que *buf_s, int32_t status, size_t *wb_size, void *ssl) {
    if (obuf_que_empty(buf_s)) {
        return;
    }
#if WITH_SSL
    // SSLEXCHANGE 期 ssl 恒为 NULL、队列里全是明文,对端也还在读明文,照常冲;
    // 只有 KEYUPDATE_WRITE 挂着一个待重试的 SSL_read,那时不能再调 SSL_write
    int32_t canflush = !BIT_CHECK(status, STATUS_KEYUPDATE_WRITE);
#else
    (void)status;
    int32_t canflush = 1;
#endif
    if (0 != canflush) {
        size_t nsend = 0;
        (void)_evpub_sock_send(fd, buf_s, &nsend, ssl);
        *wb_size -= nsend;
    }
    if (!obuf_que_empty(buf_s)) {
        LOG_WARN("close fd %d with %zu bytes undelivered.", (int32_t)fd, *wb_size);
    }
}
void _evpub_disconnect(watcher_ctx *watcher, evsock_ctx *evsk) {
#ifdef EV_IOCP
    (void)watcher;
    _iocp_disconnect(evsk);
#else
    _uev_disconnect(watcher, evsk);
#endif
}
void _evpub_mark_close(int32_t *status, int32_t rtn) {
    switch (rtn) {
    case 1:
        BIT_SET(*status, STATUS_PEER_FIN);
        break;
#if WITH_SSL
    case 2:
        BIT_SET(*status, STATUS_PEER_TRUNCATED);
        break;
#endif
    default:
        BIT_SET(*status, STATUS_PEER_ABORT);
        break;
    }
}
int32_t _evpub_close_type(int32_t status) {
    // FIN 与 TRUNCATED 先判：异常路径可能在标过它们之后再叠一次 ABORT，这样置位处就不用互斥
    if (BIT_CHECK(status, STATUS_PEER_FIN)) {
        return CLOSE_TYPE_ORDERLY;
    }
#if WITH_SSL
    if (BIT_CHECK(status, STATUS_PEER_TRUNCATED)) {
        return CLOSE_TYPE_TRUNCATED;
    }
#endif
    if (BIT_CHECK(status, STATUS_PEER_ABORT)) {
        return CLOSE_TYPE_ABORT;
    }
    return CLOSE_TYPE_LOCAL;
}
int32_t _evpub_linger_want(int32_t status) {
    return 0 != CLOSE_LINGER_MS
        && BIT_CHECK(status, STATUS_ESTABLISHED)
        && CLOSE_TYPE_LOCAL == _evpub_close_type(status);
}
int32_t _evpub_linger_drain(SOCKET fd, char *buf, size_t lens, size_t *bytes) {
    IOV_TYPE iov;
    size_t nread;
    iov.IOV_PTR_FIELD = buf;
    iov.IOV_LEN_FIELD = (IOV_LEN_TYPE)lens;
    for (;;) {
        if (ERR_OK != _evpub_sock_read(fd, &iov, 1, NULL, &nread)) {
            return 1;
        }
        if (0 == nread) {
            return 0;
        }
        *bytes += nread;
        if (*bytes > (size_t)CLOSE_LINGER_BYTES) {
            return 1;
        }
    }
}
#if WITH_SSL
int32_t _evpub_ssl_exchange_check(const void *ssl, int32_t *status, int32_t client) {
    if (NULL != ssl) {
        LOG_WARN("ssl already in use.");
        return 0;
    }
    if (BIT_CHECK(*status, STATUS_SSLEXCHANGE)) {
        LOG_WARN("repeat request ssl exchange.");
        return 0;
    }
    if (BIT_CHECK(*status, STATUS_ERROR)) {
        return 0;
    }
    // 连接未完成时 EVENT_WRITE(IOCP 为 STATUS_SENDING)表示等待 connect 而非待发数据,
    // 误入调用方的延迟分支会残留 SSLEXCHANGE 脏位;正确用法是 ev_connect 带 evssl,
    // 或等连接建立后再 ev_ssl
    if (!BIT_CHECK(*status, STATUS_ESTABLISHED)) {
        LOG_WARN("ssl exchange requested before connection established.");
        return 0;
    }
    if (client) {
        BIT_SET(*status, STATUS_CLIENT);
    } else {
        BIT_REMOVE(*status, STATUS_CLIENT);
    }
    return 1;
}
#endif
int32_t _evpub_tcp_keepalive(SOCKET fd) {
    return sock_keepalive(fd, KEEPALIVE_TIME, KEEPALIVE_INTERVAL);
}
int32_t _evpub_accept_opts(SOCKET fd) {
#if 1 == ACCEPT_INHERIT_OPTS
    (void)fd;
    return ERR_OK;
#elif 2 == ACCEPT_INHERIT_OPTS
    int32_t idle = KEEPALIVE_TIME;
    if (idle > 0
        && setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, (char *)&idle, (int32_t)sizeof(idle)) < ERR_OK) {
        return ERR_FAILED;
    }
    return ERR_OK;
#else
    if (ERR_OK != sock_nodelay(fd)) {
        return ERR_FAILED;
    }
    return _evpub_tcp_keepalive(fd);
#endif
}
int32_t _evpub_sock_launch_check(ev_ctx *ctx, const char *ip, uint16_t port, cbs_ctx *cbs,
                                 ud_cxt *ud, int32_t isudp, netaddr_ctx *addr) {
    if (NULL == cbs
        || (0 != isudp ? NULL == cbs->rf_cb : NULL == cbs->r_cb)) {
        if (NULL != cbs) {
            UD_FREE(cbs->ud_free, ud);
        }
        return ERR_FAILED;
    }
    // ev_free 已开始：命令仍能入队但 watcher 不再消费，句柄永不生效，故直接拒绝而非谎报成功
    if (0 != ATOMIC_GET(&ctx->stopping)) {
        UD_FREE(cbs->ud_free, ud);
        return ERR_FAILED;
    }
    if (ERR_OK != netaddr_set(addr, ip, port)) {
        LOG_ERROR("netaddr_set %s:%d, not a valid ip.", ip, port);
        UD_FREE(cbs->ud_free, ud);
        return ERR_FAILED;
    }
    return ERR_OK;
}
SOCKET _evpub_listen(netaddr_ctx *addr) {
    SOCKET fd = sock_create_cloexec(netaddr_family(addr), SOCK_STREAM, 0, 1);
    if (INVALID_SOCK == fd) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return INVALID_SOCK;
    }
    if (ERR_OK != sock_reuseaddr(fd, 1)
        || ERR_OK != sock_reuseport(fd)
        || ERR_OK != sock_v6only(fd, netaddr_family(addr))) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        CLOSE_SOCK(fd);
        return INVALID_SOCK;
    }
    if (ERR_OK != bind(fd, netaddr_addr(addr), netaddr_size(addr))) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        CLOSE_SOCK(fd);
        return INVALID_SOCK;
    }
#if ACCEPT_INHERIT_OPTS
    if (ERR_OK != sock_nodelay(fd)
        || ERR_OK != _evpub_tcp_keepalive(fd)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        CLOSE_SOCK(fd);
        return INVALID_SOCK;
    }
#endif
    if (ERR_OK != listen(fd, SOMAXCONN)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        CLOSE_SOCK(fd);
        return INVALID_SOCK;
    }
    return fd;
}
SOCKET _evpub_udp(netaddr_ctx *addr) {
    SOCKET fd = sock_create_cloexec(netaddr_family(addr), SOCK_DGRAM, 0, 1);
    if (INVALID_SOCK == fd) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return INVALID_SOCK;
    }
#ifdef EV_IOCP
    DWORD bytes = 0;
    BOOL behavior = FALSE;
    if (WSAIoctl(fd,
                 SIO_UDP_CONNRESET,
                 &behavior,
                 sizeof(behavior),
                 NULL,
                 0,
                 &bytes,
                 NULL,
                 NULL) < ERR_OK) {
        LOG_ERROR("WSAIoctl(%d, SIO_UDP_CONNRESET...) failed. %s", (int32_t)fd, ERRORSTR(ERRNO));
        CLOSE_SOCK(fd);
        return INVALID_SOCK;
    }
#endif
    if (ERR_OK != sock_reuseaddr(fd, 0)
        || ERR_OK != sock_v6only(fd, netaddr_family(addr))) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        CLOSE_SOCK(fd);
        return INVALID_SOCK;
    }
    if (ERR_OK != bind(fd, netaddr_addr(addr), netaddr_size(addr))) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        CLOSE_SOCK(fd);
        return INVALID_SOCK;
    }
    return fd;
}
#if WITH_SSL
// SSL不支持scatter I/O，每次只能读一个TLS记录，外层循环驱动重复调用直到socket耗尽
static inline int32_t _evpub_sock_read_ssl(SSL *ssl, IOV_TYPE *iov, size_t *nread) {
    int32_t rtn = evssl_read(ssl, iov[0].IOV_PTR_FIELD, (size_t)iov[0].IOV_LEN_FIELD, nread);
#if defined(TRIGGER_LT)
    // 水平触发下没必要读到 WANT_READ：OpenSSL 那层已空、这次又没填满给出的空间，
    // 剩下的（若有）还在内核里，下次可读事件会再报。省掉那轮必然 EAGAIN 的 recv。
    // 必须判 *nread > 0：WANT_READ/WANT_WRITE 时 evssl_read 也返 ERR_OK，只是没读到东西，
    // 那种情形交给 buffer_from_sock 的零长早退，别在这里冒充"抽干"
    if (ERR_OK == rtn
        && *nread > 0
        && *nread < (size_t)iov[0].IOV_LEN_FIELD
        && 0 == SSL_has_pending(ssl)) {
        return BUFFER_READV_DRAINED;
    }
#endif
    return rtn;
}
#endif
// 从socket普通读取（使用readv/WSARecv）
// 只有一条 iov 时改走单缓冲的 recv:省掉 iovec 数组那次用户态->内核态的拷贝与遍历。
// buffer_expand 在稳态几乎只给一条,但多条那支必须原样留着——
// 硬约束是 scatter 能力,不是它常不常用
static inline int32_t _evpub_sock_read_normal(SOCKET fd, IOV_TYPE *iov, uint32_t niov, size_t *readed) {
#ifdef EV_IOCP
    DWORD bytes, flags = 0;
    if (1 == niov) {
        int32_t one = recv(fd, iov[0].IOV_PTR_FIELD, (int32_t)iov[0].IOV_LEN_FIELD, 0);
        if (one > 0) {
            *readed = (size_t)one;
            return ERR_OK;
        }
        if (0 == one) {
            return 1;
        }
        if (!IS_EAGAIN(ERRNO)) {
            return ERR_FAILED;
        }
        return ERR_OK;
    }
    if (SOCKET_ERROR != WSARecv(fd,
                                iov,
                                niov,
                                &bytes,
                                &flags,
                                NULL,
                                NULL)) {
        if (bytes > 0) {
            *readed = bytes;
            return ERR_OK;
        }
        return 1;// 完成但 0 字节即对端 FIN；RST 走 WSARecv 失败那支
    }
    if (!IS_EAGAIN(ERRNO)) {
        return ERR_FAILED;
    }
    return ERR_OK;
#else
    ssize_t rtn = (1 == niov)
        ? recv(fd, iov[0].IOV_PTR_FIELD, iov[0].IOV_LEN_FIELD, 0)
        : readv(fd, iov, niov);
    if (rtn > 0) {
        *readed = (size_t)rtn;
        return ERR_OK;
    }
    if (0 == rtn) {
        return 1;// 对端 FIN；RST / 其他读错误走下面那支
    }
    if (!ERR_RW_RETRIABLE(ERRNO)) {
        return ERR_FAILED;
    }
    return ERR_OK;
#endif
}
int32_t _evpub_sock_read(SOCKET fd, IOV_TYPE *iov, uint32_t niov, void *arg, size_t *readed) {
    *readed = 0;
#if WITH_SSL
    if (NULL == arg) {
        return _evpub_sock_read_normal(fd, iov, niov, readed);
    }
    // 只取 iov[0]、丢掉 niov：SSL 一次只能读一个 TLS 记录到单个缓冲，理由见 _evpub_sock_read_ssl
    return _evpub_sock_read_ssl((SSL *)arg, iov, readed);
#else
    (void)arg;
    return _evpub_sock_read_normal(fd, iov, niov, readed);
#endif
}
// 将发送队列中的前N个缓冲区填充到iov数组，返回实际填充数量（最多 MAX_SEND_NIOV 条，
// 总大小不超过 MAX_SEND_SIZE;后者为 0 时不限,条数就是唯一约束）。
// total 出参回传这批 iov 的字节总和，供调用方判断是否短写
static uint32_t _evpub_off_buf_fill_iov(obuf_que *buf_s, size_t nbuf,
                                        IOV_TYPE iov[MAX_SEND_NIOV],
                                        off_buf_ctx *sndbuf[MAX_SEND_NIOV],
                                        size_t *total) {
    if (nbuf > MAX_SEND_NIOV) {
        nbuf = MAX_SEND_NIOV;
    }
    uint32_t cnt = 0;
    off_buf_ctx *buf;
    size_t remain, sum = 0;
    for (uint32_t i = 0; i < (uint32_t)nbuf; i++) {
        buf = obuf_que_at(buf_s, i);
        remain = buf->lens - buf->offset;
#if defined(OS_WIN)
        // WSABUF.len 是 ULONG(32位):单块 >=4GB 且 4GB 整数倍会截断为 0 → WSASend 发 0 字节、offset 不进的忙循环;限到 ULONG 上界
        if (remain > 0xffffffffu) {
            remain = 0xffffffffu;
        }
#endif
        iov[i].IOV_PTR_FIELD = ((char *)buf->data) + buf->offset;
        iov[i].IOV_LEN_FIELD = (IOV_LEN_TYPE)remain;
        sndbuf[i] = buf;
        cnt++;
        sum += remain;
#if 0 != MAX_SEND_SIZE
        if (sum >= MAX_SEND_SIZE) {
            break;
        }
#endif
    }
    *total = sum;
    return cnt;
}
// 根据实际发送字节数sent，从发送队列头部消费已完成的缓冲区，更新offset或弹出并释放
static void _evpub_off_buf_apply_sent(obuf_que *buf_s, off_buf_ctx *sndbuf[MAX_SEND_NIOV],
                                      uint32_t niov, size_t sent) {
    off_buf_ctx *buf;
    size_t buflen;
    for (uint32_t i = 0; i < niov && sent > 0; i++) {
        buf = sndbuf[i];
        buflen = buf->lens - buf->offset;
        if (sent >= buflen) {
            sent -= buflen;
            _evpub_off_buf_release(buf);
            obuf_que_pop(buf_s);
        } else {
            buf->offset += sent;
            sent = 0;
        }
    }
}
// 调用writev/WSASend发送iov数组中的数据，返回ERR_OK并更新sended
static inline int32_t _evpub_sock_send_iov(SOCKET fd, IOV_TYPE *iov, uint32_t niov, size_t *sended) {
    *sended = 0;
#ifdef EV_IOCP
    DWORD bytes;
    if (SOCKET_ERROR != WSASend(fd,
                                iov,
                                niov,
                                &bytes,
                                0,
                                NULL,
                                NULL)) {
        *sended = bytes;
        return ERR_OK;
    }
    if (!IS_EAGAIN(ERRNO)) {
        return ERR_FAILED;
    }
    return ERR_OK;
#else
    // 只有一条 iov 时改走单缓冲 send,省掉 iovec 数组的用户态->内核态拷贝与遍历。
    // 非背压路径队列深度恒为 1,这条几乎总命中。IOCP 侧不这么做:那边 _evpub_off_buf_fill_iov
    // 把长度钳到 ULONG 上界,而 send 的形参是 int,转过去会变负数
    ssize_t rtn = (1 == niov)
        ? send(fd, iov[0].IOV_PTR_FIELD, iov[0].IOV_LEN_FIELD, 0)
        : writev(fd, iov, (int)niov);
    if (rtn < 0) {
        if (!ERR_RW_RETRIABLE(ERRNO)) {
            return ERR_FAILED;
        }
        return ERR_OK;
    }
    *sended = (size_t)rtn;
    return ERR_OK;
#endif
}
// 循环发送队列中所有普通（非SSL）数据，直到队列空或发生错误
static int32_t _evpub_sock_send_normal(SOCKET fd, obuf_que *buf_s, size_t *nsend) {
    int32_t rtn = ERR_OK;
    size_t nbuf, sended, total;
    uint32_t niov;
    IOV_TYPE iov[MAX_SEND_NIOV];
    off_buf_ctx *sndbuf[MAX_SEND_NIOV];
    while (0 != (nbuf = obuf_que_size(buf_s))) {
        niov = _evpub_off_buf_fill_iov(buf_s, nbuf, iov, sndbuf, &total);
        rtn = _evpub_sock_send_iov(fd, iov, niov, &sended);
        if (ERR_OK != rtn) {
            break;
        }
        *nsend += sended;
        _evpub_off_buf_apply_sent(buf_s, sndbuf, niov, sended);
        // 没把这批 iov 全发出去即内核缓冲已满,下一轮必然 EAGAIN,省掉那次确认性系统调用。
        // 判据用 total 不用 MAX_SEND_SIZE:后者为 0 时不限,且 iov 还受 MAX_SEND_NIOV 条数约束。
        // 0 == sended 要单列在前——total 恒大于 0 时它才等价,那条不变式由上游入口的空载荷校验撑着
        if (0 == sended
            || sended < total) {
            break;
        }
    }
    return rtn;
}
#if WITH_SSL
// 把队头起的前几块拷成一段(不超过 MAX_SSL_SEND_SIZE)一次 SSL_write，发出去多少就从队头消费多少。
// 撞 WANT_WRITE 时一个字节都不记账：下次拼出的前缀不变、长度只增不减，OpenSSL 才认这次重试；
// 缓冲在栈上，重试时地址会变，靠 SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER 放行
static int32_t _evpub_sock_send_ssl_merge(SSL *ssl, obuf_que *buf_s, uint32_t nbuf, size_t *sended) {
    char stage[MAX_SSL_SEND_SIZE];
    off_buf_ctx *buf;
    size_t staged = 0, take, sent;
    for (uint32_t i = 0; i < nbuf && staged < sizeof(stage); i++) {
        buf = obuf_que_at(buf_s, i);
        take = buf->lens - buf->offset;
        if (take > sizeof(stage) - staged) {
            take = sizeof(stage) - staged;
        }
        memcpy(stage + staged, (char *)buf->data + buf->offset, take);
        staged += take;
    }
    int32_t rtn = evssl_send(ssl, stage, staged, sended);
    if (ERR_OK != rtn) {
        return rtn;
    }
    sent = *sended;
    while (sent > 0) {
        buf = obuf_que_peek(buf_s);
        take = buf->lens - buf->offset;
        if (sent < take) {
            buf->offset += sent;
            break;
        }
        sent -= take;
        obuf_que_pop(buf_s);
        _evpub_off_buf_release(buf);
    }
    return rtn;
}
// 通过 SSL 发送队列中的数据。单次上限与抽干循环两条都不能去掉：超过 MAX_SSL_SEND_SIZE
// 会让 TLS1.3 KeyUpdate 断连，不抽干则边缘触发下发送就此停住。
// 队头不足 MAX_SSL_SEND_SIZE 且后面还有块时合并成一次写，其余逐块直发
static int32_t _evpub_sock_send_ssl(SSL *ssl, obuf_que *buf_s, size_t *nsend) {
    int32_t rtn = ERR_OK;
    size_t sended, lens;
    uint32_t nbuf;
    off_buf_ctx *buf;
    for (;;) {
        buf = obuf_que_peek(buf_s);
        if (NULL == buf) {
            break;
        }
        lens = buf->lens - buf->offset;
        nbuf = obuf_que_size(buf_s);
        if (nbuf > 1
            && lens < MAX_SSL_SEND_SIZE) {
            rtn = _evpub_sock_send_ssl_merge(ssl, buf_s, nbuf, &sended);
            if (ERR_OK != rtn) {
                break;
            }
            (*nsend) += sended;
            if (0 == sended) {
                break;
            }
            continue;
        }
        if (lens > MAX_SSL_SEND_SIZE) {
            lens = MAX_SSL_SEND_SIZE;
        }
        rtn = evssl_send(ssl, (char *)buf->data + buf->offset, lens, &sended);
        if (ERR_OK != rtn) {
            break;
        }
        (*nsend) += sended;
        buf->offset += sended;
        if (0 == sended) {
            break;
        }
        if (buf->offset == buf->lens) {
            obuf_que_pop(buf_s);
            _evpub_off_buf_release(buf);
        }
    }
    return rtn;
}
#endif
int32_t _evpub_sock_send(SOCKET fd, obuf_que *buf_s, size_t *nsend, void *arg) {
    *nsend = 0;
#if WITH_SSL
    if (NULL == arg) {
        return _evpub_sock_send_normal(fd, buf_s, nsend);
    }
    return _evpub_sock_send_ssl((SSL *)arg, buf_s, nsend);
#else
    (void)arg;
    return _evpub_sock_send_normal(fd, buf_s, nsend);
#endif
}
void _evpub_add_bufs_sendto(watcher_ctx *watcher, evsock_ctx *evsk, sendto_ctx *buf, int32_t tried) {
#ifdef EV_IOCP
    // IOCP 侧 _iocp_add_bufs_trysendto 本身已是独立的 try-then-queue 实现，不需要外部传入的尝试状态；
    // 代价是 tried=1 进来时 _olp_sendto_drain 会再同步发一次(必然复现同一失败)，那是慢路径,不值得为它加形参。
    // watcher 要用：发送卡在资源紧张上时靠它挂重试 tick
    (void)tried;
    _iocp_add_bufs_trysendto(watcher, evsk, buf);
#else
    _uev_add_bufs_sendto(watcher, evsk, buf, tried);
#endif
}
int32_t _evpub_try_sendto(watcher_ctx *watcher, evsock_ctx *evsk, const void *data, size_t len, netaddr_ctx *addr) {
#ifdef EV_IOCP
    // 本平台不需要 watcher:同步失败不断连,由调用方排队
    (void)watcher;
    return _iocp_try_sendto(evsk, data, len, addr);
#else
    return _uev_try_sendto(watcher, evsk, data, len, addr);
#endif
}
