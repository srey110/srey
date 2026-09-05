#include "containers/hashmap.h"
#include "utils/netutils.h"
#include "utils/timer.h"
#ifdef EV_IOCP
#include "event/iocp.h"
#else
#include "event/uev.h"
#endif

// hashmap 哈希函数：以 fd 为 key
uint64_t _evpub_sockel_hash(const void *item, uint64_t seed0, uint64_t seed1) {
    (void)seed0;
    (void)seed1;
    return hash_u64((uint64_t)(*(const sock_ctx **)item)->fd);
}
// hashmap比较函数：比较两个sock_ctx的fd
int _evpub_sockel_compare(const void *a, const void *b, void *ud) {
    (void)ud;
    SOCKET fa = (*(const sock_ctx **)a)->fd;
    SOCKET fb = (*(const sock_ctx **)b)->fd;
    return (fa < fb) ? -1 : (fa > fb) ? 1 : 0; // 三路比较，避免 UINT_PTR 相减截断为 int 溢出
}
sock_ctx *_evpub_sockel_get(watcher_ctx *watcher, SOCKET fd) {
    sock_ctx key;
    key.fd = fd;
    sock_ctx *pkey = &key;
    void **tmp = (void **)hashmap_get(watcher->element, &pkey);
    return NULL == tmp ? NULL : *tmp;
}
void _evpub_sockel_add(watcher_ctx *watcher, sock_ctx *skctx) {
    ASSERTAB(NULL == hashmap_set(watcher->element, &skctx), "socket repeat.");
    ASSERTAB(!hashmap_oom(watcher->element), "hashmap oom.");
}
void *_evpub_sockel_remove(watcher_ctx *watcher, SOCKET fd) {
    if (INVALID_SOCK == fd) {
        return NULL;
    }
    sock_ctx key;
    key.fd = fd;
    sock_ctx *pkey = &key;
    return (void *)hashmap_delete(watcher->element, &pkey);
}
void _evpub_tick_add(watcher_ctx *watcher, ev_tick *tk) {
    list_push_tail(&watcher->ticks, &tk->node);
}
void _evpub_tick_remove(watcher_ctx *watcher, ev_tick *tk) {
    list_remove(&watcher->ticks, &tk->node);
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
int32_t _evpub_sock_type(sock_ctx *skctx) {
    return skctx->type;
}
// 定期收缩对象池（调用方按 EVENT_CHECK_INTERVAL 节流触发，避免频繁 syscall）。
// hashmap_count 作收缩基数：IOCP 下 cmd sock 不入 hashmap（精确），Unix 下含 1 个 cmd 管道 sock（偏差可忽略）。
void _evpub_pool_shrink(watcher_ctx *watcher, uint64_t *shrink_start, uint64_t now_ms) {
    if (!pool_shrink_due(shrink_start, now_ms)) {
        return;
    }
    pool_shrink_to(&watcher->pool, shrink_nkeep(hashmap_count(watcher->element)));
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
void _evpub_off_buf_clear(queue_ctx *bufs) {
    off_buf_ctx *buf;
    while (NULL != (buf = queue_pop(bufs))) {
        _evpub_off_buf_release(buf);
    }
    queue_clear(bufs);
}
void _evpub_sendto_clear(queue_ctx *bufs) {
    sendto_ctx *buf;
    while (NULL != (buf = queue_pop(bufs))) {
        FREE(buf->data);
    }
    queue_clear(bufs);
}
// 队列超上限判定,TCP / UDP 两条文案各占一支——LOG 宏会拼接 fmt,fmt 必须是字面量
static int32_t _evpub_sendqu_full(queue_ctx *buf_s, SOCKET fd, int32_t istcp) {
    if (0 != MAX_SENDQ_CNT
        && queue_size(buf_s) >= MAX_SENDQ_CNT) {
        if (0 != istcp) {
            LOG_WARN("TCP send queue overflow on fd %d (>= %d), disconnect.", (int32_t)fd, MAX_SENDQ_CNT);
        } else {
            LOG_WARN("UDP send queue overflow on fd %d (>= %d), drop datagram.", (int32_t)fd, MAX_SENDQ_CNT);
        }
        return 1;
    }
    return 0;
}
int32_t _evpub_sendqu_check_tcp(queue_ctx *buf_s, int32_t status, SOCKET fd) {
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
    return 0 == _evpub_sendqu_full(buf_s, fd, 1);
}
int32_t _evpub_sendqu_check_udp(queue_ctx *buf_s, SOCKET fd) {
    return 0 == _evpub_sendqu_full(buf_s, fd, 0);
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
void _evpub_close_flush_tcp(SOCKET fd, queue_ctx *buf_s, int32_t status, size_t *wb_size, void *ssl) {
    if (0 == queue_size(buf_s)) {
        return;
    }
    // SSLEXCHANGE 期 ssl 恒为 NULL、队列里全是明文,对端也还在读明文,照常冲;
    // 只有 KEYUPDATE_WRITE 挂着一个待重试的 SSL_read,那时不能再调 SSL_write
    if (!BIT_CHECK(status, STATUS_KEYUPDATE_WRITE)) {
        size_t nsend = 0;
        (void)_evpub_sock_send(fd, buf_s, &nsend, ssl);
        *wb_size -= nsend;
    }
    if (queue_size(buf_s) > 0) {
        LOG_WARN("close fd %d with %zu bytes undelivered.", (int32_t)fd, *wb_size);
    }
}
void _evpub_disconnect(watcher_ctx *watcher, sock_ctx *skctx) {
#ifdef EV_IOCP
    (void)watcher;
    _iocp_disconnect(skctx);
#else
    _uev_disconnect(watcher, skctx);
#endif
}
void _evpub_mark_close(int32_t *status, int32_t rtn, void *ssl) {
    int32_t fin;
#if WITH_SSL
    fin = (NULL != ssl) ? evssl_recvd_shutdown((SSL *)ssl) : (1 == rtn);
#else
    (void)ssl;
    fin = (1 == rtn);
#endif
    BIT_SET(*status, fin ? STATUS_PEER_FIN : STATUS_PEER_ABORT);
}
int32_t _evpub_close_type(int32_t status) {
    // FIN 先判：异常路径可能在标过 FIN 之后再叠一次 ABORT，这样置位处就不用互斥
    if (BIT_CHECK(status, STATUS_PEER_FIN)) {
        return CLOSE_TYPE_ORDERLY;
    }
    if (BIT_CHECK(status, STATUS_PEER_ABORT)) {
        return CLOSE_TYPE_ABORT;
    }
    return CLOSE_TYPE_LOCAL;
}
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
int32_t _evpub_tcp_sockopts(SOCKET fd) {
    if (ERR_OK != sock_nodelay(fd)
        || ERR_OK != sock_nonblock(fd)) {
        return ERR_FAILED;
    }
    return ERR_OK;
}
int32_t _evpub_tcp_keepalive(SOCKET fd) {
    return sock_keepalive(fd, KEEPALIVE_TIME, KEEPALIVE_INTERVAL);
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
    SOCKET fd = sock_create_cloexec(netaddr_family(addr), SOCK_STREAM, 0);
    if (INVALID_SOCK == fd) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return INVALID_SOCK;
    }
    if (ERR_OK != sock_reuseaddr(fd, 1)
        || ERR_OK != sock_reuseport(fd)
        || ERR_OK != sock_nonblock(fd)
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
    if (ERR_OK != listen(fd, SOMAXCONN)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        CLOSE_SOCK(fd);
        return INVALID_SOCK;
    }
    return fd;
}
SOCKET _evpub_udp(netaddr_ctx *addr) {
    SOCKET fd = sock_create_cloexec(netaddr_family(addr), SOCK_DGRAM, 0);
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
        || ERR_OK != sock_nonblock(fd)
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
    return evssl_read(ssl, iov[0].IOV_PTR_FIELD, (size_t)iov[0].IOV_LEN_FIELD, nread);
}
#endif
// 从socket普通读取（使用readv/WSARecv）
static inline int32_t _evpub_sock_read_normal(SOCKET fd, IOV_TYPE *iov, uint32_t niov, size_t *readed) {
#ifdef EV_IOCP
    DWORD bytes, flags = 0;
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
    ssize_t rtn = readv(fd, iov, niov);
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
// 将发送队列中的前N个缓冲区填充到iov数组，返回实际填充数量（最多MAX_SEND_NIOV，总大小不超过MAX_SEND_SIZE）
static uint32_t _evpub_off_buf_fill_iov(queue_ctx *buf_s, size_t nbuf,
                                        IOV_TYPE iov[MAX_SEND_NIOV],
                                        off_buf_ctx *sndbuf[MAX_SEND_NIOV]) {
    if (nbuf > MAX_SEND_NIOV) {
        nbuf = MAX_SEND_NIOV;
    }
    size_t total = 0;
    uint32_t cnt = 0;
    off_buf_ctx *buf;
    size_t remain;
    for (uint32_t i = 0; i < (uint32_t)nbuf; i++) {
        buf = queue_at(buf_s, i);
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
        total += remain;
        cnt++;
        if (total >= MAX_SEND_SIZE) {
            break;
        }
    }
    return cnt;
}
// 根据实际发送字节数sent，从发送队列头部消费已完成的缓冲区，更新offset或弹出并释放
static void _evpub_off_buf_apply_sent(queue_ctx *buf_s, off_buf_ctx *sndbuf[MAX_SEND_NIOV],
                                      uint32_t niov, size_t sent) {
    off_buf_ctx *buf;
    size_t buflen;
    for (uint32_t i = 0; i < niov && sent > 0; i++) {
        buf = sndbuf[i];
        buflen = buf->lens - buf->offset;
        if (sent >= buflen) {
            sent -= buflen;
            _evpub_off_buf_release(buf);
            queue_pop(buf_s);
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
    ssize_t rtn = writev(fd, iov, (int)niov);
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
static int32_t _evpub_sock_send_normal(SOCKET fd, queue_ctx *buf_s, size_t *nsend) {
    int32_t rtn = ERR_OK;
    size_t nbuf, sended;
    uint32_t niov;
    IOV_TYPE iov[MAX_SEND_NIOV];
    off_buf_ctx *sndbuf[MAX_SEND_NIOV];
    while (0 != (nbuf = queue_size(buf_s))) {
        niov = _evpub_off_buf_fill_iov(buf_s, nbuf, iov, sndbuf);
        rtn = _evpub_sock_send_iov(fd, iov, niov, &sended);
        if (ERR_OK == rtn) {
            *nsend += sended;
            _evpub_off_buf_apply_sent(buf_s, sndbuf, niov, sended);
            if (0 == sended) {
                break;
            }
        } else {
            break;
        }
    }
    return rtn;
}
#if WITH_SSL
// 通过 SSL 发送队列中的数据。单次上限与抽干循环两条都不能去掉：超过 MAX_SSL_SEND_SIZE
// 会让 TLS1.3 KeyUpdate 断连，不抽干则边缘触发下发送就此停住
static int32_t _evpub_sock_send_ssl(SSL *ssl, queue_ctx *buf_s, size_t *nsend) {
    int32_t rtn = ERR_OK;
    size_t sended, lens;
    off_buf_ctx *buf;
    for (;;) {
        buf = queue_peek(buf_s);
        if (NULL == buf) {
            break;
        }
        lens = buf->lens - buf->offset;
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
            queue_pop(buf_s);
            _evpub_off_buf_release(buf);
        }
    }
    return rtn;
}
#endif
int32_t _evpub_sock_send(SOCKET fd, queue_ctx *buf_s, size_t *nsend, void *arg) {
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
void _evpub_add_bufs_sendto(watcher_ctx *watcher, sock_ctx *skctx, sendto_ctx *buf, int32_t tried) {
#ifdef EV_IOCP
    // IOCP 侧 _iocp_add_bufs_trysendto 本身已是独立的 try-then-queue 实现，不需要外部传入的尝试状态；
    // watcher 要用：发送卡在资源紧张上时靠它挂重试 tick
    (void)tried;
    _iocp_add_bufs_trysendto(watcher, skctx, buf);
#else
    _uev_add_bufs_sendto(watcher, skctx, buf, tried);
#endif
}
int32_t _evpub_try_sendto(watcher_ctx *watcher, sock_ctx *skctx, const void *data, size_t len, netaddr_ctx *addr) {
#ifdef EV_IOCP
    // IOCP 发送恒为异步投递，不存在“同步立即完成、零分配”的路径，统一交调用方转入排队
    (void)watcher;
    (void)skctx;
    (void)data;
    (void)len;
    (void)addr;
    return 1;
#else
    return _uev_try_sendto(watcher, skctx, data, len, addr);
#endif
}
