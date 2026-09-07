#include "event/uev.h"
#include "utils/buffer.h"
#include "utils/netutils.h"
#include "utils/tda.h"
#include "containers/hashmap.h"

#ifndef EV_IOCP

// 监听socket与所属监听器的绑定（SO_REUSEPORT时每个watcher有独立fd）
typedef struct lsnsock_ctx {
    sock_ctx sock;          // 监听socket的事件上下文
    struct listener_ctx *lsn; // 所属监听器
    watcher_ctx *watcher;   // 所属 watcher（EMFILE 退避 tick 回调重挂 READ 用）
    uint64_t backoff_until; // EMFILE 退避截止时刻(ms)，0=未退避
    ev_tick backoff_tick;   // EMFILE 退避周期驱动节点（cb 非 NULL 表示已挂）
}lsnsock_ctx;
// Unix平台监听器上下文
typedef struct listener_ctx {
    int32_t nlsn;           // 监听socket数量（等于nthreads，SO_REUSEPORT时每线程一个）
    atomic_t remove;        // 标记为待移除（ev_unlisten后设置）
    atomic_t ref;           // 引用计数（等于nlsn，每个_uev_remove_lsn减1）
    lsnsock_ctx *lsnsock;   // 监听socket数组
#if WITH_SSL
    evssl_ctx *evssl;       // SSL上下文（NULL表示不使用SSL）
#endif
    cbs_ctx cbs;            // 回调函数集合
    ud_cxt ud;              // 用户数据模板
    uint64_t id;            // 监听器唯一ID
}listener_ctx;
// Unix平台TCP连接上下文
typedef struct tcp_ctx {
    sock_ctx sock;          // 基础事件上下文（含fd/events/ev_cb）
    int32_t status;         // 连接状态标志位（sock_status组合）
#if WITH_SSL
    SSL *ssl;               // SSL会话（NULL表示普通TCP）
    struct evssl_ctx *evssl; // 待升级的SSL上下文（发送完毕后升级）
#endif
    size_t wb_size;         // 当前 buf_s 中字节累计
    uint64_t skid;          // 连接唯一ID
    tda_ctx tda;            // 字节告警翻倍状态
    buffer_ctx buf_r;       // 接收缓冲区
    queue_ctx buf_s;        // 发送队列
    cbs_ctx cbs;            // 回调函数集合
    ud_cxt ud;              // 用户数据
}tcp_ctx;
// Unix平台UDP上下文
typedef struct udp_ctx {
    sock_ctx sock;              // 基础事件上下文
    int32_t status;             // 状态标志位
    size_t wb_size;             // 当前 buf_s 中字节累计
    uint64_t skid;              // 连接唯一ID
    tda_ctx tda;                // 字节告警翻倍状态
    cbs_ctx cbs;                // 回调函数集合
    queue_ctx buf_s;            // 发送队列
    ud_cxt ud;                  // 用户数据
}udp_ctx;

static void _usk_on_rw_cb(watcher_ctx *watcher, sock_ctx *skctx, int32_t ev); // 前向声明：TCP读写事件回调

void _uev_sk_shutdown(sock_ctx *skctx) {
#if WITH_SSL
    tcp_ctx *tcp = UPCAST(skctx, tcp_ctx, sock);
    evssl_shutdown(tcp->ssl, tcp->sock.fd);
#else
    shutdown(skctx->fd, SHUT_RD);
#endif
}
void *_evpub_sk_new(void *args) {
    skpool_args *skargs = (skpool_args *)args;
    tcp_ctx *tcp;
    MALLOC(tcp, sizeof(tcp_ctx));
    tcp->sock.ev_cb = _usk_on_rw_cb;
    tcp->sock.type = SOCK_STREAM;
    tcp->sock.fd = skargs->fd;
    tcp->sock.events = 0;
#ifdef COMMIT_NCHANGES
    tcp->sock.chg_round = 0;
#endif
    tcp->status = STATUS_NONE;
    tcp->skid = createid();
#if WITH_SSL
    tcp->ssl = NULL;
    tcp->evssl = NULL;
#endif
    tcp->cbs = *skargs->cbs;
    COPY_UD(tcp->ud, skargs->ud);
    buffer_init(&tcp->buf_r);
    queue_init(&tcp->buf_s, sizeof(off_buf_ctx), INIT_SENDBUF_LEN);
    tcp->wb_size = 0;
    tda_init(&tcp->tda, WB_WARN_INIT_SIZE);
    return &tcp->sock;
}
void _evpub_sk_free(void *sk) {
    tcp_ctx *tcp = UPCAST((sock_ctx *)sk, tcp_ctx, sock);
#if WITH_SSL
    FREE_SSL(tcp->ssl);
#endif
    CLOSE_SOCK(tcp->sock.fd);
    buffer_free(&tcp->buf_r);
    _evpub_off_buf_clear(&tcp->buf_s);
    queue_free(&tcp->buf_s);
    UD_FREE(tcp->cbs.ud_free, &tcp->ud);
    FREE(tcp);
}
void _evpub_sk_clear(void *sk) {
    tcp_ctx *tcp = UPCAST((sock_ctx *)sk, tcp_ctx, sock);
    tcp->sock.events = 0;
#ifdef COMMIT_NCHANGES
    tcp->sock.chg_round = 0;
#endif
    tcp->status = STATUS_NONE;
#if WITH_SSL
    FREE_SSL(tcp->ssl);
    tcp->evssl = NULL;
#endif
    CLOSE_SOCK(tcp->sock.fd);
    _evpub_off_buf_clear(&tcp->buf_s);
    tcp->wb_size = 0;
    tda_init(&tcp->tda, WB_WARN_INIT_SIZE);
    buffer_drain(&tcp->buf_r, buffer_size(&tcp->buf_r));
    UD_FREE(tcp->cbs.ud_free, &tcp->ud);
}
void _evpub_sk_reset(void *sk, void *args) {
    tcp_ctx *tcp = UPCAST((sock_ctx *)sk, tcp_ctx, sock);
    skpool_args *skargs = (skpool_args *)args;
    tcp->sock.fd = skargs->fd;
    tcp->sock.ev_cb = _usk_on_rw_cb;
    tcp->cbs = *skargs->cbs;
    tcp->skid = createid();
    COPY_UD(tcp->ud, skargs->ud);
}
ud_cxt *_uev_get_ud(sock_ctx *skctx) {
    if (SOCK_STREAM == skctx->type) {
        return &UPCAST(skctx, tcp_ctx, sock)->ud;
    } else {
        return &UPCAST(skctx, udp_ctx, sock)->ud;
    }
}
int32_t _uev_check_skid(sock_ctx *skctx, const uint64_t skid) {
    if (SOCK_STREAM == skctx->type) {
        if (skid == UPCAST(skctx, tcp_ctx, sock)->skid) {
            return ERR_OK;
        }
    } else if (SOCK_DGRAM == skctx->type) {
        if (skid == UPCAST(skctx, udp_ctx, sock)->skid) {
            return ERR_OK;
        }
    }
    // type==0 (listener / pipe) 不属于业务 fd，直接拒绝；防 误传 listener_fd 时
    // UPCAST 强转读偏移到不可预测字段后碰巧匹配 skid 触发未定义行为
    return ERR_FAILED;
}
// 调用连接关闭回调
static inline void _usk_call_close_cb(ev_ctx *ev, tcp_ctx *tcp) {
    if (NULL != tcp->cbs.c_cb) {
        tcp->cbs.c_cb(ev, tcp->sock.fd, tcp->skid, SOCK_IS_CLIENT(tcp->status),
                      _evpub_close_type(tcp->status), &tcp->ud);
    }
}
// 调用UDP关闭回调。UDP 无 FIN 可言，STATUS_PEER_* 永不置位，close_type 恒为 LOCAL
static inline void _usk_call_udp_close_cb(ev_ctx *ev, udp_ctx *udp) {
    if (NULL != udp->cbs.c_cb) {
        udp->cbs.c_cb(ev, udp->sock.fd, udp->skid, 0, _evpub_close_type(udp->status), &udp->ud);
    }
}
static inline int32_t _usk_keep_event(watcher_ctx *watcher, sock_ctx *skctx, int32_t ev) {
#ifndef MANUAL_ADD
    if (BIT_CHECK(skctx->events, ev)) {
        return ERR_OK;
    }
#endif
    return _uev_add_event(watcher, skctx->fd, &skctx->events, ev, skctx);
}
// 把 socket 从事件循环摘净：drop_changes →（MANUAL_REMOVE 下）del_event → sockel_remove →
// CLOSE_SOCK → 清 ev_cb。drop 必须排在 del 之前：devpoll 下 del 只是把 POLLREMOVE 排进 changes
// 等下一轮提交，而 drop 按 fd 无差别过滤，顺序反了会把它一起丢掉、fd 没摘出 /dev/poll 就被 close。
// CLOSE_SOCK 立刻把 fd 还给 OS 并置 INVALID_SOCK，_evpub_sk_clear 再关是幂等的；ev_cb 清空挡住
// 同批次后续 events 二次进来（隔离期跨轮 stale 也读到 NULL），pool_pop 时由 _evpub_sk_reset 恢复
static inline void _usk_detach(watcher_ctx *watcher, sock_ctx *skctx) {
    _uev_drop_changes(watcher, skctx);
#ifdef MANUAL_REMOVE
    _uev_del_event(watcher, skctx->fd, &skctx->events, skctx->events, skctx);
#endif
    _evpub_sockel_remove(watcher, skctx->fd);
    CLOSE_SOCK(skctx->fd);
    skctx->ev_cb = NULL;
}
// 关闭 TCP 连接：触发关闭回调 → 摘出事件循环 → 释放 ud → 入隔离队列暂存 QTN_MS 后归 pool
static inline void _usk_close_tcp(watcher_ctx *watcher, tcp_ctx *tcp) {
    _usk_call_close_cb(watcher->ev, tcp);
    _usk_detach(watcher, &tcp->sock);
    // c_cb（prots_net_close）内部经 task_grab 才会清 ud.context，task 已从 maptasks 摘除时会被跳过；
    // 此处不依赖 task 存活直接清理，与 _evpub_sk_clear 的同一调用幂等（context 为 NULL 即直接返回）
    UD_FREE(tcp->cbs.ud_free, &tcp->ud);
    _uev_qtn_push(watcher, &tcp->sock, QTN_TCP);
}
// UDP datagram 无序无连接：从事件循环摘除 + close fd + 清回调 + qtn 隔离期延后释放
static inline void _usk_close_udp(watcher_ctx *watcher, udp_ctx *udp) {
    // 内层错误路径(如 _uev_disconnect)已关闭后，外层可能再次直调本函数；fd 已 INVALID 即早退，防止同一 udp 二次入 qtn 隔离队列被 drain 两次 free
    if (INVALID_SOCK == udp->sock.fd) {
        return;
    }
    _usk_call_udp_close_cb(watcher->ev, udp);
    _usk_detach(watcher, &udp->sock);
    _uev_qtn_push(watcher, &udp->sock, QTN_UDP);
}
void _uev_disconnect(watcher_ctx *watcher, sock_ctx *skctx) {
    if (SOCK_STREAM == skctx->type) {
        tcp_ctx *tcp = UPCAST(skctx, tcp_ctx, sock);
        if (BIT_CHECK(tcp->status, STATUS_ERROR)) {
            return;
        }
        _evpub_close_flush_tcp(tcp->sock.fd, &tcp->buf_s, tcp->status, &tcp->wb_size, TCP_SSL(tcp));
        BIT_SET(tcp->status, STATUS_ERROR);
        _uev_sk_shutdown(skctx);
        // READ 接住 shutdown 造成的 EOF 边沿, WRITE 触发 _usk_on_rw_cb 入口的 STATUS_ERROR
        // 分支就地关闭; 两次注册都是拆连接的路, 任一注册不上就直接关, 别把连接吊到 keepalive
        if (ERR_OK != _usk_keep_event(watcher, &tcp->sock, EVENT_READ)
            || ERR_OK != _usk_keep_event(watcher, &tcp->sock, EVENT_WRITE)) {
            _usk_close_tcp(watcher, tcp);
        }
    } else {
        // UDP datagram 无连接,没有待发队列要冲
        udp_ctx *udp = UPCAST(skctx, udp_ctx, sock);
        if (BIT_CHECK(udp->status, STATUS_ERROR)) {
            return;
        }
        BIT_SET(udp->status, STATUS_ERROR);
        if (ERR_OK != _usk_keep_event(watcher, skctx, EVENT_WRITE)) {
            _usk_close_udp(watcher, udp);
        }
    }
}
// 调用accept回调，返回值非ERR_OK则拒绝连接
static inline int32_t _usk_call_acp_cb(ev_ctx *ev, tcp_ctx *tcp) {
    if (NULL != tcp->cbs.acp_cb) {
        return tcp->cbs.acp_cb(ev, tcp->sock.fd, tcp->skid, &tcp->ud);
    }
    return ERR_OK;
}
// 调用connect回调，返回值非ERR_OK则断开连接
static inline int32_t _usk_call_conn_cb(ev_ctx *ev, tcp_ctx *tcp, int32_t err) {
    if (NULL != tcp->cbs.conn_cb) {
        return tcp->cbs.conn_cb(ev, tcp->sock.fd, tcp->skid, err, &tcp->ud);
    }
    return ERR_OK;
}
// 调用SSL握手完成回调，返回值非ERR_OK则断开连接
static inline int32_t _usk_call_ssl_exchanged_cb(ev_ctx *ev, tcp_ctx *tcp) {
    if (NULL != tcp->cbs.exch_cb) {
        return tcp->cbs.exch_cb(ev, tcp->sock.fd, tcp->skid, SOCK_IS_CLIENT(tcp->status), &tcp->ud, TCP_SSL(tcp));
    }
    return ERR_OK;
}
// 调用数据接收回调（nread > 0 才触发）
static inline void _usk_call_recv_cb(ev_ctx *ev, tcp_ctx *tcp, size_t nread) {
    if (nread > 0) {
        tcp->cbs.r_cb(ev, tcp->sock.fd, tcp->skid, SOCK_IS_CLIENT(tcp->status), &tcp->buf_r, nread, &tcp->ud);
    }
}
// 调用发送完成回调（nsend > 0 且有s_cb 才触发）
static inline void _usk_call_send_cb(ev_ctx *ev, tcp_ctx *tcp, size_t nsend) {
    if (NULL != tcp->cbs.s_cb
        && nsend > 0) {
        tcp->cbs.s_cb(ev, tcp->sock.fd, tcp->skid, SOCK_IS_CLIENT(tcp->status), nsend, &tcp->ud);
    }
}
// 调用UDP接收回调；0 字节 datagram 由本函数过滤不向上抛（_usk_on_udp_rcb 仍不视为 EOF，
// 继续 recvmsg 循环），避免上层处理空 payload 的特殊路径
static inline void _usk_call_recvfrom_cb(ev_ctx *ev, udp_ctx *udp, char *buf, netaddr_ctx *addr, size_t nread) {
    if (nread > 0) {
        udp->cbs.rf_cb(ev, udp->sock.fd, udp->skid, buf, nread, addr, &udp->ud);
    }
}
#if WITH_SSL
// 触发切ssl
static int32_t _usk_ssl_exchange_trigger(watcher_ctx *watcher, tcp_ctx *tcp, struct evssl_ctx *evssl) {
    tcp->ssl = evssl_setfd(evssl, tcp->sock.fd);
    if (NULL == tcp->ssl) {
        return ERR_FAILED;
    }
    if (BIT_CHECK(tcp->status, STATUS_CLIENT)) {
        switch (evssl_tryconn(tcp->ssl)) {
        case ERR_OK://完成
            return _usk_call_ssl_exchanged_cb(watcher->ev, tcp);
        case 1://等待读就绪（WANT_READ）
            BIT_SET(tcp->status, STATUS_AUTHSSL);
            break;
        case 2://等待写就绪（WANT_WRITE）：注册写事件，写就绪后由 _usk_on_rw_cb 经 AUTHSSL 驱动重试
            BIT_SET(tcp->status, STATUS_AUTHSSL);
            if (ERR_OK != _uev_add_event(watcher, tcp->sock.fd, &tcp->sock.events, EVENT_WRITE, &tcp->sock)) {
                return ERR_FAILED;
            }
            break;
        default://错误
            return ERR_FAILED;
        }
    } else {
        BIT_SET(tcp->status, STATUS_AUTHSSL);
    }
    return ERR_OK;
}
#endif
void _uev_try_ssl_exchange(watcher_ctx *watcher, sock_ctx *skctx, struct evssl_ctx *evssl, int32_t client) {
#if WITH_SSL
    if (SOCK_STREAM != skctx->type) {
        LOG_WARN("can't ssl exchange on udp.");
        return;
    }
    tcp_ctx *tcp = UPCAST(skctx, tcp_ctx, sock);
    if (0 == _evpub_ssl_exchange_check(tcp->ssl, &tcp->status, client)) {
        return;
    }
    if (BIT_CHECK(skctx->events, EVENT_WRITE)) {
        tcp->evssl = evssl;
        BIT_SET(tcp->status, STATUS_SSLEXCHANGE);
    } else {
        if (ERR_OK != _usk_ssl_exchange_trigger(watcher, tcp, evssl)) {
            _uev_disconnect(watcher, skctx);
            LOG_ERROR("ssl exchange error.");
        }
    }
#else
    (void)watcher;
    (void)skctx;
    (void)evssl;
    (void)client;
#endif
}
// 从socket读取数据到接收缓冲区并触发recv回调，MANUAL_ADD时需重新注册读事件
static inline int32_t _usk_tcp_recv(watcher_ctx *watcher, tcp_ctx *tcp) {
    size_t nread;
    int32_t evrtn = ERR_OK;
    int32_t rtn = buffer_from_sock(&tcp->buf_r, tcp->sock.fd, &nread, _evpub_sock_read, TCP_SSL(tcp));
#if WITH_SSL
    if (ERR_OK == rtn
        && NULL != tcp->ssl
        && SSL_want_write(tcp->ssl)) {// tls1.3 KeyUpdate探测
        BIT_SET(tcp->status, STATUS_KEYUPDATE_WRITE);
        evrtn = _uev_add_event(watcher, tcp->sock.fd, &tcp->sock.events, EVENT_WRITE, &tcp->sock);
    }
#endif
    _usk_call_recv_cb(watcher->ev, tcp, nread);
    if (ERR_OK != rtn) {
        _evpub_mark_close(&tcp->status, rtn);
        return ERR_FAILED;
    }
    if (ERR_OK != evrtn) {
        return ERR_FAILED;
    }
    return _usk_keep_event(watcher, &tcp->sock, EVENT_READ);
}
// 发送队列中的数据，队列空后删除写事件（可选SSL升级），MANUAL_ADD时重注册写事件。
// STATUS_KEYUPDATE_READ 的置与清都只在本函数：别处清位条件对不上置位处，残留期间
// _uev_add_bufs_send 会早退，请求-响应型协议下就再等不到读事件、连接卡死。
static inline int32_t _usk_tcp_send(watcher_ctx *watcher, tcp_ctx *tcp) {
    size_t nsend;
    int32_t rtn = _evpub_sock_send(tcp->sock.fd, &tcp->buf_s, &nsend, TCP_SSL(tcp));
    tcp->wb_size -= nsend;
    _usk_call_send_cb(watcher->ev, tcp, nsend);
    if (ERR_OK != rtn) {
        _evpub_mark_close(&tcp->status, rtn);
        return ERR_FAILED;// 分类已进 status, 不透传 evssl_* 的 1/2(口径同 _usk_tcp_recv)
    }
    uint32_t cnt = queue_size(&tcp->buf_s);
#if WITH_SSL
    // 挂读，并且必须摘掉写事件：四套 POSIX 后端只有 epoll 是 ET，kqueue/evport/pollset/devpoll
    // 留着写事件就每轮重进本函数、SSL_write 再返 WANT_READ，空转烧满 watcher 线程。
    // 摘掉安全：清位后本函数按 cnt 重新注册
    if (NULL != tcp->ssl) {
        if (0 != cnt && SSL_want_read(tcp->ssl)) {
            BIT_SET(tcp->status, STATUS_KEYUPDATE_READ);
            if (BIT_CHECK(tcp->sock.events, EVENT_WRITE)) {
                _uev_del_event(watcher, tcp->sock.fd, &tcp->sock.events, EVENT_WRITE, &tcp->sock);
            }
            return _usk_keep_event(watcher, &tcp->sock, EVENT_READ);
        }
        BIT_REMOVE(tcp->status, STATUS_KEYUPDATE_READ);
    }
#endif
    if (0 == cnt) {
        if (BIT_CHECK(tcp->sock.events, EVENT_WRITE)) {// 直发快路径无EVENT_WRITE
            _uev_del_event(watcher, tcp->sock.fd, &tcp->sock.events, EVENT_WRITE, &tcp->sock);
        }
#if WITH_SSL
        if (BIT_CHECK(tcp->status, STATUS_SSLEXCHANGE)) {
            BIT_REMOVE(tcp->status, STATUS_SSLEXCHANGE);
            if (ERR_OK != _usk_ssl_exchange_trigger(watcher, tcp, tcp->evssl)) {
                LOG_ERROR("ssl exchange error.");
                return ERR_FAILED;
            }
        }
#endif
        return ERR_OK;
    }
    return _usk_keep_event(watcher, &tcp->sock, EVENT_WRITE);
}
#if WITH_SSL
// SSL 握手；1 继续执行 return；0：返回
static int32_t _usk_ssl_do_handshake(watcher_ctx *watcher, tcp_ctx *tcp, int32_t *err) {
    *err = ERR_OK;
    int32_t rtn = BIT_CHECK(tcp->status, STATUS_CLIENT) ?
                  evssl_tryconn(tcp->ssl) :
                  evssl_tryacpt(tcp->ssl);
    switch (rtn) {
    case ERR_OK://完成：清除写事件注册（若有），然后通知上层
        if (BIT_CHECK(tcp->sock.events, EVENT_WRITE)) {
            _uev_del_event(watcher, tcp->sock.fd, &tcp->sock.events, EVENT_WRITE, &tcp->sock);
        }
        if (ERR_OK != _usk_call_ssl_exchanged_cb(watcher->ev, tcp)) {
            *err = ERR_FAILED;
            return 1;
        }
        BIT_REMOVE(tcp->status, STATUS_AUTHSSL);
#ifdef READV_EINVAL
        return 0;
#else
        return 1;
#endif
    case 1://等待读就绪（WANT_READ）：若之前在等写，撤销写事件
        if (BIT_CHECK(tcp->sock.events, EVENT_WRITE)) {
            _uev_del_event(watcher, tcp->sock.fd, &tcp->sock.events, EVENT_WRITE, &tcp->sock);
        }
        if (ERR_OK != _usk_keep_event(watcher, &tcp->sock, EVENT_READ)) {
            *err = ERR_FAILED;
            return 1;
        }
        return 0;
    case 2://等待写就绪（WANT_WRITE）：注册写事件，写就绪后再次进入此分支重试
        if (ERR_OK != _uev_add_event(watcher, tcp->sock.fd, &tcp->sock.events, EVENT_WRITE, &tcp->sock)) {
            *err = ERR_FAILED;
            return 1;
        }
        return 0;
    default://错误
        *err = ERR_FAILED;
        return 1;
    }
}
#endif
// TCP读写事件统一回调：处理SSL握手、读、写，任意失败则关闭连接
static void _usk_on_rw_cb(watcher_ctx *watcher, sock_ctx *skctx, int32_t ev) {
    tcp_ctx *tcp = UPCAST(skctx, tcp_ctx, sock);
    if (BIT_CHECK(tcp->status, STATUS_ERROR)) {
        _usk_close_tcp(watcher, tcp);
        return;
    }
    int32_t evread = BIT_CHECK(ev, EVENT_READ);
    int32_t evwrite = BIT_CHECK(ev, EVENT_WRITE);
#if WITH_SSL
    if (NULL != tcp->ssl) {
        if (BIT_CHECK(tcp->status, STATUS_AUTHSSL)) {// SSL握手
            int32_t err;
            if (!_usk_ssl_do_handshake(watcher, tcp, &err)) {
                return;
            }
            if (ERR_OK != err) {
                BIT_SET(tcp->status, STATUS_ERROR);
                _usk_close_tcp(watcher, tcp);
                return;
            }
        } else {
            if (evwrite && BIT_CHECK(tcp->status, STATUS_KEYUPDATE_WRITE)) {// tls1.3 KeyUpdate 处理
                BIT_REMOVE(tcp->status, STATUS_KEYUPDATE_WRITE);
                if (0 == queue_size(&tcp->buf_s)) {
                    evwrite = 0;
                    _uev_del_event(watcher, tcp->sock.fd, &tcp->sock.events, EVENT_WRITE, &tcp->sock);
                }
                evread = 1;// 挂起的 SSL_read 必须先完成,才轮到 SSL_write
            }
        }
    }
#endif// WITH_SSL
    int32_t rtn = ERR_OK;
    if (evread) {
        rtn = _usk_tcp_recv(watcher, tcp);
#if WITH_SSL
        // 刚读到的对端数据已喂进 OpenSSL，回头重试上次没发出去的。KEYUPDATE_WRITE 也挂着时让它先跑，
        // 免得在挂起的 SSL_read 之前调 SSL_write。只管重试不清位——清位归 _usk_tcp_send
        if (ERR_OK == rtn
            && BIT_CHECK(tcp->status, STATUS_KEYUPDATE_READ)
            && !BIT_CHECK(tcp->status, STATUS_KEYUPDATE_WRITE)) {
            rtn = _usk_tcp_send(watcher, tcp);
            evwrite = 0;
        }
#endif
    }
    if (ERR_OK == rtn && evwrite && !BIT_CHECK(tcp->status, STATUS_KEYUPDATE_WRITE)) {
        rtn = _usk_tcp_send(watcher, tcp);
    }
    if (ERR_OK != rtn) {
        BIT_SET(tcp->status, STATUS_ERROR);
        _usk_close_tcp(watcher, tcp);
        return;
    }
}
void _uev_add_bufs_send(watcher_ctx *watcher, sock_ctx *skctx, off_buf_ctx *buf) {
    tcp_ctx *tcp = UPCAST(skctx, tcp_ctx, sock);
    // 已在关闭流程：拒收新数据
    if (BIT_CHECK(tcp->status, STATUS_ERROR)) {
        _evpub_off_buf_release(buf);
        return;
    }
    if (!_evpub_sendqu_check_tcp(&tcp->buf_s, tcp->status, skctx->fd)) {
        _evpub_off_buf_release(buf);
        _uev_disconnect(watcher, skctx);
        return;
    }
    int32_t was_empty = (0 == queue_size(&tcp->buf_s));
    tcp->wb_size += buf->lens;
    _evpub_sendqu_tda(&tcp->tda, tcp->wb_size, skctx->fd, 1);
    queue_push(&tcp->buf_s, buf);
    // 队列本来就非空：要么 EVENT_WRITE 已注册,要么正处 KEYUPDATE_READ(那时写事件被摘掉,
    // 由 _usk_on_rw_cb 收到数据后重驱动 _usk_tcp_send)。两种都无需在此重复处理
    if (!was_empty) {
        return;
    }
#if WITH_SSL
    // KeyUpdate 挂起 SSL_read 时调 SSL_write 违反 OpenSSL"必须先重试同一操作"的约定；
    // EVENT_WRITE 在置 KEYUPDATE_WRITE 时已注册,数据留队等 _usk_on_rw_cb 重试完再按序处理。
    // 不判 KEYUPDATE_READ:它只在 buf_s 非空时置位,上面 !was_empty 已经先返回了
    if (BIT_CHECK(tcp->status, STATUS_KEYUPDATE_WRITE)) {
        return;
    }
#endif
    // 刚从空队列开始：立即尝试发送，避免多等一轮事件循环才发出；
    // 发送未完成时，_usk_tcp_send 内部的 _usk_keep_event 会负责注册/保活 EVENT_WRITE
    if (ERR_OK != _usk_tcp_send(watcher, tcp)) {
        BIT_SET(tcp->status, STATUS_ERROR);
        _usk_close_tcp(watcher, tcp);
    }
}
static void _usk_on_connect_cb_err(watcher_ctx *watcher, tcp_ctx *tcp) {
    _usk_call_conn_cb(watcher->ev, tcp, ERR_FAILED);
    _evpub_sockel_remove(watcher, tcp->sock.fd);
    // 顺序同 _usk_detach，但这里 drop 多担一件事：进来前已排过的那条 del_event 带着 skctx
    // 当 udata，sock 紧接着回池，kqueue 下留着会落到复用同一 fd 号的新连接上。
    // 代价是 devpoll 的 POLLREMOVE 也被清掉，故下面按平台补排一次
    _uev_drop_changes(watcher, &tcp->sock);
#ifdef MANUAL_REMOVE
    _uev_del_event(watcher, tcp->sock.fd, &tcp->sock.events, tcp->sock.events, &tcp->sock);
#endif
    tcp->sock.ev_cb = NULL;
    pool_push(&watcher->pool, &tcp->sock, 0);
}
// connect完成事件回调：检查连接结果，切换为读写回调，触发conn回调
static void _usk_on_connect_cb(watcher_ctx *watcher, sock_ctx *skctx, int32_t ev) {
    (void)ev;
    tcp_ctx *tcp = UPCAST(skctx, tcp_ctx, sock);
    tcp->sock.ev_cb = _usk_on_rw_cb;
    _uev_del_event(watcher, tcp->sock.fd, &tcp->sock.events, tcp->sock.events, skctx);
    if (BIT_CHECK(tcp->status, STATUS_ERROR)
        || ERR_OK != sock_checkconn(tcp->sock.fd)) {
        _usk_on_connect_cb_err(watcher, tcp);
        return;
    }
    BIT_SET(tcp->status, STATUS_ESTABLISHED);
    if (ERR_OK != _evpub_tcp_keepalive(tcp->sock.fd)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        _usk_on_connect_cb_err(watcher, tcp);
        return;
    }
#if WITH_SSL
    if (NULL != tcp->evssl) {// 默认启用ssl，初始化
        tcp->ssl = evssl_setfd(tcp->evssl, tcp->sock.fd);
        if (NULL == tcp->ssl) {
            _usk_on_connect_cb_err(watcher, tcp);
            return;
        }
        BIT_SET(tcp->status, STATUS_AUTHSSL);
    }
#endif
    if (ERR_OK != _uev_add_event(watcher, tcp->sock.fd, &tcp->sock.events, EVENT_READ, &tcp->sock)) {
        _usk_on_connect_cb_err(watcher, tcp);
        return;
    }
    // 链接成功回调
    if (ERR_OK != _usk_call_conn_cb(watcher->ev, tcp, ERR_OK)) {
        _uev_disconnect(watcher, skctx);
        return;
    }
#if WITH_SSL
    if (NULL != tcp->ssl) {
        switch (evssl_tryconn(tcp->ssl)) {
        case ERR_OK://完成
            BIT_REMOVE(tcp->status, STATUS_AUTHSSL);
            if (ERR_OK != _usk_call_ssl_exchanged_cb(watcher->ev, tcp)) {
                _uev_disconnect(watcher, skctx);
                return;
            }
            break;
        case 1:// WANT_READ
            break;
        case 2://WANT_WRITE
            if (ERR_OK != _uev_add_event(watcher, tcp->sock.fd, &tcp->sock.events, EVENT_WRITE, &tcp->sock)) {
                _uev_disconnect(watcher, skctx);
                return;
            }
            break;
        default://错误
            _uev_disconnect(watcher, skctx);
            return;
        }
    }
#endif
}
int32_t ev_connect(ev_ctx *ctx, struct evssl_ctx *evssl, const char *ip, const uint16_t port, cbs_ctx *cbs, ud_cxt *ud,
    int32_t setsess, SOCKET *fd, uint64_t *skid) {
    netaddr_ctx addr;
    if (ERR_OK != _evpub_sock_launch_check(ctx, ip, port, cbs, ud, 0, &addr)) {
        return ERR_FAILED;
    }
    *fd = sock_create_cloexec(netaddr_family(&addr), SOCK_STREAM, 0);
    if (INVALID_SOCK == *fd) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        UD_FREE(cbs->ud_free, ud);
        return ERR_FAILED;
    }
    if (ERR_OK != _evpub_tcp_sockopts(*fd)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        CLOSE_SOCK((*fd));
        UD_FREE(cbs->ud_free, ud);
        return ERR_FAILED;
    }
    int32_t rtn = connect(*fd, netaddr_addr(&addr), netaddr_size(&addr));
    if (ERR_OK != rtn) {
        rtn = ERRNO;
        if (!ERR_CONNECT_RETRIABLE(rtn)) {
            LOG_ERROR("connect %s:%d, %s", ip, port, ERRORSTR(ERRNO));
            CLOSE_SOCK((*fd));
            UD_FREE(cbs->ud_free, ud);
            return ERR_FAILED;
        }
    }
    skpool_args skargs = { *fd, cbs, ud };
    sock_ctx *skctx = (sock_ctx *)_evpub_sk_new(&skargs);
    skctx->ev_cb = _usk_on_connect_cb;
    tcp_ctx *tcp = UPCAST(skctx, tcp_ctx, sock);
    if (setsess) {
        tcp->ud.sess = tcp->skid;
    }
    BIT_SET(tcp->status, STATUS_CLIENT);
    *skid = tcp->skid;
#if WITH_SSL
    tcp->evssl = evssl;
#else
    (void)evssl;
#endif
    _cmd_connect(ctx, skctx, NULL);
    return ERR_OK;
}
void _uev_add_conn_inloop(watcher_ctx *watcher, sock_ctx *skctx) {
    _evpub_sockel_add(watcher, skctx);
    if (ERR_OK != _uev_add_event(watcher, skctx->fd, &skctx->events, EVENT_WRITE, skctx)) {
        tcp_ctx *tcp = UPCAST(skctx, tcp_ctx, sock);
        _usk_call_conn_cb(watcher->ev, tcp, ERR_FAILED);
        _evpub_sockel_remove(watcher, skctx->fd);
        pool_push(&watcher->pool, skctx, 0);
    }
}
// EMFILE/ENFILE 退避到点：重挂监听 READ 并摘除本 tick（重挂失败则继续退避）
static uint32_t _usk_accept_backoff(void *ud, uint64_t now_ms) {
    lsnsock_ctx *acpt = ud;
    if (now_ms < acpt->backoff_until) {
        return (uint32_t)(acpt->backoff_until - now_ms);
    }
    if (ERR_OK != _usk_keep_event(acpt->watcher, &acpt->sock, EVENT_READ)) {
        acpt->backoff_until = now_ms + ACCEPT_BACKOFF_MS;
        return ACCEPT_BACKOFF_MS;
    }
    acpt->backoff_until = 0;
    acpt->backoff_tick.cb = NULL;
    _evpub_tick_remove(acpt->watcher, &acpt->backoff_tick);
    return EVENT_WAIT_TIMEOUT;
}
// EINTR / ECONNABORTED 各平台通用：前者瞬时，后者消耗一个 backlog 项，重试必推进。
// 其余是 Linux accept(2) 把新连接上已挂起的网络错误当作 accept 自身错误码返回的那一组，
// 文档要求按 EAGAIN 处理（重试下一条），它们不表示监听 socket 出错；macOS/BSD 的 accept
// 不返回这些，而 EOPNOTSUPP 在那里只表示"fd 不是 SOCK_STREAM"的永久错误，当可重试会无界自旋
static int32_t _usk_accept_retriable(int32_t err) {
    switch (err) {
    case EINTR:
    case ECONNABORTED:
#if defined(OS_LINUX)
    case ENETDOWN:
    case EPROTO:
    case ENOPROTOOPT:
    case EHOSTDOWN:
    case EHOSTUNREACH:
    case EOPNOTSUPP:
    case ENETUNREACH:
#ifdef ENONET
    case ENONET:
#endif
#endif
        return 1;
    default:
        return 0;
    }
}
// accept 返回 INVALID_SOCK 的分类：可重试错误返回 ERR_OK 继续，其余返回 ERR_FAILED 退出循环；
// EMFILE/ENFILE 额外暂停监听 READ 并挂退避 tick，到点重挂重试（不误拒 backlog，避免 ET 停滞/LT 忙轮询）
static inline int32_t _usk_check_accept(watcher_ctx *watcher, lsnsock_ctx *acpt) {
    int32_t err = ERRNO;
    if (0 != _usk_accept_retriable(err)) {
        return ERR_OK;
    }
    if (EMFILE == err || ENFILE == err) {
        _uev_del_event(watcher, acpt->sock.fd, &acpt->sock.events, EVENT_READ, &acpt->sock);
        acpt->backoff_until = timer_cur_ms(&watcher->timer) + ACCEPT_BACKOFF_MS;
        if (NULL == acpt->backoff_tick.cb) {
            acpt->watcher = watcher;
            acpt->backoff_tick.cb = _usk_accept_backoff;
            acpt->backoff_tick.ud = acpt;
            _evpub_tick_add(watcher, &acpt->backoff_tick);
        }
    }
    return ERR_FAILED;
}
// 监听socket可读事件回调：循环accept新连接并分发给对应watcher。
// 末尾重挂 READ 失败时这条监听 socket 从此收不到事件，只能弃掉：cbs_ctx 里没有"监听失效"
// 这类回调，通知不到业务，故把后果写进日志——SO_REUSEPORT 下每个 watcher 一条，掉一条只是
// 少一份 accept 容量，端口照常可连，不打出来没人会发现。清 ev_cb 的理由见 _usk_detach
static void _usk_on_accept_cb(watcher_ctx *watcher, sock_ctx *skctx, int32_t ev) {
    (void)ev;
    lsnsock_ctx *acpt = UPCAST(skctx, lsnsock_ctx, sock);
    SOCKET fd;
    watcher_ctx *to;
    int32_t unremove;
    while ((unremove = (0 == ATOMIC_GET(&acpt->lsn->remove)))) {
        fd = sock_accept_cloexec(acpt->sock.fd, NULL, NULL);
        if (INVALID_SOCK == fd) {
            if (ERR_OK == _usk_check_accept(watcher, acpt)) {
                continue;
            }
            break;
        }
        if (ERR_OK != _evpub_tcp_sockopts(fd)
            || ERR_OK != _evpub_tcp_keepalive(fd)) {
            CLOSE_SOCK(fd);
            continue;
        }
        to = GET_PTR(watcher->ev->watcher, watcher->ev->nthreads, fd);
        if (to->index == watcher->index) {
            _uev_add_acpfd_inloop(to, fd, acpt->lsn);
        } else {
            // 跨 watcher 投递前 ref++ 占位：防 ev_unlisten 在目标 watcher 取出
            // CMD_ADDACP 前将 lsn ref 减到 0 释放，_on_cmd_addacp/_uev_free_pipe 配对减
            ATOMIC_ADD(&acpt->lsn->ref, 1);
            _cmd_add_acpfd(to, fd, acpt->lsn);
        }
    }
    if (unremove && NULL == acpt->backoff_tick.cb) {
        if (ERR_OK != _usk_keep_event(watcher, &acpt->sock, EVENT_READ)) {
            LOG_ERROR("watcher %d listener fd %d re-arm READ failed (%s), no longer accepts.",
                      watcher->index, (int32_t)acpt->sock.fd, ERRORSTR(ERRNO));// 须在 CLOSE_SOCK 之前:close 会覆写 errno
            _evpub_sockel_remove(watcher, acpt->sock.fd);
            CLOSE_SOCK(acpt->sock.fd);
            acpt->sock.ev_cb = NULL;
        }
    }
}
static void _uev_add_acpfd_inloop_err(watcher_ctx *watcher, tcp_ctx *tcp) {
    _evpub_sockel_remove(watcher, tcp->sock.fd);
    pool_push(&watcher->pool, &tcp->sock, 0);
}
void _uev_add_acpfd_inloop(watcher_ctx *watcher, SOCKET fd, listener_ctx *lsn) {
    skpool_args skargs = { fd, &lsn->cbs, &lsn->ud };
    sock_ctx *skctx = pool_pop(&watcher->pool, &skargs, 0);
    tcp_ctx *tcp = UPCAST(skctx, tcp_ctx, sock);
    BIT_SET(tcp->status, STATUS_ESTABLISHED);// accept 出来的连接已连通
    _evpub_sockel_add(watcher, skctx);
#if WITH_SSL
    if (NULL != lsn->evssl) {// 默认启用ssl
        // 设置ssl
        tcp->ssl = evssl_setfd(lsn->evssl, tcp->sock.fd);
        if (NULL == tcp->ssl) {
            _uev_add_acpfd_inloop_err(watcher, tcp);
            return;
        }
        BIT_SET(tcp->status, STATUS_AUTHSSL);
    }
#endif
    if (ERR_OK != _uev_add_event(watcher, fd, &skctx->events, EVENT_READ, skctx)) {
        _uev_add_acpfd_inloop_err(watcher, tcp);
        return;
    }
    if (ERR_OK != _usk_call_acp_cb(watcher->ev, tcp)) {
        _uev_disconnect(watcher, skctx);
        return;
    }
}
// 关闭监听socket（无SO_REUSEPORT只关闭第一个，否则关闭所有cnt个）
static void _usk_close_lsnsock(listener_ctx *lsn, int32_t cnt) {
#ifndef SO_REUSEPORT
    CLOSE_SOCK(lsn->lsnsock[0].sock.fd);
#else
    for (int32_t i = 0; i < cnt; i++) {
        CLOSE_SOCK(lsn->lsnsock[i].sock.fd);
    }
#endif
}
int32_t ev_listen(ev_ctx *ctx, struct evssl_ctx *evssl, const char *ip, const uint16_t port,
    cbs_ctx *cbs, ud_cxt *ud, uint64_t *id) {
    netaddr_ctx addr;
    if (ERR_OK != _evpub_sock_launch_check(ctx, ip, port, cbs, ud, 0, &addr)) {
        return ERR_FAILED;
    }
#ifndef SO_REUSEPORT
    SOCKET fd = _evpub_listen(&addr);
    if (INVALID_SOCK == fd) {
        LOG_ERROR("listen %s:%d error.", ip, port);
        UD_FREE(cbs->ud_free, ud);
        return ERR_FAILED;
    }
#endif
    listener_ctx *lsn;
    MALLOC(lsn, sizeof(listener_ctx));
#ifndef SO_REUSEPORT
    lsn->nlsn = 1;
#else
    lsn->nlsn = ctx->nthreads;
#endif
    ATOMIC_SET(&lsn->ref, 0);
    ATOMIC_SET(&lsn->remove, 0);
    lsn->cbs = *cbs;
    COPY_UD(lsn->ud, ud);
#if WITH_SSL
    lsn->evssl = evssl;
#else
    (void)evssl;
#endif
    CALLOC(lsn->lsnsock, lsn->nlsn, sizeof(lsnsock_ctx));
    int32_t i;
    lsnsock_ctx *lsnsock;
    for (i = 0; i < lsn->nlsn; i++) {
        lsnsock = &lsn->lsnsock[i];
        lsnsock->lsn = lsn;
        lsnsock->sock.ev_cb = _usk_on_accept_cb;
#ifndef SO_REUSEPORT
        lsnsock->sock.fd = fd;
#else
        lsnsock->sock.fd = _evpub_listen(&addr);
        if (INVALID_SOCK == lsnsock->sock.fd) {
            // 仅关闭已成功创建的 i 个 fd;_uev_freelsn 内 ud_free(lsn->ud) 释放浅拷贝资源
            lsn->nlsn = i;
            _uev_freelsn(lsn);
            return ERR_FAILED;
        }
#endif
    }
    ATOMIC_SET(&lsn->ref, lsn->nlsn);
    lsn->id = createid();
    for (i = 0; i < lsn->nlsn; i++) {
        _cmd_listen(&ctx->watcher[i], &lsn->lsnsock[i].sock);
    }
    spin_lock(&ctx->spin);
    array_push_back(&ctx->arrlsn, &lsn);
    spin_unlock(&ctx->spin);
    SET_PTR(id, lsn->id);
    return ERR_OK;
}
void _uev_add_lsn_inloop(watcher_ctx *watcher, sock_ctx *skctx) {
    _evpub_sockel_add(watcher, skctx);
    if (ERR_OK != _uev_add_event(watcher, skctx->fd, &skctx->events, EVENT_READ, skctx)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        _evpub_sockel_remove(watcher, skctx->fd);
        CLOSE_SOCK(skctx->fd);
    }
}
void _uev_freelsn(listener_ctx *lsn) {
    _usk_close_lsnsock(lsn, lsn->nlsn);
    UD_FREE(lsn->cbs.ud_free, &lsn->ud);
    FREE(lsn->lsnsock);
    FREE(lsn);
}
// 递减 lsn 引用计数；归零时立即释放 listener_ctx。
// 主线程 / drain 残留路径用 (无 _uev_loop_event 在迭代，立即 FREE 安全)
void _uev_try_freelsn(struct listener_ctx *lsn) {
    if (1 == ATOMIC_ADD(&lsn->ref, -1)) {
        _uev_freelsn(lsn);
    }
}
void _uev_qtn_push(watcher_ctx *watcher, void *obj, qtn_type type) {
    qtn_entry e;
    e.obj = obj;
    e.enter_ms = timer_cur_ms(&watcher->timer);
    e.type = type;
    queue_push(&watcher->qtn, &e);
}
void _uev_qtn_freelsn(watcher_ctx *watcher, listener_ctx *lsn) {
    if (1 == ATOMIC_ADD(&lsn->ref, -1)) {
        _uev_qtn_push(watcher, lsn, QTN_LSN);
    }
}
// 释放一个隔离期到点的对象。hard 区分两种收尾:0 为常规 drain,tcp 回池留着复用;
// 1 为 watcher 退出前的 flush,池本身也要没了,tcp 得真释放。udp / lsn 两种走法相同。
// 新增 qtn_type 只需在这里加一个 case,别再回到两个循环里各加一次
static void _uev_qtn_release(watcher_ctx *watcher, qtn_entry *e, int32_t hard) {
    switch (e->type) {
    case QTN_TCP:
        if (0 != hard) {
            _evpub_sk_free((sock_ctx *)e->obj);
        } else {
            pool_push(&watcher->pool, (sock_ctx *)e->obj, 0);
        }
        break;
    case QTN_UDP:
        _uev_free_udp((sock_ctx *)e->obj);
        break;
    case QTN_LSN:
        _uev_freelsn((listener_ctx *)e->obj);
        break;
    }
}
void _uev_qtn_drain(watcher_ctx *watcher, uint64_t now_ms) {
    qtn_entry *e;
    while (NULL != (e = (qtn_entry *)queue_peek(&watcher->qtn))) {
        if (now_ms - e->enter_ms < QTN_MS) {
            break;
        }
        _uev_qtn_release(watcher, e, 0);
        queue_pop(&watcher->qtn);
    }
}
void _uev_qtn_flush(watcher_ctx *watcher) {
    qtn_entry *e;
    while (NULL != (e = (qtn_entry *)queue_pop(&watcher->qtn))) {
        _uev_qtn_release(watcher, e, 1);
    }
    queue_free(&watcher->qtn);
}
// 根据id从arrlsn中查找并移除listener_ctx（加自旋锁保护）
static listener_ctx * _usk_get_listener(ev_ctx *ctx, uint64_t id) {
    listener_ctx *lsn = NULL;
    listener_ctx **tmp;
    spin_lock(&ctx->spin);
    uint32_t n = array_size(&ctx->arrlsn);
    for (uint32_t i = 0; i < n; i++) {
        tmp = (listener_ctx **)array_at(&ctx->arrlsn, i);
        if ((*tmp)->id == id) {
            lsn = *tmp;
            array_del_nomove(&ctx->arrlsn, i);
            break;
        }
    }
    spin_unlock(&ctx->spin);
    return lsn;
}
void ev_unlisten(ev_ctx *ctx, uint64_t id) {
    listener_ctx *lsn = _usk_get_listener(ctx, id);
    if (NULL == lsn) {
        return;
    }
    // 占位 +1：防止 for 循环期间 watcher 把 ref 减到 0 触发 FREE(lsn)，后续读 lsn->nlsn UAF。
    // 末尾减占位仲裁：watcher 全处理完则此处归零释放，否则由最后一个 _uev_remove_lsn 释放
    ATOMIC_ADD(&lsn->ref, 1);
    ATOMIC_SET(&lsn->remove, 1);
    for (int32_t i = 0; i < lsn->nlsn; i++) {
        _cmd_unlisten(&ctx->watcher[i], lsn);
    }
    // 占位减必须发 CMD 给 worker[0] 执行,不能主线程直接 FREE：worker 的 events[] 里可能还有
    // 指向 lsnsock 的项没遍历完,同步 FREE 就是跨线程 UAF；走 worker 则 FREE 跨过 QTN_MS 隔离期
    _cmd_lsn_unref(&ctx->watcher[0], lsn);
}
// fd 就地从 lsnsock 上取:这个字段归本 watcher 写(出错路径的 CLOSE_SOCK 会把它置 INVALID_SOCK),
// 由业务线程在 ev_unlisten 里读了再传进来的话，读到的可能是刚被关掉、fd 号又被新 accept 的连接
// 占用的旧值——那样只按"在不在元素表里"判就会误关那条无辜连接,而它既不入池也不入隔离队列、
// close 回调也不触发。在本线程读就没有这个时间差
void _uev_remove_lsn(watcher_ctx *watcher, listener_ctx *lsn) {
    lsnsock_ctx *curlsn = &lsn->lsnsock[watcher->index];
    SOCKET fd = curlsn->sock.fd;
    if (INVALID_SOCK != fd) {
        _evpub_sockel_remove(watcher, fd);
        _uev_drop_changes(watcher, &curlsn->sock);// 顺序理由见 _usk_detach
#ifdef MANUAL_REMOVE
        _uev_del_event(watcher, fd, &curlsn->sock.events, EVENT_READ, &curlsn->sock);
#endif
        CLOSE_SOCK(curlsn->sock.fd);
    }
    // lsnsock 释放前必须摘掉退避 tick，否则 tick 节点随 lsnsock 数组释放悬空
    if (NULL != curlsn->backoff_tick.cb) {
        _evpub_tick_remove(watcher, &curlsn->backoff_tick);
        curlsn->backoff_tick.cb = NULL;
    }
    // 仅清本 watcher 持有的 lsnsock ev_cb,让本批次 events[] 残留事件跳过本 lsnsock;
    // 跨 watcher 不写(避免与其他 watcher _uev_loop_event 读 events[k].udata->ev_cb 产生 race)
    curlsn->sock.ev_cb = NULL;
    // ref 归零入隔离队列: lsn 在 QTN_MS 隔离期内 lsnsock 数组内存仍活,
    _uev_qtn_freelsn(watcher, lsn);
}
// 初始化msghdr结构体（用于recvmsg/sendmsg的地址和iov绑定）
static inline void _usk_init_msghdr(struct msghdr *msg, netaddr_ctx *addr, IOV_TYPE *iov, uint32_t niov) {
    ZERO(msg, sizeof(struct msghdr));
    msg->msg_name = netaddr_addr(addr);
    msg->msg_namelen = netaddr_size(addr);
    msg->msg_iov = iov;
    msg->msg_iovlen = niov;
}
// UDP接收处理：循环 recvmsg 直到 EAGAIN，满足 EPOLLET 边缘触发 "读至无数据" 契约
// 单次 recvmsg 仅读一个 datagram；ET 模式下若 buffer 仍有 datagram 不会再触发 EVENT_READ，
// 必须本次唤醒就读光，否则后续 datagram 卡到 buffer 直到新边沿到达
static int32_t _usk_on_udp_rcb(watcher_ctx *watcher, udp_ctx *udp) {
    int32_t rtn;
    int32_t err;
    int32_t nerr = 0;
    netaddr_ctx addr;
    IOV_TYPE iov;
    struct msghdr msg;
    netaddr_empty(&addr);
    iov.IOV_PTR_FIELD = watcher->udp_rbuf;
    iov.IOV_LEN_FIELD = (IOV_LEN_TYPE)MAX_RECVFROM_SIZE;
    _usk_init_msghdr(&msg, &addr, &iov, 1);
    for (;;) {
        // recvmsg 会将 msg_namelen 改写为本次实际地址长度，下一次调用前须重置为缓冲最大值
        msg.msg_namelen = netaddr_size(&addr);
        rtn = (int32_t)recvmsg(udp->sock.fd, &msg, 0);
        if (rtn >= 0) {
            nerr = 0;// 读到 datagram 即证 fd 正常,失败计数按"连续"而非累计,免高流量下偶发失败攒满上限误关
            if (msg.msg_flags & MSG_TRUNC) {
                // datagram 超过 MAX_RECVFROM_SIZE 被截断：残缺数据不上抛，告警丢弃后继续收（不关 socket）
                LOG_WARN("UDP datagram truncated on fd %d (exceeds %d bytes), dropped.",
                         (int32_t)udp->sock.fd, MAX_RECVFROM_SIZE);
                continue;
            }
            // 0 字节是合法 UDP datagram 不视为对端关闭；由 _usk_call_recvfrom_cb 过滤不向上抛
            _usk_call_recvfrom_cb(watcher->ev, udp, watcher->udp_rbuf, &addr, (size_t)rtn);
            continue;
        }
        err = ERRNO;
        if (ERR_RW_RETRIABLE(err)) {
            rtn = ERR_OK;
            break;
        }
        if (EBADF == err
            || ENOTSOCK == err) {
            break;// fd 本身失效,rtn 保持负值让调用方关闭
        }
        // 与发送侧 _usk_udp_sendmsg_once / IOCP 侧 _olp_on_recvfrom_cb 一致：单包失败告警丢弃并
        // 继续排空,不因一个瞬时错误关掉承载所有对端的 UDP socket；但不消耗 datagram 的错误
        // (如 EINVAL)会原地打转,故连续失败超上限即认 fd 异常,保持 rtn 负值让调用方关闭
        LOG_WARN("UDP recvmsg dropped on fd %d: %s.", (int32_t)udp->sock.fd, ERRORSTR(err));
        if (++nerr >= UDP_RECV_MAX_ERRS) {
            break;
        }
    }
    if (ERR_OK == rtn) {
        rtn = _usk_keep_event(watcher, &udp->sock, EVENT_READ);
    }
    return rtn;
}
// 对单个 UDP payload 尝试一次 sendmsg；返回 ERR_OK 该包已处理完(发送成功，或遇到无害的单包错误已丢弃)；
// 返回 1 为 EAGAIN/EINTR，可重试，调用方需保留该包；返回 ERR_FAILED 为 EBADF/ENOTSOCK，fd 本身已失效
static inline int32_t _usk_udp_sendmsg_once(SOCKET fd, const void *data, size_t len, netaddr_ctx *addr) {
    IOV_TYPE iov;
    struct msghdr msg;
    iov.IOV_PTR_FIELD = (char *)data;
    iov.IOV_LEN_FIELD = (IOV_LEN_TYPE)len;
    _usk_init_msghdr(&msg, addr, &iov, 1);
    if (sendmsg(fd, &msg, 0) >= 0) {
        return ERR_OK;
    }
    int32_t err = ERRNO;
    if (ERR_RW_RETRIABLE(err)) {
        return 1;
    }
    if (EBADF == err || ENOTSOCK == err) {
        return ERR_FAILED;
    }
    LOG_WARN("UDP sendto dropped one datagram on fd %d: %s.", (int32_t)fd, ERRORSTR(err));
    return ERR_OK;
}
// UDP发送处理：循环发送队列中所有数据包，队列空后删除写事件
static int32_t _usk_on_udp_wcb(watcher_ctx *watcher, udp_ctx *udp) {
    sendto_ctx *buf;
    int32_t snd;
    while (NULL != (buf = queue_peek(&udp->buf_s))) {
        snd = _usk_udp_sendmsg_once(udp->sock.fd, buf->data, buf->len, &buf->addr);
        if (1 == snd) {
            break;
        }
        udp->wb_size -= buf->len;
        FREE(buf->data);
        queue_pop(&udp->buf_s);
        if (ERR_FAILED == snd) {
            return ERR_FAILED;
        }
    }
    if (0 == queue_size(&udp->buf_s)) {
        if (BIT_CHECK(udp->sock.events, EVENT_WRITE)) {// 直发快路径无EVENT_WRITE
            _uev_del_event(watcher, udp->sock.fd, &udp->sock.events, EVENT_WRITE, &udp->sock);
        }
        return ERR_OK;
    }
    return _usk_keep_event(watcher, &udp->sock, EVENT_WRITE);
}
// UDP读写事件统一回调：STATUS_ERROR时入隔离队列延后释放，否则分别处理读写事件
static void _usk_on_udp_rw(watcher_ctx *watcher, sock_ctx *skctx, int32_t ev) {
    udp_ctx *udp = UPCAST(skctx, udp_ctx, sock);
    if (BIT_CHECK(udp->status, STATUS_ERROR)) {
        _usk_close_udp(watcher, udp);
        return;
    }
    int32_t rtn = ERR_OK;
    if (BIT_CHECK(ev, EVENT_READ)) {
        rtn = _usk_on_udp_rcb(watcher, udp);
    }
    if (ERR_OK == rtn
        && BIT_CHECK(ev, EVENT_WRITE)) {
        rtn = _usk_on_udp_wcb(watcher, udp);
    }
    if (ERR_OK != rtn) {
        BIT_SET(udp->status, STATUS_ERROR);
        _usk_close_udp(watcher, udp);
    }
}
void _uev_add_bufs_sendto(watcher_ctx *watcher, sock_ctx *skctx, sendto_ctx *buf, int32_t tried) {
    udp_ctx *udp = UPCAST(skctx, udp_ctx, sock);
    // ERROR 期拒收:否则绕过 _usk_on_udp_rw 的 STATUS_ERROR 检查直接触达 _usk_on_udp_wcb
    if (BIT_CHECK(udp->status, STATUS_ERROR)
        || !_evpub_sendqu_check_udp(&udp->buf_s, skctx->fd)) {
        FREE(buf->data);
        return;
    }
    int32_t was_empty = (0 == queue_size(&udp->buf_s));
    udp->wb_size += buf->len;
    _evpub_sendqu_tda(&udp->tda, udp->wb_size, skctx->fd, 0);
    queue_push(&udp->buf_s, buf);
    // 队列本来就非空：EVENT_WRITE 必然已经注册（否则数据早发不出去），无需重复处理
    if (!was_empty) {
        return;
    }
    if (tried) {
        // 调用方入队前已尝试过一次发送(EAGAIN)，此刻必然复现，跳过重试，仅确保写事件已注册
        if (ERR_OK != _usk_keep_event(watcher, skctx, EVENT_WRITE)) {
            _uev_disconnect(watcher, skctx);
        }
        return;
    }
    // 未尝试过：立即尝试发送，避免多等一轮事件循环才发出；
    // 发送未完成时，_usk_on_udp_wcb 内部的 _usk_keep_event 会负责注册/保活 EVENT_WRITE
    if (ERR_OK != _usk_on_udp_wcb(watcher, udp)) {
        BIT_SET(udp->status, STATUS_ERROR);
        _usk_close_udp(watcher, udp);
    }
}
// 尝试直接发送 UDP 数据；返回 0 表示已处理完(发送成功、单包丢弃或致命错误已断开)，调用方无需任何后续操作；
// 返回 1 表示需要调用方继续(发送队列已有积压或 EAGAIN)，自行 MALLOC+memcpy 后以 tried=1 转入 _uev_add_bufs_sendto 排队
int32_t _uev_try_sendto(watcher_ctx *watcher, sock_ctx *skctx, const void *data, size_t len, netaddr_ctx *addr) {
    udp_ctx *udp = UPCAST(skctx, udp_ctx, sock);
    // 已在 error 关闭流程：fd 已知致命，无需再发起 sendmsg 尝试，也不需要调用方转入排队
    if (BIT_CHECK(udp->status, STATUS_ERROR)) {
        return 0;
    }
    if (0 != queue_size(&udp->buf_s)) {
        return 1;
    }
    int32_t snd = _usk_udp_sendmsg_once(skctx->fd, data, len, addr);
    if (ERR_OK == snd) {
        return 0;
    }
    if (ERR_FAILED == snd) {
        _uev_disconnect(watcher, skctx);
        return 0;
    }
    return 1;
}
// 分配并初始化UDP上下文
static sock_ctx *_usk_new_udp(SOCKET fd, cbs_ctx *cbs, ud_cxt *ud) {
    udp_ctx *udp;
    MALLOC(udp, sizeof(udp_ctx));
    udp->sock.ev_cb = _usk_on_udp_rw;
    udp->sock.type = SOCK_DGRAM;
    udp->sock.fd = fd;
    udp->sock.events = 0;
#ifdef COMMIT_NCHANGES
    udp->sock.chg_round = 0;
#endif
    udp->status = STATUS_NONE;
    udp->skid = createid();
    udp->cbs = *cbs;
    COPY_UD(udp->ud, ud);
    queue_init(&udp->buf_s, sizeof(sendto_ctx), INIT_SENDBUF_LEN);
    udp->wb_size = 0;
    tda_init(&udp->tda, WB_WARN_INIT_SIZE);
    return &udp->sock;
}
void _uev_free_udp(sock_ctx *skctx) {
    udp_ctx *udp = UPCAST(skctx, udp_ctx, sock);
    CLOSE_SOCK(udp->sock.fd);
    _evpub_sendto_clear(&udp->buf_s);
    queue_free(&udp->buf_s);
    UD_FREE(udp->cbs.ud_free, &udp->ud);
    FREE(udp);
}
int32_t ev_udp(ev_ctx *ctx, const char *ip, const uint16_t port, cbs_ctx *cbs, ud_cxt *ud,
    SOCKET *fd, uint64_t *skid) {
    netaddr_ctx addr;
    if (ERR_OK != _evpub_sock_launch_check(ctx, ip, port, cbs, ud, 1, &addr)) {
        return ERR_FAILED;
    }
    *fd = _evpub_udp(&addr);
    if (INVALID_SOCK == *fd) {
        LOG_ERROR("udp %s:%d error.", ip, port);
        UD_FREE(cbs->ud_free, ud);
        return ERR_FAILED;
    }
    sock_ctx *skctx = _usk_new_udp(*fd, cbs, ud);
    *skid = UPCAST(skctx, udp_ctx, sock)->skid;
    _cmd_add(GET_PTR(ctx->watcher, ctx->nthreads, *fd), skctx);
    return ERR_OK;
}
void _uev_add_fd_inloop(watcher_ctx *watcher, sock_ctx *skctx) {
    _evpub_sockel_add(watcher, skctx);
    if (ERR_OK != _uev_add_event(watcher, skctx->fd, &skctx->events, EVENT_READ, skctx)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        _evpub_sockel_remove(watcher, skctx->fd);
        if (SOCK_STREAM == skctx->type) {
            pool_push(&watcher->pool, skctx, 0);
        } else {
            _usk_call_udp_close_cb(watcher->ev, UPCAST(skctx, udp_ctx, sock));
            _uev_free_udp(skctx);
        }
        return;
    }
}

#endif//EV_IOCP
