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
    IOV_TYPE buf_r;             // 接收缓冲区描述符（指向buf）
    struct msghdr msg;          // recvmsg使用的消息头（含地址和iov）
    queue_ctx buf_s;            // 发送队列
    netaddr_ctx addr;           // 接收到的对端地址（recvmsg填充）
    ud_cxt ud;                  // 用户数据
    char buf[MAX_RECVFROM_SIZE]; // 固定接收缓冲区
}udp_ctx;

static void _usk_on_rw_cb(watcher_ctx *watcher, sock_ctx *skctx, int32_t ev); // 前向声明：TCP读写事件回调
static inline void _usk_close_tcp(watcher_ctx *watcher, tcp_ctx *tcp); // 前向声明：TCP连接关闭

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
void _uev_disconnect(watcher_ctx *watcher, sock_ctx *skctx, int32_t immed) {
    if (SOCK_STREAM == skctx->type) {
        tcp_ctx *tcp = UPCAST(skctx, tcp_ctx, sock);
        if (BIT_CHECK(tcp->status, STATUS_ERROR)) {
            return;
        }
        if (BIT_CHECK(tcp->status, STATUS_GRACEFUL_CLOSE)) {
            if (0 == immed) {
                return;
            }
            // graceful 中升级到 immed: 清 GRACEFUL_CLOSE 走 ERROR 路径
            BIT_REMOVE(tcp->status, STATUS_GRACEFUL_CLOSE);
        }
        // graceful 但已无待发数据 → 退化为立即关
        if (0 == immed
            && 0 == queue_size(&tcp->buf_s)) {
            immed = 1;
        }
        if (0 != immed) {
            BIT_SET(tcp->status, STATUS_ERROR);
        } else {
            BIT_SET(tcp->status, STATUS_GRACEFUL_CLOSE);
        }
        _uev_sk_shutdown(skctx);
        // 注册 EVENT_WRITE：immed=1 触发 _usk_on_rw_cb 入口 STATUS_ERROR 分支立即关
        //                  immed=0 触发 STATUS_GRACEFUL_CLOSE 分支发完 buf_s 后关
        if (!BIT_CHECK(tcp->sock.events, EVENT_WRITE)) {
            if (ERR_OK != _uev_add_event(watcher, tcp->sock.fd, &tcp->sock.events, EVENT_WRITE, &tcp->sock)) {
                _usk_close_tcp(watcher, tcp);
            }
        }
    } else {
        // UDP datagram 无序无连接,graceful 无意义,始终走立即关分支
        udp_ctx *udp = UPCAST(skctx, udp_ctx, sock);
        if (BIT_CHECK(udp->status, STATUS_ERROR)) {
            return;
        }
        BIT_SET(udp->status, STATUS_ERROR);
        if (!BIT_CHECK(skctx->events, EVENT_WRITE)) {
            if (ERR_OK != _uev_add_event(watcher, skctx->fd, &skctx->events, EVENT_WRITE, skctx)) {
#ifdef MANUAL_REMOVE
                _uev_del_event(watcher, skctx->fd, &skctx->events, skctx->events, skctx);
#endif
                _evpub_sockel_remove(watcher, skctx->fd);
                CLOSE_SOCK(skctx->fd);
                skctx->ev_cb = NULL;
                _uev_qtn_push(watcher, skctx, QTN_UDP);
            }
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
#if WITH_SSL
        return tcp->cbs.exch_cb(ev, tcp->sock.fd, tcp->skid, BIT_CHECK(tcp->status, STATUS_CLIENT), &tcp->ud, tcp->ssl);
#else
        return tcp->cbs.exch_cb(ev, tcp->sock.fd, tcp->skid, BIT_CHECK(tcp->status, STATUS_CLIENT), &tcp->ud, NULL);
#endif
    }
    return ERR_OK;
}
// 调用数据接收回调（nread > 0 才触发）
static inline void _usk_call_recv_cb(ev_ctx *ev, tcp_ctx *tcp, size_t nread) {
    if (nread > 0) {
        tcp->cbs.r_cb(ev, tcp->sock.fd, tcp->skid, BIT_CHECK(tcp->status, STATUS_CLIENT), &tcp->buf_r, nread, &tcp->ud);
    }
}
// 调用发送完成回调（nsend > 0 且有s_cb 才触发）
static inline void _usk_call_send_cb(ev_ctx *ev, tcp_ctx *tcp, size_t nsend) {
    if (NULL != tcp->cbs.s_cb
        && nsend > 0) {
        tcp->cbs.s_cb(ev, tcp->sock.fd, tcp->skid, BIT_CHECK(tcp->status, STATUS_CLIENT), nsend, &tcp->ud);
    }
}
// 调用连接关闭回调
static inline void _usk_call_close_cb(ev_ctx *ev, tcp_ctx *tcp) {
    if (NULL != tcp->cbs.c_cb) {
        tcp->cbs.c_cb(ev, tcp->sock.fd, tcp->skid, BIT_CHECK(tcp->status, STATUS_CLIENT), &tcp->ud);
    }
}
// 调用UDP接收回调；0 字节 datagram 由本函数过滤不向上抛（_usk_on_udp_rcb 仍不视为 EOF，
// 继续 recvmsg 循环），避免上层处理空 payload 的特殊路径
static inline void _usk_call_recvfrom_cb(ev_ctx *ev, udp_ctx *udp, size_t nread) {
    if (nread > 0) {
        udp->cbs.rf_cb(ev, udp->sock.fd, udp->skid, udp->buf_r.IOV_PTR_FIELD, nread, &udp->addr, &udp->ud);
    }
}
#if WITH_SSL
// 在事件循环内启动SSL握手：设置SSL fd并根据角色调用connect/accept
static int32_t _usk_ssl_exchange(watcher_ctx *watcher, tcp_ctx *tcp, struct evssl_ctx *evssl) {
    tcp->ssl = evssl_setfd(evssl, tcp->sock.fd);
    if (NULL == tcp->ssl) {
        return ERR_FAILED;
    }
    if (BIT_CHECK(tcp->status, STATUS_CLIENT)) {
        switch (evssl_tryconn(tcp->ssl)) {
        case ERR_FAILED://错误
            return ERR_FAILED;
        case ERR_OK://完成
            return _usk_call_ssl_exchanged_cb(watcher->ev, tcp);
        case 1://等待读就绪（WANT_READ）
            BIT_SET(tcp->status, STATUS_AUTHSSL);
            break;
        case 2://等待写就绪（WANT_WRITE）：注册写事件，写就绪后由 _usk_on_rw_cb 重试
            BIT_SET(tcp->status, STATUS_AUTHSSL);
            BIT_SET(tcp->status, STATUS_SSLWANTWRITE);
            if (ERR_OK != _uev_add_event(watcher, tcp->sock.fd, &tcp->sock.events, EVENT_WRITE, &tcp->sock)) {
                return ERR_FAILED;
            }
            break;
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
    if (NULL != tcp->ssl) {
        LOG_WARN("ssl already in use.");
        return;
    }
    if (BIT_CHECK(tcp->status, STATUS_SSLEXCHANGE)) {
        LOG_WARN("repeat request ssl exchange.");
        return;
    }
    if (BIT_CHECK(tcp->status, STATUS_ERROR)) {
        return;
    }
    if (client) {
        BIT_SET(tcp->status, STATUS_CLIENT);
    } else {
        BIT_REMOVE(tcp->status, STATUS_CLIENT);
    }
    if (BIT_CHECK(skctx->events, EVENT_WRITE)) {
        tcp->evssl = evssl;
        BIT_SET(tcp->status, STATUS_SSLEXCHANGE);
    } else {
        if (ERR_OK != _usk_ssl_exchange(watcher, tcp, evssl)) {
            _uev_disconnect(watcher, skctx, 1);
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
static int32_t _usk_tcp_recv(watcher_ctx *watcher, tcp_ctx *tcp) {
    size_t nread;
#if WITH_SSL
    int32_t rtn = buffer_from_sock(&tcp->buf_r, tcp->sock.fd, &nread, _evpub_sock_read, tcp->ssl);
#else
    int32_t rtn = buffer_from_sock(&tcp->buf_r, tcp->sock.fd, &nread, _evpub_sock_read, NULL);
#endif
    _usk_call_recv_cb(watcher->ev, tcp, nread);
#ifdef MANUAL_ADD
    if (ERR_OK == rtn) {
        rtn = _uev_add_event(watcher, tcp->sock.fd, &tcp->sock.events, EVENT_READ, &tcp->sock);
    }
#endif
    return rtn;
}
// 发送队列中的数据，队列空后删除写事件（可选SSL升级），MANUAL_ADD时重注册写事件
static int32_t _usk_tcp_send(watcher_ctx *watcher, tcp_ctx *tcp) {
    size_t nsend;
#if WITH_SSL
    int32_t rtn = _evpub_sock_send(tcp->sock.fd, &tcp->buf_s, &nsend, tcp->ssl);
#else
    int32_t rtn = _evpub_sock_send(tcp->sock.fd, &tcp->buf_s, &nsend, NULL);
#endif
    tcp->wb_size -= nsend;
    _usk_call_send_cb(watcher->ev, tcp, nsend);
    if (ERR_OK != rtn) {
        return rtn;
    }
    if (0 == queue_size(&tcp->buf_s)) {
        _uev_del_event(watcher, tcp->sock.fd, &tcp->sock.events, EVENT_WRITE, &tcp->sock);
#if WITH_SSL
        if (BIT_CHECK(tcp->status, STATUS_SSLEXCHANGE)) {
            BIT_REMOVE(tcp->status, STATUS_SSLEXCHANGE);
            if (ERR_OK != _usk_ssl_exchange(watcher, tcp, tcp->evssl)) {
                LOG_ERROR("ssl exchange error.");
                return ERR_FAILED;
            }
        }
#endif
        return ERR_OK;
    }
#ifdef MANUAL_ADD
    rtn = _uev_add_event(watcher, tcp->sock.fd, &tcp->sock.events, EVENT_WRITE, &tcp->sock);
#endif
    return rtn;
}
// 关闭TCP连接：触发关闭回调，从hashmap移除，入隔离队列暂存 QTN_MS 后归 pool
static inline void _usk_close_tcp(watcher_ctx *watcher, tcp_ctx *tcp) {
    _usk_call_close_cb(watcher->ev, tcp);
#ifdef MANUAL_REMOVE
    _uev_del_event(watcher, tcp->sock.fd, &tcp->sock.events, tcp->sock.events, &tcp->sock);
#endif
    _evpub_sockel_remove(watcher, tcp->sock.fd);
    // 立即 close fd 把 fd 还给 OS（不延迟到 _evpub_sk_clear）；CLOSE_SOCK 设 fd=INVALID_SOCK,
    // 后续 _evpub_sk_clear 再次 CLOSE_SOCK 内宏判 INVALID_SOCK 跳过（幂等）
    CLOSE_SOCK(tcp->sock.fd);
    // 清空回调防止同批次后续 events 进入 skctx 触发二次 _usk_close_tcp；
    // 隔离期内本 watcher events 跨轮 stale 也读 ev_cb=NULL 跳过；
    // pool_pop 经 _evpub_sk_reset 恢复为 _usk_on_rw_cb（usock.c:_evpub_sk_reset 内）
    tcp->sock.ev_cb = NULL;
    _uev_qtn_push(watcher, &tcp->sock, QTN_TCP);
}
// TCP读写事件统一回调：处理SSL握手、读、写，任意失败则关闭连接
static void _usk_on_rw_cb(watcher_ctx *watcher, sock_ctx *skctx, int32_t ev) {
    int32_t rtn;
    int32_t evread = BIT_CHECK(ev, EVENT_READ);
    tcp_ctx *tcp = UPCAST(skctx, tcp_ctx, sock);
#if WITH_SSL
    /* SSL握手驱动条件：
     *   读就绪（evread）         — WANT_READ 后新数据到达，正常握手路径
     *   写就绪（STATUS_SSLWANTWRITE） — WANT_WRITE 后 socket 发送缓冲区可用，握手恢复 */
    if (NULL != tcp->ssl
        && (evread 
            || (BIT_CHECK(ev, EVENT_WRITE) && (BIT_CHECK(tcp->status, STATUS_SSLWANTWRITE))))
        && BIT_CHECK(tcp->status, STATUS_AUTHSSL)
        && !BIT_CHECK(tcp->status, STATUS_ERROR)) {
        if (BIT_CHECK(tcp->status, STATUS_CLIENT)) {
            rtn = evssl_tryconn(tcp->ssl);
        } else {
            rtn = evssl_tryacpt(tcp->ssl);
        }
        switch (rtn) {
        case ERR_FAILED://错误
            if (BIT_CHECK(tcp->status, STATUS_SSLWANTWRITE)) {
                BIT_REMOVE(tcp->status, STATUS_SSLWANTWRITE);
                _uev_del_event(watcher, tcp->sock.fd, &tcp->sock.events, EVENT_WRITE, &tcp->sock);
            }
            BIT_SET(tcp->status, STATUS_ERROR);
            break;
        case ERR_OK://完成：清除写事件注册（若有），然后通知上层
            if (BIT_CHECK(tcp->status, STATUS_SSLWANTWRITE)) {
                BIT_REMOVE(tcp->status, STATUS_SSLWANTWRITE);
                _uev_del_event(watcher, tcp->sock.fd, &tcp->sock.events, EVENT_WRITE, &tcp->sock);
            }
            if (ERR_OK == _usk_call_ssl_exchanged_cb(watcher->ev, tcp)) {
                BIT_REMOVE(tcp->status, STATUS_AUTHSSL);
#ifdef READV_EINVAL
                return;
#else
                break;
#endif
            }
            BIT_SET(tcp->status, STATUS_ERROR);
            break;
        case 1://等待读就绪（WANT_READ）：若之前在等写，撤销写事件
            if (BIT_CHECK(tcp->status, STATUS_SSLWANTWRITE)) {
                BIT_REMOVE(tcp->status, STATUS_SSLWANTWRITE);
                _uev_del_event(watcher, tcp->sock.fd, &tcp->sock.events, EVENT_WRITE, &tcp->sock);
            }
#ifdef MANUAL_ADD
            if (ERR_OK != _uev_add_event(watcher, tcp->sock.fd, &tcp->sock.events, EVENT_READ, &tcp->sock)) {
                BIT_SET(tcp->status, STATUS_ERROR);
                break;
            } else {
                return;
            }
#else
            return;
#endif
        case 2://等待写就绪（WANT_WRITE）：注册写事件，写就绪后再次进入此分支重试
            BIT_SET(tcp->status, STATUS_SSLWANTWRITE);
            if (ERR_OK != _uev_add_event(watcher, tcp->sock.fd, &tcp->sock.events, EVENT_WRITE, &tcp->sock)) {
                BIT_SET(tcp->status, STATUS_ERROR);
                break;
            }
            return;
        }//switch
    }
#endif
    if (BIT_CHECK(tcp->status, STATUS_ERROR)) {
        _usk_close_tcp(watcher, tcp);
        return;
    }
    rtn = ERR_OK;
    int32_t graceful = BIT_CHECK(tcp->status, STATUS_GRACEFUL_CLOSE);
    if (evread && !graceful) {
        rtn = _usk_tcp_recv(watcher, tcp);
    }
    if (ERR_OK == rtn
        && BIT_CHECK(ev, EVENT_WRITE)) {
        rtn = _usk_tcp_send(watcher, tcp);
    }
    if (ERR_OK != rtn) {
        // graceful 中转 error 路径:清 GRACEFUL_CLOSE 保持状态互斥
        if (graceful) {
            BIT_REMOVE(tcp->status, STATUS_GRACEFUL_CLOSE);
        }
        BIT_SET(tcp->status, STATUS_ERROR);
        _usk_close_tcp(watcher, tcp);
        return;
    }
    if (graceful
        && 0 == queue_size(&tcp->buf_s)) {
        BIT_REMOVE(tcp->status, STATUS_GRACEFUL_CLOSE);
        BIT_SET(tcp->status, STATUS_ERROR);
        _usk_close_tcp(watcher, tcp);
    }
}
void _uev_add_write_inloop(watcher_ctx *watcher, sock_ctx *skctx, off_buf_ctx *buf) {
    if (SOCK_STREAM == skctx->type) {
        tcp_ctx *tcp = UPCAST(skctx, tcp_ctx, sock);
        // 已在 graceful/error 关闭流程：拒收新数据
        if (BIT_CHECK(tcp->status, STATUS_GRACEFUL_CLOSE)
            || BIT_CHECK(tcp->status, STATUS_ERROR)) {
            _evpub_off_buf_release(buf);
            return;
        }
#if WITH_SSL
        // SSL 握手期间禁止发送业务数据，否则会中断握手；命中即丢数据并立即关连接
        if (BIT_CHECK(tcp->status, STATUS_AUTHSSL)
            || BIT_CHECK(tcp->status, STATUS_SSLEXCHANGE)) {
            LOG_WARN("ev_send during SSL handshake on fd %d, disconnect.", (int32_t)skctx->fd);
            _evpub_off_buf_release(buf);
            _uev_disconnect(watcher, skctx, 1);
            return;
        }
#endif
        // TCP 慢消费者保护：发送队列超阈值丢数据并 disconnect，避免业务无脑写打爆内存
        if (0 != MAX_SENDQ_CNT
            && queue_size(&tcp->buf_s) >= MAX_SENDQ_CNT) {
            LOG_WARN("TCP send queue overflow on fd %d (>= %d), disconnect.",
                     (int32_t)skctx->fd, MAX_SENDQ_CNT);
            _evpub_off_buf_release(buf);
            _uev_disconnect(watcher, skctx, 1);
            return;
        }
        tcp->wb_size += buf->lens;
        if (tda_check(&tcp->tda, tcp->wb_size)) {
            LOG_WARN("TCP send buf growing on fd %d: %zu bytes.",
                     (int32_t)skctx->fd, tcp->wb_size);
        }
        queue_push(&tcp->buf_s, buf);
    } else {
        udp_ctx *udp = UPCAST(skctx, udp_ctx, sock);
        // UDP 队列超阈值丢 datagram 不断 fd
        if (0 != MAX_SENDQ_CNT
            && queue_size(&udp->buf_s) >= MAX_SENDQ_CNT) {
            LOG_WARN("UDP send queue overflow on fd %d (>= %d), drop datagram.",
                     (int32_t)skctx->fd, MAX_SENDQ_CNT);
            _evpub_off_buf_release(buf);
            return;
        }
        udp->wb_size += buf->lens;
        if (tda_check(&udp->tda, udp->wb_size)) {
            LOG_WARN("UDP send buf growing on fd %d: %zu bytes.",
                     (int32_t)skctx->fd, udp->wb_size);
        }
        queue_push(&udp->buf_s, buf);
    }
    if (!BIT_CHECK(skctx->events, EVENT_WRITE)) {
        if (ERR_OK != _uev_add_event(watcher, skctx->fd, &skctx->events, EVENT_WRITE, skctx)) {
            _uev_disconnect(watcher, skctx, 1);
        }
    }
}
// connect完成事件回调：检查连接结果，切换为读写回调，触发conn回调
static void _usk_on_connect_cb(watcher_ctx *watcher, sock_ctx *skctx, int32_t ev) {
    (void)ev;
    tcp_ctx *tcp = UPCAST(skctx, tcp_ctx, sock);
    tcp->sock.ev_cb = _usk_on_rw_cb;
    _uev_del_event(watcher, tcp->sock.fd, &tcp->sock.events, tcp->sock.events, skctx);
    if (BIT_CHECK(tcp->status, STATUS_ERROR)
        || ERR_OK != sock_checkconn(tcp->sock.fd)
        || ERR_OK != _uev_add_event(watcher, tcp->sock.fd, &tcp->sock.events, EVENT_READ, &tcp->sock)) {
        _usk_call_conn_cb(watcher->ev, tcp, ERR_FAILED);
        _evpub_sockel_remove(watcher, tcp->sock.fd);
        pool_push(&watcher->pool, &tcp->sock);
        return;
    }
    if (ERR_OK != _usk_call_conn_cb(watcher->ev, tcp, ERR_OK)) {
        _uev_disconnect(watcher, skctx, 1);
        return;
    }
#if WITH_SSL
    if (NULL != tcp->evssl) {
        if (ERR_OK != _usk_ssl_exchange(watcher, tcp, tcp->evssl)) {
            _uev_disconnect(watcher, skctx, 1);
            return;
        }
    }
#endif
}
int32_t ev_connect(ev_ctx *ctx, struct evssl_ctx *evssl, const char *ip, const uint16_t port, cbs_ctx *cbs, ud_cxt *ud,
    SOCKET *fd, uint64_t *skid) {
    if (NULL == cbs || NULL == cbs->r_cb) {
        if (NULL != cbs) {
            UD_FREE(cbs->ud_free, ud);
        }
        return ERR_FAILED;
    }
    netaddr_ctx addr;
    if (ERR_OK != netaddr_set(&addr, ip, port)) {
        LOG_ERROR("netaddr_set %s:%d, %s", ip, port, ERRORSTR(ERRNO));
        UD_FREE(cbs->ud_free, ud);
        return ERR_FAILED;
    }
    *fd = _evpub_create_sock(SOCK_STREAM, netaddr_family(&addr));
    if (INVALID_SOCK == *fd) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        UD_FREE(cbs->ud_free, ud);
        return ERR_FAILED;
    }
    sock_reuseaddr(*fd);
    _evpub_nodelay_nonblock(*fd);
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
        skctx->ev_cb = _usk_on_rw_cb;
        _evpub_sockel_remove(watcher, skctx->fd);
        pool_push(&watcher->pool, skctx);
    }
}
// 监听socket可读事件回调：循环accept新连接并分发给对应watcher
static void _usk_on_accept_cb(watcher_ctx *watcher, sock_ctx *skctx, int32_t ev) {
    lsnsock_ctx *acpt = UPCAST(skctx, lsnsock_ctx, sock);
    SOCKET fd;
    watcher_ctx *to;
    int32_t err;
    for (;;) {
        fd = accept(acpt->sock.fd, NULL, NULL);
        if (INVALID_SOCK == fd) {
            err = ERRNO;
            if (EINTR == err || ECONNABORTED == err) {
                continue;
            }
            break;
        }
        if (ERR_OK != _evpub_nodelay_nonblock(fd)
            || ERR_OK != sock_keepalive(fd, KEEPALIVE_TIME, KEEPALIVE_INTERVAL)) {
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
#ifdef MANUAL_ADD
    if (ERR_OK != _uev_add_event(watcher, acpt->sock.fd, &acpt->sock.events, EVENT_READ, &acpt->sock)) {
        _evpub_sockel_remove(watcher, acpt->sock.fd);
        LOG_ERROR("%s", ERRORSTR(ERRNO));
    }
#else
    (void)ev;
#endif
}
void _uev_add_acpfd_inloop(watcher_ctx *watcher, SOCKET fd, listener_ctx *lsn) {
    skpool_args skargs = { fd, &lsn->cbs, &lsn->ud };
    sock_ctx *skctx = pool_pop(&watcher->pool, &skargs);
    tcp_ctx *tcp = UPCAST(skctx, tcp_ctx, sock);
    _evpub_sockel_add(watcher, skctx);
    /* _uev_add_event(READ) 必须在 _usk_ssl_exchange 之前：EPOLLET 的 ADD-time 语义保证若
     * ClientHello 已在 socket buffer，下一次 epoll_wait 仍会合成 EPOLLIN 通知。
     * 整个函数在单一 watcher 线程内运行完毕（STATUS_AUTHSSL 已设置）后才进入
     * 下一次 epoll_wait，_usk_on_rw_cb 届时调用 evssl_tryacpt 驱动握手。*/
    if (ERR_OK != _uev_add_event(watcher, fd, &skctx->events, EVENT_READ, skctx)) {
        _evpub_sockel_remove(watcher, fd);
        pool_push(&watcher->pool, skctx);
        return;
    }
    if (ERR_OK != _usk_call_acp_cb(watcher->ev, tcp)) {
        _uev_disconnect(watcher, skctx, 1);
        return;
    }
#if WITH_SSL
    if (NULL != lsn->evssl) {
        if (ERR_OK != _usk_ssl_exchange(watcher, tcp, lsn->evssl)) {
            _uev_disconnect(watcher, skctx, 1);
            return;
        }
    }
#endif
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
    if (NULL == cbs || NULL == cbs->r_cb) {
        if (NULL != cbs) {
            UD_FREE(cbs->ud_free, ud);
        }
        return ERR_FAILED;
    }
    netaddr_ctx addr;
    if (ERR_OK != netaddr_set(&addr, ip, port)) {
        LOG_ERROR("netaddr_set %s:%d, %s", ip, port, ERRORSTR(ERRNO));
        UD_FREE(cbs->ud_free, ud);
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
    MALLOC(lsn->lsnsock, sizeof(lsnsock_ctx) * lsn->nlsn);
    int32_t i;
    lsnsock_ctx *lsnsock;
    for (i = 0; i < lsn->nlsn; i++) {
        lsnsock = &lsn->lsnsock[i];
        lsnsock->lsn = lsn;
        lsnsock->sock.type = 0;
        lsnsock->sock.events = 0;
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
    spin_lock(&ctx->spin);
    array_push_back(&ctx->arrlsn, &lsn);
    spin_unlock(&ctx->spin);
    for (i = 0; i < lsn->nlsn; i++) {
        _cmd_listen(&ctx->watcher[i], &lsn->lsnsock[i].sock);
    }
    SET_PTR(id, lsn->id);
    return ERR_OK;
}
void _uev_add_lsn_inloop(watcher_ctx *watcher, sock_ctx *skctx) {
    _evpub_sockel_add(watcher, skctx);
    if (ERR_OK != _uev_add_event(watcher, skctx->fd, &skctx->events, EVENT_READ, skctx)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        _evpub_sockel_remove(watcher, skctx->fd);
    }
}
void _uev_freelsn(listener_ctx *lsn) {
    if (0 == ATOMIC_GET(&lsn->remove)) {
        _usk_close_lsnsock(lsn, lsn->nlsn);
    }
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
    e.enter_ms = timer_cur_ms(&watcher->tm_qtn);
    e.type = type;
    queue_push(&watcher->qtn, &e);
}
void _uev_qtn_freelsn(watcher_ctx *watcher, listener_ctx *lsn) {
    if (1 == ATOMIC_ADD(&lsn->ref, -1)) {
        _uev_qtn_push(watcher, lsn, QTN_LSN);
    }
}
void _uev_qtn_drain(watcher_ctx *watcher, uint64_t now_ms) {
    qtn_entry *e;
    while (NULL != (e = (qtn_entry *)queue_peek(&watcher->qtn))) {
        if (now_ms - e->enter_ms < QTN_MS) {
            break;
        }
        switch (e->type) {
        case QTN_TCP:
            pool_push(&watcher->pool, (sock_ctx *)e->obj);
            break;
        case QTN_UDP:
            _uev_free_udp((sock_ctx *)e->obj);
            break;
        case QTN_LSN:
            _uev_freelsn((listener_ctx *)e->obj);
            break;
        }
        queue_pop(&watcher->qtn);
    }
}
void _uev_qtn_flush(watcher_ctx *watcher) {
    qtn_entry *e;
    while (NULL != (e = (qtn_entry *)queue_pop(&watcher->qtn))) {
        switch (e->type) {
        case QTN_TCP:
            _evpub_sk_free((sock_ctx *)e->obj);
            break;
        case QTN_UDP:
            _uev_free_udp((sock_ctx *)e->obj);
            break;
        case QTN_LSN:
            _uev_freelsn((listener_ctx *)e->obj);
            break;
        }
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
    // 占位 +1：防止 for 循环期间所有 watcher 处理完 CMD_UNLSN 把 ref 减到 0
    // 触发 _uev_freelsn → FREE(lsn) → 后续读 lsn->nlsn / lsn->lsnsock[i].sock.fd UAF。
    // 末尾减占位仲裁：watcher 全部处理完则此处归零触发 _uev_freelsn，否则由最后一个
    // _uev_remove_lsn 触发。与 IOCP 路径 _acceptex 占位 + ev_unlisten 末尾减占位风格一致
    ATOMIC_ADD(&lsn->ref, 1);
    ATOMIC_SET(&lsn->remove, 1);
    for (int32_t i = 0; i < lsn->nlsn; i++) {
        _cmd_unlisten(&ctx->watcher[i], lsn->lsnsock[i].sock.fd, lsn);
    }
    // 占位减发 CMD 给 worker[0] 在 _uev_cmd_loop 内执行 (_on_cmd_lsn_unref 内归 0 时
    // 入 watcher->qtn 隔离队列); 主线程直接 _uev_try_freelsn 在 ref 归 0 时立即 FREE,
    // 会跟 worker 在 _uev_loop_event 内迭代 events[] 数组的跨线程 UAF 竞态: worker 已减完
    // 自己 CMD_UNLSN ref 但 events[k>i] 仍可能指向 lsnsock, 主线程同步 FREE 后 worker
    // 读 skctx->ev_cb UAF。走 worker 上下文则 FREE 跨过 QTN_MS 隔离期, events 已遍历完
    _cmd_lsn_unref(&ctx->watcher[0], lsn);
}
void _uev_remove_lsn(watcher_ctx *watcher, SOCKET fd, listener_ctx *lsn) {
    sock_ctx **skctx = _evpub_sockel_remove(watcher, fd);
#ifdef MANUAL_REMOVE
    if (NULL != skctx) {
        lsnsock_ctx *acpt = UPCAST(*skctx, lsnsock_ctx, sock);
        _uev_del_event(watcher, fd, &acpt->sock.events, EVENT_READ, &acpt->sock);
    }
#else
    (void)skctx;
#endif
    SOCK_CLOSE(fd);
    // 仅清本 watcher 持有的 lsnsock ev_cb,让本批次 events[] 残留事件跳过本 lsnsock;
    // 跨 watcher 不写(避免与其他 watcher _uev_loop_event 读 events[k].udata->ev_cb 产生 race)
    lsn->lsnsock[watcher->index].sock.ev_cb = NULL;
    // ref 归零入隔离队列: lsn 在 QTN_MS 隔离期内 lsnsock 数组内存仍活,
    _uev_qtn_freelsn(watcher, lsn);
}
// 初始化msghdr结构体（用于recvmsg/sendmsg的地址和iov绑定）
static void _usk_init_msghdr(struct msghdr *msg, netaddr_ctx *addr, IOV_TYPE *iov, uint32_t niov) {
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
    for (;;) {
        // recvmsg 会将 msg_namelen 改写为本次实际地址长度，下一次调用前须重置为缓冲最大值
        udp->msg.msg_namelen = netaddr_size(&udp->addr);
        rtn = (int32_t)recvmsg(udp->sock.fd, &udp->msg, 0);
        if (rtn >= 0) {
            if (udp->msg.msg_flags & MSG_TRUNC) {
                // datagram 超过 MAX_RECVFROM_SIZE 被截断：残缺数据不上抛，告警丢弃后继续收（不关 socket）
                LOG_WARN("UDP datagram truncated on fd %d (exceeds %d bytes), dropped.",
                         (int32_t)udp->sock.fd, MAX_RECVFROM_SIZE);
                continue;
            }
            // 0 字节是合法 UDP datagram 不视为对端关闭；由 _usk_call_recvfrom_cb 过滤不向上抛
            _usk_call_recvfrom_cb(watcher->ev, udp, (size_t)rtn);
            continue;
        }
        if (ERR_RW_RETRIABLE(ERRNO)) {
            rtn = ERR_OK;
        }
        break;
    }
#ifdef MANUAL_ADD
    if (ERR_OK == rtn) {
        rtn = _uev_add_event(watcher, udp->sock.fd, &udp->sock.events, EVENT_READ, &udp->sock);
    }
#endif
    return rtn;
}
// UDP发送处理：循环sendmsg发送队列中所有数据包，队列空后删除写事件
static int32_t _usk_on_udp_wcb(watcher_ctx *watcher, udp_ctx *udp) {
    IOV_TYPE iov;
    off_buf_ctx *buf;
    netaddr_ctx *addr;
    struct msghdr msg;
    int32_t rtn = ERR_OK;
    while (NULL != (buf = queue_peek(&udp->buf_s))) {
        addr = (netaddr_ctx *)buf->data;
        iov.IOV_PTR_FIELD = (char *)buf->data + sizeof(netaddr_ctx);
        iov.IOV_LEN_FIELD = (IOV_LEN_TYPE)buf->lens;
        _usk_init_msghdr(&msg, addr, &iov, 1);
        rtn = sendmsg(udp->sock.fd, &msg, 0);
        if (rtn >= 0) {
            udp->wb_size -= buf->lens;
            queue_pop(&udp->buf_s);
            _evpub_off_buf_release(buf);
            rtn = ERR_OK;
        } else {
            if (ERR_RW_RETRIABLE(ERRNO)) {
                rtn = ERR_OK;
            } else {
                udp->wb_size -= buf->lens;
                queue_pop(&udp->buf_s);
                _evpub_off_buf_release(buf);
                rtn = ERR_FAILED;
            }
            break;
        }
    }
    if (ERR_OK != rtn) {
        return rtn;
    }
    if (0 == queue_size(&udp->buf_s)) {
        _uev_del_event(watcher, udp->sock.fd, &udp->sock.events, EVENT_WRITE, &udp->sock);
        return ERR_OK;
    }
#ifdef MANUAL_ADD
    rtn = _uev_add_event(watcher, udp->sock.fd, &udp->sock.events, EVENT_WRITE, &udp->sock);
#endif
    return rtn;
}
// UDP读写事件统一回调：STATUS_ERROR时入隔离队列延后释放，否则分别处理读写事件
static void _usk_on_udp_rw(watcher_ctx *watcher, sock_ctx *skctx, int32_t ev) {
    udp_ctx *udp = UPCAST(skctx, udp_ctx, sock);
    if (BIT_CHECK(udp->status, STATUS_ERROR)) {
#ifdef MANUAL_REMOVE
        _uev_del_event(watcher, skctx->fd, &skctx->events, skctx->events, skctx);
#endif
        _evpub_sockel_remove(watcher, skctx->fd);
        CLOSE_SOCK(skctx->fd);
        skctx->ev_cb = NULL;
        _uev_qtn_push(watcher, skctx, QTN_UDP);
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
#ifdef MANUAL_REMOVE
        _uev_del_event(watcher, skctx->fd, &skctx->events, skctx->events, skctx);
#endif
        _evpub_sockel_remove(watcher, skctx->fd);
        CLOSE_SOCK(skctx->fd);
        skctx->ev_cb = NULL;
        _uev_qtn_push(watcher, skctx, QTN_UDP);
    }
}
// 分配并初始化UDP上下文
static sock_ctx *_usk_new_udp(SOCKET fd, cbs_ctx *cbs, ud_cxt *ud) {
    udp_ctx *udp;
    MALLOC(udp, sizeof(udp_ctx));
    udp->sock.ev_cb = _usk_on_udp_rw;
    udp->sock.type = SOCK_DGRAM;
    udp->sock.fd = fd;
    udp->sock.events = 0;
    udp->status = STATUS_NONE;
    udp->skid = createid();
    udp->cbs = *cbs;
    udp->buf_r.IOV_PTR_FIELD = udp->buf;
    udp->buf_r.IOV_LEN_FIELD = sizeof(udp->buf);
    COPY_UD(udp->ud, ud);
    netaddr_empty(&udp->addr);
    _usk_init_msghdr(&udp->msg, &udp->addr, &udp->buf_r, 1);
    queue_init(&udp->buf_s, sizeof(off_buf_ctx), INIT_SENDBUF_LEN);
    udp->wb_size = 0;
    tda_init(&udp->tda, WB_WARN_INIT_SIZE);
    return &udp->sock;
}
void _uev_free_udp(sock_ctx *skctx) {
    udp_ctx *udp = UPCAST(skctx, udp_ctx, sock);
    CLOSE_SOCK(udp->sock.fd);
    _evpub_off_buf_clear(&udp->buf_s);
    queue_free(&udp->buf_s);
    UD_FREE(udp->cbs.ud_free, &udp->ud);
    FREE(udp);
}
int32_t ev_udp(ev_ctx *ctx, const char *ip, const uint16_t port, cbs_ctx *cbs, ud_cxt *ud,
    SOCKET *fd, uint64_t *skid) {
    if (NULL == cbs || NULL == cbs->rf_cb) {
        if (NULL != cbs) {
            UD_FREE(cbs->ud_free, ud);
        }
        return ERR_FAILED;
    }
    netaddr_ctx addr;
    if (ERR_OK != netaddr_set(&addr, ip, port)) {
        LOG_ERROR("netaddr_set %s:%d, %s", ip, port, ERRORSTR(ERRNO));
        UD_FREE(cbs->ud_free, ud);
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
            pool_push(&watcher->pool, skctx);
        } else {
            _uev_free_udp(skctx);
        }
        return;
    }
}

#endif//EV_IOCP
