#include "event/uev.h"
#include "utils/buffer.h"
#include "utils/netutils.h"
#include "utils/tda.h"
#include "containers/hashmap.h"

#ifndef EV_IOCP

#define QTN_MS   500 // 释放对象隔离时间(毫秒)，应大于一轮 kevent 周期

// 监听socket与所属监听器的绑定（SO_REUSEPORT时每个watcher有独立fd）
typedef struct lsnsock_ctx {
    evsock_ctx sock;          // 监听socket的事件上下文
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
    evsock_ctx sock;          // 基础事件上下文（含fd/events/ev_cb）
    int32_t status;         // 连接状态标志位（sock_status组合）
#if WITH_SSL
    SSL *ssl;               // SSL会话（NULL表示普通TCP）
    struct evssl_ctx *evssl; // 待升级的SSL上下文（发送完毕后升级）
    uint64_t wpend_ms;      // STATUS_WPEND_SSL 的零进展起点，仅该位置位期间有效
    list_node wpend_node;   // 挂 watcher->wpends；只在 STATUS_WPEND_SSL 置位期间在链上
#endif
    list_node flush_node;   // 挂 watcher->flushes；只在 STATUS_FLUSHPEND 置位期间在链上
    size_t wb_size;         // 当前 buf_s 中字节累计
    tda_ctx tda;            // 字节告警翻倍状态
    buffer_ctx buf_r;       // 接收缓冲区
    obuf_que buf_s;        // 发送队列
    cbs_ctx cbs;            // 回调函数集合
    ud_cxt ud;              // 用户数据
}tcp_ctx;
// Unix平台UDP上下文
typedef struct udp_ctx {
    evsock_ctx sock;              // 基础事件上下文
    int32_t status;             // 状态标志位
    size_t wb_size;             // 当前 buf_s 中字节累计
    tda_ctx tda;                // 字节告警翻倍状态
    cbs_ctx cbs;                // 回调函数集合
    sbuf_que buf_s;            // 发送队列
    ud_cxt ud;                  // 用户数据
}udp_ctx;

static void _usk_on_rw_cb(watcher_ctx *watcher, evsock_ctx *evsk, int32_t ev); // 前向声明：TCP读写事件回调

void _uev_sk_shutdown(evsock_ctx *evsk) {
#if WITH_SSL
    tcp_ctx *tcp = UPCAST(evsk, tcp_ctx, sock);
    evssl_shutdown(tcp->ssl, tcp->sock.sk.fd);
#else
    shutdown(evsk->sk.fd, SHUT_RD);
#endif
}
void *_evpub_sk_new(void *args) {
    skpool_args *skargs = (skpool_args *)args;
    tcp_ctx *tcp;
    MALLOC(tcp, sizeof(tcp_ctx));
    tcp->sock.ev_cb = _usk_on_rw_cb;
    tcp->sock.type = SOCK_STREAM;
    tcp->sock.sk = skargs->sk;// 带入 fd 与 index,skid 随后由 createid 覆写
    tcp->sock.events = 0;
#ifdef COMMIT_NCHANGES
    tcp->sock.chg_round = 0;
#endif
    tcp->status = STATUS_NONE;
    tcp->sock.sk.skid = createid();
#if WITH_SSL
    tcp->ssl = NULL;
    tcp->evssl = NULL;
#endif
    tcp->cbs = *skargs->cbs;
    COPY_UD(tcp->ud, skargs->ud);
    buffer_init(&tcp->buf_r);
    obuf_que_init(&tcp->buf_s, INIT_SENDBUF_LEN);
    tcp->wb_size = 0;
    tda_init(&tcp->tda, WB_WARN_INIT_SIZE);
    return &tcp->sock;
}
void _evpub_sk_free(void *sk) {
    tcp_ctx *tcp = UPCAST((evsock_ctx *)sk, tcp_ctx, sock);
#if WITH_SSL
    FREE_SSL(tcp->ssl);
#endif
    CLOSE_SOCK(tcp->sock.sk.fd);
    buffer_free(&tcp->buf_r);
    _evpub_off_buf_clear(&tcp->buf_s);
    obuf_que_free(&tcp->buf_s);
    UD_FREE(tcp->cbs.ud_free, &tcp->ud);
    FREE(tcp);
}
void _evpub_sk_clear(void *sk) {
    tcp_ctx *tcp = UPCAST((evsock_ctx *)sk, tcp_ctx, sock);
    tcp->sock.events = 0;
#ifdef COMMIT_NCHANGES
    tcp->sock.chg_round = 0;
#endif
    tcp->status = STATUS_NONE;
#if WITH_SSL
    FREE_SSL(tcp->ssl);
    tcp->evssl = NULL;
#endif
    CLOSE_SOCK(tcp->sock.sk.fd);
    _evpub_off_buf_clear(&tcp->buf_s);
    // clear 只归零不缩容,背压期涨上去的环会跟着对象一直待在池里,回池这步缩回初始容量
    if (obuf_que_capacity(&tcp->buf_s) > INIT_SENDBUF_LEN) {
        obuf_que_resize(&tcp->buf_s, INIT_SENDBUF_LEN);
    }
    tcp->wb_size = 0;
    tda_init(&tcp->tda, WB_WARN_INIT_SIZE);
    buffer_drain(&tcp->buf_r, buffer_size(&tcp->buf_r));
    UD_FREE(tcp->cbs.ud_free, &tcp->ud);
}
void _evpub_sk_reset(void *sk, void *args) {
    tcp_ctx *tcp = UPCAST((evsock_ctx *)sk, tcp_ctx, sock);
    skpool_args *skargs = (skpool_args *)args;
    tcp->sock.sk = skargs->sk;// 同 _evpub_sk_new
    tcp->sock.ev_cb = _usk_on_rw_cb;
    tcp->cbs = *skargs->cbs;
    tcp->sock.sk.skid = createid();
    COPY_UD(tcp->ud, skargs->ud);
}
ud_cxt *_uev_get_ud(evsock_ctx *evsk) {
    if (SOCK_STREAM == evsk->type) {
        return &UPCAST(evsk, tcp_ctx, sock)->ud;
    } else {
        return &UPCAST(evsk, udp_ctx, sock)->ud;
    }
}
int32_t _uev_check_skid(evsock_ctx *evsk, const uint64_t skid) {
    // listener / pipe 的 skid 恒 0 而业务 skid 最小为 1，故无需再按 type 排除，见 evsock_ctx.sk
    return skid == evsk->sk.skid ? ERR_OK : ERR_FAILED;
}
// 调用连接关闭回调
static inline void _usk_call_close_cb(ev_ctx *ev, tcp_ctx *tcp) {
    if (NULL != tcp->cbs.c_cb) {
        tcp->cbs.c_cb(ev, &tcp->sock.sk, SOCK_IS_CLIENT(tcp->status),
                      _evpub_close_type(tcp->status), &tcp->ud);
    }
}
// 调用UDP关闭回调。UDP 无 FIN 可言，STATUS_PEER_* 永不置位，close_type 恒为 LOCAL
static inline void _usk_call_udp_close_cb(ev_ctx *ev, udp_ctx *udp) {
    if (NULL != udp->cbs.c_cb) {
        udp->cbs.c_cb(ev, &udp->sock.sk, 0, _evpub_close_type(udp->status), &udp->ud);
    }
}
static inline int32_t _usk_keep_event(watcher_ctx *watcher, evsock_ctx *evsk, int32_t ev) {
#ifndef MANUAL_ADD
    if (BIT_CHECK(evsk->events, ev)) {
        return ERR_OK;
    }
#endif
    return _uev_add_event(watcher, evsk->sk.fd, &evsk->events, ev, evsk);
}
// 把 socket 从事件循环摘净：drop_changes →（MANUAL_REMOVE 下）del_event → sockel_remove →
// CLOSE_SOCK → 清 ev_cb。drop 必须排在 del 之前：devpoll 下 del 只是把 POLLREMOVE 排进 changes
// 等下一轮提交，而 drop 按 fd 无差别过滤，顺序反了会把它一起丢掉、fd 没摘出 /dev/poll 就被 close。
// CLOSE_SOCK 立刻把 fd 还给 OS 并置 INVALID_SOCK，_evpub_sk_clear 再关是幂等的；ev_cb 清空挡住
// 同批次后续 events 二次进来（隔离期跨轮 stale 也读到 NULL），pool_pop 时由 _evpub_sk_reset 恢复
static inline void _usk_detach(watcher_ctx *watcher, evsock_ctx *evsk) {
    _uev_drop_changes(watcher, evsk);
#ifdef MANUAL_REMOVE
    _uev_del_event(watcher, evsk->sk.fd, &evsk->events, evsk->events, evsk);
#endif
    _evpub_sockel_remove(watcher, evsk->sk.fd);
    CLOSE_SOCK(evsk->sk.fd);
    evsk->ev_cb = NULL;
}
#if WITH_SSL
// 挂起写看门狗。link/unlink 把连接挂进/摘出 watcher->wpends，tick 负责从外部收割：
// 挂起期间事件层不读，两端都是本框架时会成环，而成环后写永不就绪、连接自己的回调也永不再来。
// 只有 link/unlink 翻转 STATUS_WPEND_SSL，位与在链上一一对应，对象回池前必须先 unlink。
// 三者都与 _evpub_tick_add 同口径：须在该 fd 所属的 event 线程内调用。
// 定义序 unlink → tick → link 由依赖决定（tick 调 unlink、link 引用 tick），别重排
static void _usk_wpend_unlink(watcher_ctx *watcher, tcp_ctx *tcp) {
    if (!BIT_CHECK(tcp->status, STATUS_WPEND_SSL)) {
        return;
    }
    BIT_REMOVE(tcp->status, STATUS_WPEND_SSL);
    list_remove(&watcher->wpends, &tcp->wpend_node);
}
// 只看队头：链表按 wpend_ms 先后串，队头没到期后面的更不会到期，一次收割完就能停；
// 链空即把自己摘掉，让 ticks 恒空、_evpub_tick_drive 连时钟都不用读
static uint32_t _usk_wpend_tick(void *ud, uint64_t now_ms) {
    watcher_ctx *watcher = ud;
    list_node *head;
    tcp_ctx *tcp;
    for (;;) {
        head = watcher->wpends.head;
        if (NULL == head) {
            break;
        }
        tcp = UPCAST(head, tcp_ctx, wpend_node);
        if (now_ms - tcp->wpend_ms < SSL_WPEND_MAX_MS) {
            return (uint32_t)(SSL_WPEND_MAX_MS - (now_ms - tcp->wpend_ms));
        }
        LOG_WARN("ssl write pending %dms with no progress on fd %d, disconnect.",
                 SSL_WPEND_MAX_MS, (int32_t)tcp->sock.sk.fd);
        _usk_wpend_unlink(watcher, tcp);
        _evpub_mark_close(&tcp->status, ERR_FAILED);
        _uev_disconnect(watcher, &tcp->sock);
    }
    watcher->wpend_tick.cb = NULL;
    _evpub_tick_remove(watcher, &watcher->wpend_tick);
    return EVENT_WAIT_TIMEOUT;
}
// progress 非 0 表示本轮真发出了字节：重新计时并移到队尾，保持链表按时间有序
static void _usk_wpend_link(watcher_ctx *watcher, tcp_ctx *tcp, int32_t progress) {
    if (BIT_CHECK(tcp->status, STATUS_WPEND_SSL)) {
        // 已在链上:只有真发出了字节才算有进展,重新计时并移到队尾保持有序。
        // 写事件一直挂着,对端窗口微开就会触发一次 nsend==0 的写就绪,
        // 无条件重计时会让看门狗永不到期
        if (0 == progress) {
            return;
        }
        list_remove(&watcher->wpends, &tcp->wpend_node);
    } else {
        BIT_SET(tcp->status, STATUS_WPEND_SSL);
    }
    tcp->wpend_ms = timer_cur_ms(&watcher->timer);
    list_push_tail(&watcher->wpends, &tcp->wpend_node);
    if (NULL == watcher->wpend_tick.cb) {
        watcher->wpend_tick.cb = _usk_wpend_tick;
        watcher->wpend_tick.ud = watcher;
        _evpub_tick_add(watcher, &watcher->wpend_tick);
    }
}
#endif
// 本轮攒发链的挂与摘。位与"在 watcher->flushes 上"一一对应，置清位只在这两处；
// 对象回池前必须先 unlink，口径同上面的 wpend
static inline void _usk_flush_unlink(watcher_ctx *watcher, tcp_ctx *tcp) {
    if (!BIT_CHECK(tcp->status, STATUS_FLUSHPEND)) {
        return;
    }
    BIT_REMOVE(tcp->status, STATUS_FLUSHPEND);
    list_remove(&watcher->flushes, &tcp->flush_node);
}
static inline void _usk_flush_link(watcher_ctx *watcher, tcp_ctx *tcp) {
    if (BIT_CHECK(tcp->status, STATUS_FLUSHPEND)) {
        return;
    }
    BIT_SET(tcp->status, STATUS_FLUSHPEND);
    list_push_tail(&watcher->flushes, &tcp->flush_node);
}
// 关闭 TCP 连接：触发关闭回调 → 摘出事件循环 → 释放 ud → 入隔离队列暂存 QTN_MS 后归 pool
static inline void _usk_close_tcp(watcher_ctx *watcher, tcp_ctx *tcp) {
    // 内层错误路径(如回调里 ev_send 同步发失败)已关闭后,外层按自己的错误路径还会再关一次；
    // fd 已 INVALID 即早退,防止同一 tcp 二次入 qtn 隔离队列被 drain 两次 free(口径同 _usk_close_udp)
    if (sock_is_invalid(&tcp->sock.sk)) {
        return;
    }
    // 对象回池前必须摘链,否则 watcher->flushes 里留悬空节点
    _usk_flush_unlink(watcher, tcp);
#if WITH_SSL
    // 对象回池前必须摘链,否则 watcher->wpends 里留悬空节点(口径同监听 socket 的 backoff_tick)
    _usk_wpend_unlink(watcher, tcp);
#endif
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
    if (sock_is_invalid(&udp->sock.sk)) {
        return;
    }
    _usk_call_udp_close_cb(watcher->ev, udp);
    _usk_detach(watcher, &udp->sock);
    _uev_qtn_push(watcher, &udp->sock, QTN_UDP);
}
void _uev_disconnect(watcher_ctx *watcher, evsock_ctx *evsk) {
    if (SOCK_STREAM == evsk->type) {
        tcp_ctx *tcp = UPCAST(evsk, tcp_ctx, sock);
        if (BIT_CHECK(tcp->status, STATUS_ERROR)) {
            return;
        }
        _evpub_close_flush_tcp(tcp->sock.sk.fd, &tcp->buf_s, tcp->status, &tcp->wb_size, TCP_SSL(tcp));
        BIT_SET(tcp->status, STATUS_ERROR);
        _uev_sk_shutdown(evsk);
        // READ 接住 shutdown 造成的 EOF 边沿, WRITE 触发 _usk_on_rw_cb 入口的 STATUS_ERROR
        // 分支就地关闭; 两次注册都是拆连接的路, 任一注册不上就直接关, 别把连接吊到 keepalive
        if (ERR_OK != _usk_keep_event(watcher, &tcp->sock, EVENT_READ)
            || ERR_OK != _usk_keep_event(watcher, &tcp->sock, EVENT_WRITE)) {
            _usk_close_tcp(watcher, tcp);
        }
    } else {
        // UDP datagram 无连接,没有待发队列要冲
        udp_ctx *udp = UPCAST(evsk, udp_ctx, sock);
        if (BIT_CHECK(udp->status, STATUS_ERROR)) {
            return;
        }
        BIT_SET(udp->status, STATUS_ERROR);
        if (ERR_OK != _usk_keep_event(watcher, evsk, EVENT_WRITE)) {
            _usk_close_udp(watcher, udp);
        }
    }
}
// 调用accept回调，返回值非ERR_OK则拒绝连接
static inline int32_t _usk_call_acp_cb(ev_ctx *ev, tcp_ctx *tcp) {
    if (NULL != tcp->cbs.acp_cb) {
        return tcp->cbs.acp_cb(ev, &tcp->sock.sk, &tcp->ud);
    }
    return ERR_OK;
}
// 调用connect回调，返回值非ERR_OK则断开连接
static inline int32_t _usk_call_conn_cb(ev_ctx *ev, tcp_ctx *tcp, int32_t err) {
    if (NULL != tcp->cbs.conn_cb) {
        return tcp->cbs.conn_cb(ev, &tcp->sock.sk, err, &tcp->ud);
    }
    return ERR_OK;
}
#if WITH_SSL
// 调用SSL握手完成回调，返回值非ERR_OK则断开连接。调用点全在 #if WITH_SSL 内，
// 定义放外面 WITH_SSL=0 时是 unused
static inline int32_t _usk_call_ssl_exchanged_cb(ev_ctx *ev, tcp_ctx *tcp) {
    if (NULL != tcp->cbs.exch_cb) {
        return tcp->cbs.exch_cb(ev, &tcp->sock.sk, SOCK_IS_CLIENT(tcp->status), &tcp->ud, TCP_SSL(tcp));
    }
    return ERR_OK;
}
#endif
// 调用数据接收回调（nread > 0 才触发）
static inline void _usk_call_recv_cb(ev_ctx *ev, tcp_ctx *tcp, size_t nread) {
    if (nread > 0) {
        tcp->cbs.r_cb(ev, &tcp->sock.sk, SOCK_IS_CLIENT(tcp->status), &tcp->buf_r, nread, &tcp->ud);
    }
}
// 调用发送完成回调（nsend > 0 且有s_cb 才触发）。
// 回调里再调 ev_send 会同线程免投递直落 _uev_add_bufs_send,同步重入本轮发送会打乱
// _usk_tcp_send 回调之后那段记账,故置 STATUS_SENDING 让它只入队
static inline void _usk_call_send_cb(ev_ctx *ev, tcp_ctx *tcp, size_t nsend) {
    if (NULL != tcp->cbs.s_cb
        && nsend > 0) {
        BIT_SET(tcp->status, STATUS_SENDING);
        tcp->cbs.s_cb(ev, &tcp->sock.sk, SOCK_IS_CLIENT(tcp->status), nsend, &tcp->ud);
        BIT_REMOVE(tcp->status, STATUS_SENDING);
    }
}
// 调用UDP接收回调；0 字节 datagram 由本函数过滤不向上抛（_usk_on_udp_rcb 仍不视为 EOF，
// 继续 recvmsg 循环），避免上层处理空 payload 的特殊路径
static inline void _usk_call_recvfrom_cb(ev_ctx *ev, udp_ctx *udp, char *buf, netaddr_ctx *addr, size_t nread) {
    if (nread > 0) {
        udp->cbs.rf_cb(ev, &udp->sock.sk, buf, nread, addr, &udp->ud);
    }
}
#if WITH_SSL
// 触发切ssl
static int32_t _usk_ssl_exchange_trigger(watcher_ctx *watcher, tcp_ctx *tcp, struct evssl_ctx *evssl) {
    tcp->ssl = evssl_setfd(evssl, tcp->sock.sk.fd);
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
            if (ERR_OK != _uev_add_event(watcher, tcp->sock.sk.fd, &tcp->sock.events, EVENT_WRITE, &tcp->sock)) {
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
void _uev_keyupdate(watcher_ctx *watcher, evsock_ctx *evsk, int32_t updatetype) {
#if WITH_SSL
    (void)watcher;
    if (SOCK_STREAM != evsk->type) {
        return;
    }
    tcp_ctx *tcp = UPCAST(evsk, tcp_ctx, sock);
    // 只排程，不碰事件注册:报文要等下一次 SSL_write 才上线路(契约见 evssl.h)
    if (NULL == tcp->ssl
        || BIT_CHECK(tcp->status, STATUS_AUTHSSL)
        || BIT_CHECK(tcp->status, STATUS_ERROR)) {
        return;
    }
    if (ERR_OK != evssl_keyupdate(tcp->ssl, updatetype)) {
        LOG_WARN("ssl keyupdate failed on fd %d.", (int32_t)evsk->sk.fd);
    }
#else
    (void)watcher;
    (void)evsk;
    (void)updatetype;
#endif
}
void _uev_try_ssl_exchange(watcher_ctx *watcher, evsock_ctx *evsk, struct evssl_ctx *evssl, int32_t client) {
#if WITH_SSL
    if (SOCK_STREAM != evsk->type) {
        LOG_WARN("can't ssl exchange on udp.");
        return;
    }
    tcp_ctx *tcp = UPCAST(evsk, tcp_ctx, sock);
    if (0 == _evpub_ssl_exchange_check(tcp->ssl, &tcp->status, client)) {
        return;
    }
    // 判队列非空而不是判 EVENT_WRITE：攒发链上的连接数据还在队列里、写事件却尚未注册，
    // 看 EVENT_WRITE 会把"还没发"当成"已发完"，握手报文就抢在明文尾包前面出去
    if (!obuf_que_empty(&tcp->buf_s)) {
        tcp->evssl = evssl;
        BIT_SET(tcp->status, STATUS_SSLEXCHANGE);
    } else {
        if (ERR_OK != _usk_ssl_exchange_trigger(watcher, tcp, evssl)) {
            _uev_disconnect(watcher, evsk);
            LOG_ERROR("ssl exchange error.");
        }
    }
#else
    (void)watcher;
    (void)evsk;
    (void)evssl;
    (void)client;
#endif
}
// 从socket读取数据到接收缓冲区并触发recv回调，MANUAL_ADD时需重新注册读事件
static inline int32_t _usk_tcp_recv(watcher_ctx *watcher, tcp_ctx *tcp) {
    size_t nread;
    int32_t evrtn = ERR_OK;
    int32_t rtn = buffer_from_sock(&tcp->buf_r, tcp->sock.sk.fd, &nread, _evpub_sock_read, TCP_SSL(tcp));
#if WITH_SSL
    if (ERR_OK == rtn
        && NULL != tcp->ssl
        && SSL_want_write(tcp->ssl)) {// tls1.3 KeyUpdate探测
        BIT_SET(tcp->status, STATUS_KEYUPDATE_WRITE);
        evrtn = _uev_add_event(watcher, tcp->sock.sk.fd, &tcp->sock.events, EVENT_WRITE, &tcp->sock);
    }
#endif
    _usk_call_recv_cb(watcher->ev, tcp, nread);
    if (ERR_OK != rtn) {
        _evpub_mark_close(&tcp->status, rtn);
        return ERR_FAILED;
    }
    // 不 mark_close:这是本端按上限主动断,close_type 留在 LOCAL 档
    if (0 != _evpub_recvbuf_full(&tcp->buf_r, tcp->sock.sk.fd)) {
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
    int32_t rtn = _evpub_sock_send(tcp->sock.sk.fd, &tcp->buf_s, &nsend, TCP_SSL(tcp));
    tcp->wb_size -= nsend;
    _usk_call_send_cb(watcher->ev, tcp, nsend);
    if (ERR_OK != rtn) {
        _evpub_mark_close(&tcp->status, rtn);
        return ERR_FAILED;// 分类已进 status, 不透传 evssl_* 的 1/2(口径同 _usk_tcp_recv)
    }
    uint32_t cnt = obuf_que_size(&tcp->buf_s);
#if WITH_SSL
    // 挂读，并且必须摘掉写事件：水平触发下留着写事件就每轮重进本函数、SSL_write 再返
    // WANT_READ，空转烧满 watcher 线程。
    // 摘掉安全：清位后本函数按 cnt 重新注册
    if (NULL != tcp->ssl) {
        // 先全清再按 SSL 在等哪一边挂一个:两档互斥,队列空则一个都不该挂
        BIT_REMOVE(tcp->status, STATUS_KEYUPDATE_READ);
        if (0 == cnt) {
            _usk_wpend_unlink(watcher, tcp);
        } else {
            if (SSL_want_read(tcp->ssl)) {
                _usk_wpend_unlink(watcher, tcp);
                BIT_SET(tcp->status, STATUS_KEYUPDATE_READ);
                if (BIT_CHECK(tcp->sock.events, EVENT_WRITE)) {
                    _uev_del_event(watcher, tcp->sock.sk.fd, &tcp->sock.events, EVENT_WRITE, &tcp->sock);
                }
                return _usk_keep_event(watcher, &tcp->sock, EVENT_READ);
            }
            if (SSL_want_write(tcp->ssl)) {
                _usk_wpend_link(watcher, tcp, (0 != nsend) ? 1 : 0);
            } else {
                _usk_wpend_unlink(watcher, tcp);
            }
        }
    }
#endif
    if (0 == cnt) {
        if (BIT_CHECK(tcp->sock.events, EVENT_WRITE)) {// 直发快路径无EVENT_WRITE
            _uev_del_event(watcher, tcp->sock.sk.fd, &tcp->sock.events, EVENT_WRITE, &tcp->sock);
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
void _uev_flush_pending(watcher_ctx *watcher) {
    list_node *node;
    tcp_ctx *tcp;
#ifdef FLUSH_WATERMARK
    watcher->flush_bytes = 0;
#endif
    while (NULL != (node = list_pop_head(&watcher->flushes))) {
        tcp = UPCAST(node, tcp_ctx, flush_node);
        BIT_REMOVE(tcp->status, STATUS_FLUSHPEND);
        // 挂链之后同一批命令又把它关了：队列已由关闭路径接管，再发会重挂事件
        if (BIT_CHECK(tcp->status, STATUS_ERROR)
            || obuf_que_empty(&tcp->buf_s)) {
            continue;
        }
        if (ERR_OK != _usk_tcp_send(watcher, tcp)) {
            BIT_SET(tcp->status, STATUS_ERROR);
            _usk_close_tcp(watcher, tcp);
        }
    }
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
            _uev_del_event(watcher, tcp->sock.sk.fd, &tcp->sock.events, EVENT_WRITE, &tcp->sock);
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
            _uev_del_event(watcher, tcp->sock.sk.fd, &tcp->sock.events, EVENT_WRITE, &tcp->sock);
        }
        if (ERR_OK != _usk_keep_event(watcher, &tcp->sock, EVENT_READ)) {
            *err = ERR_FAILED;
            return 1;
        }
        return 0;
    case 2://等待写就绪（WANT_WRITE）：注册写事件，写就绪后再次进入此分支重试
        if (ERR_OK != _uev_add_event(watcher, tcp->sock.sk.fd, &tcp->sock.events, EVENT_WRITE, &tcp->sock)) {
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
static void _usk_on_rw_cb(watcher_ctx *watcher, evsock_ctx *evsk, int32_t ev) {
    tcp_ctx *tcp = UPCAST(evsk, tcp_ctx, sock);
    if (BIT_CHECK(tcp->status, STATUS_ERROR)) {
        _usk_close_tcp(watcher, tcp);
        return;
    }
    int32_t rtn = ERR_OK;
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
                if (obuf_que_empty(&tcp->buf_s)) {
                    evwrite = 0;
                    _uev_del_event(watcher, tcp->sock.sk.fd, &tcp->sock.events, EVENT_WRITE, &tcp->sock);
                }
                evread = 1;// 挂起的 SSL_read 必须先完成,才轮到 SSL_write
            }
        }
        // 有挂起的应用写就不能进 SSL_read(理由见 evpub.h 的 STATUS_WPEND_SSL)。
        // 先冲一次:冲掉了照常读,冲不掉本轮不读——写事件挂着,排空后由下面那处接力补读。
        // 判 KEYUPDATE_WRITE 是因为那一档挂着待重试的 SSL_read,此时不能调 SSL_write
        if (evread
            && BIT_CHECK(tcp->status, STATUS_WPEND_SSL)
            && !BIT_CHECK(tcp->status, STATUS_KEYUPDATE_WRITE)) {
            rtn = _usk_tcp_send(watcher, tcp);
            evwrite = 0;
            if (ERR_OK == rtn
                && BIT_CHECK(tcp->status, STATUS_WPEND_SSL)) {
                evread = 0;
                // 水平触发下不摘读事件就每轮重进本函数空转。
                // 口径同 KEYUPDATE_READ 那一档摘写事件,清位后由 _usk_tcp_recv 末尾补回
                if (BIT_CHECK(tcp->sock.events, EVENT_READ)) {
                    _uev_del_event(watcher, tcp->sock.sk.fd, &tcp->sock.events, EVENT_READ, &tcp->sock);
                }
            }
        }
    }
#endif// WITH_SSL
    if (ERR_OK == rtn && evread) {
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
#if WITH_SSL
    // KEYUPDATE_WRITE 挂着待重试的 SSL_read,此时不能调 SSL_write
    int32_t cansend = !BIT_CHECK(tcp->status, STATUS_KEYUPDATE_WRITE);
#else
    int32_t cansend = 1;
#endif
    if (ERR_OK == rtn && evwrite && 0 != cansend) {
#if WITH_SSL
        int32_t waspend = BIT_CHECK(tcp->status, STATUS_WPEND_SSL);
#endif
        rtn = _usk_tcp_send(watcher, tcp);
#if WITH_SSL
        // 上面因 WPEND_SSL 跳过的那次读,挂起写一排空就主动补一次:水平触发下电平没落
        // 下轮还会再报,补这一次是为了省掉那一轮事件循环的等待
        if (ERR_OK == rtn
            && 0 != waspend
            && !BIT_CHECK(tcp->status, STATUS_WPEND_SSL)) {
            rtn = _usk_tcp_recv(watcher, tcp);
            // 收尾同上面读分支:清 WPEND_SSL 的同一次发送可能改挂 KEYUPDATE_READ 并摘掉写事件,
            // 不在这里重试,队列就再没人驱动
            if (ERR_OK == rtn
                && BIT_CHECK(tcp->status, STATUS_KEYUPDATE_READ)
                && !BIT_CHECK(tcp->status, STATUS_KEYUPDATE_WRITE)) {
                rtn = _usk_tcp_send(watcher, tcp);
            }
        }
#endif
    }
    if (ERR_OK != rtn) {
        BIT_SET(tcp->status, STATUS_ERROR);
        _usk_close_tcp(watcher, tcp);
        return;
    }
}
void _uev_add_bufs_send(watcher_ctx *watcher, evsock_ctx *evsk, off_buf_ctx *buf) {
    tcp_ctx *tcp = UPCAST(evsk, tcp_ctx, sock);
    // 已在关闭流程：拒收新数据
    if (BIT_CHECK(tcp->status, STATUS_ERROR)) {
        _evpub_off_buf_release(buf);
        return;
    }
    if (!_evpub_sendqu_check_tcp(obuf_que_size(&tcp->buf_s), tcp->status, evsk->sk.fd)) {
        _evpub_off_buf_release(buf);
        _uev_disconnect(watcher, evsk);
        return;
    }
    int32_t was_empty = obuf_que_empty(&tcp->buf_s);
    tcp->wb_size += buf->lens;
#ifdef FLUSH_WATERMARK
    watcher->flush_bytes += buf->lens;
#endif
    _evpub_sendqu_tda(&tcp->tda, tcp->wb_size, evsk->sk.fd, 1);
    obuf_que_push(&tcp->buf_s, buf);
    // s_cb 执行期：外层 _usk_tcp_send 尚未做完回调后那段记账,此刻发或改攒发链都会打乱它。
    // 只入队即可,外层按回调之后重读的 cnt 把这批一并带出去,不会拖到下一轮
    if (BIT_CHECK(tcp->status, STATUS_SENDING)) {
        return;
    }
    // 已在攒发链上：堆满一次 iov 就先发一批，免得一轮里的巨量 ev_send 全压到轮末
    if (BIT_CHECK(tcp->status, STATUS_FLUSHPEND)) {
        if (obuf_que_size(&tcp->buf_s) < MAX_SEND_NIOV) {
            return;
        }
        _usk_flush_unlink(watcher, tcp);
        if (ERR_OK != _usk_tcp_send(watcher, tcp)) {
            BIT_SET(tcp->status, STATUS_ERROR);
            _usk_close_tcp(watcher, tcp);
        }
        return;
    }
    // 非空且不在攒发链：要么 EVENT_WRITE 已注册,要么正处 KEYUPDATE_READ(那时写事件被摘掉,
    // 由 _usk_on_rw_cb 收到数据后重驱动 _usk_tcp_send)。两种都无需在此重复处理
    if (!was_empty) {
        return;
    }
#if WITH_SSL
    // KeyUpdate 挂起 SSL_read 时调 SSL_write 违反 OpenSSL"必须先重试同一操作"的约定；
    // EVENT_WRITE 在置 KEYUPDATE_WRITE 时已注册,数据留队等 _usk_on_rw_cb 重试完再按序处理。
    // 不判 KEYUPDATE_READ:它只在 buf_s 非空时置位,走不到这里
    if (BIT_CHECK(tcp->status, STATUS_KEYUPDATE_WRITE)) {
        return;
    }
#endif
    // 从空队列开始：不立即发,挂进攒发链等本轮派发结束后一次 writev 发掉。同一 fd 在一轮里
    // 常有多条 ev_send,逐条发就是逐次 syscall 加逐个 TCP 段(NODELAY 开着)
    _usk_flush_link(watcher, tcp);
#ifdef FLUSH_WATERMARK
    // 攒到水位就先冲,免得载荷堆着等
    if (watcher->flush_bytes >= FLUSH_WATERMARK) {
        _uev_flush_pending(watcher);
    }
#endif
}
static void _usk_on_connect_cb_err(watcher_ctx *watcher, tcp_ctx *tcp) {
    _usk_call_conn_cb(watcher->ev, tcp, ERR_FAILED);
    _evpub_sockel_remove(watcher, tcp->sock.sk.fd);
    // 顺序同 _usk_detach，但这里 drop 多担一件事：进来前已排过的那条 del_event 带着 evsk
    // 当 udata，sock 紧接着回池，kqueue 下留着会落到复用同一 fd 号的新连接上。
    // 代价是 devpoll 的 POLLREMOVE 也被清掉，故下面按平台补排一次
    _uev_drop_changes(watcher, &tcp->sock);
#ifdef MANUAL_REMOVE
    _uev_del_event(watcher, tcp->sock.sk.fd, &tcp->sock.events, tcp->sock.events, &tcp->sock);
#endif
    tcp->sock.ev_cb = NULL;
    pool_push(&watcher->pool, &tcp->sock, 0);
}
// connect完成事件回调：检查连接结果，切换为读写回调，触发conn回调
static void _usk_on_connect_cb(watcher_ctx *watcher, evsock_ctx *evsk, int32_t ev) {
    (void)ev;
    tcp_ctx *tcp = UPCAST(evsk, tcp_ctx, sock);
    tcp->sock.ev_cb = _usk_on_rw_cb;
    _uev_del_event(watcher, tcp->sock.sk.fd, &tcp->sock.events, tcp->sock.events, evsk);
    if (BIT_CHECK(tcp->status, STATUS_ERROR)
        || ERR_OK != sock_checkconn(tcp->sock.sk.fd)) {
        _usk_on_connect_cb_err(watcher, tcp);
        return;
    }
    BIT_SET(tcp->status, STATUS_ESTABLISHED);
    if (ERR_OK != _evpub_tcp_keepalive(tcp->sock.sk.fd)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        _usk_on_connect_cb_err(watcher, tcp);
        return;
    }
#if WITH_SSL
    if (NULL != tcp->evssl) {// 默认启用ssl，初始化
        tcp->ssl = evssl_setfd(tcp->evssl, tcp->sock.sk.fd);
        if (NULL == tcp->ssl) {
            _usk_on_connect_cb_err(watcher, tcp);
            return;
        }
        BIT_SET(tcp->status, STATUS_AUTHSSL);
    }
#endif
    if (ERR_OK != _uev_add_event(watcher, tcp->sock.sk.fd, &tcp->sock.events, EVENT_READ, &tcp->sock)) {
        _usk_on_connect_cb_err(watcher, tcp);
        return;
    }
    // 链接成功回调
    if (ERR_OK != _usk_call_conn_cb(watcher->ev, tcp, ERR_OK)) {
        _uev_disconnect(watcher, evsk);
        return;
    }
#if WITH_SSL
    if (NULL != tcp->ssl) {
        switch (evssl_tryconn(tcp->ssl)) {
        case ERR_OK://完成
            BIT_REMOVE(tcp->status, STATUS_AUTHSSL);
            if (ERR_OK != _usk_call_ssl_exchanged_cb(watcher->ev, tcp)) {
                _uev_disconnect(watcher, evsk);
                return;
            }
            break;
        case 1:// WANT_READ
            break;
        case 2://WANT_WRITE
            if (ERR_OK != _uev_add_event(watcher, tcp->sock.sk.fd, &tcp->sock.events, EVENT_WRITE, &tcp->sock)) {
                _uev_disconnect(watcher, evsk);
                return;
            }
            break;
        default://错误
            _uev_disconnect(watcher, evsk);
            return;
        }
    }
#endif
}
int32_t ev_connect(ev_ctx *ctx, struct evssl_ctx *evssl, const char *ip, const uint16_t port, cbs_ctx *cbs, ud_cxt *ud,
    int32_t setsess, sock_ctx *sk) {
    netaddr_ctx addr;
    if (ERR_OK != _evpub_sock_launch_check(ctx, ip, port, cbs, ud, 0, &addr)) {
        return ERR_FAILED;
    }
    sk->fd = sock_create_cloexec(netaddr_family(&addr), SOCK_STREAM, 0, 1);
    if (sock_is_invalid(sk)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        UD_FREE(cbs->ud_free, ud);
        return ERR_FAILED;
    }
    if (ERR_OK != sock_nodelay(sk->fd)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        CLOSE_SOCK((sk->fd));
        UD_FREE(cbs->ud_free, ud);
        return ERR_FAILED;
    }
    int32_t rtn = connect(sk->fd, netaddr_addr(&addr), netaddr_size(&addr));
    if (ERR_OK != rtn) {
        rtn = ERRNO;
        if (!ERR_CONNECT_RETRIABLE(rtn)) {
            LOG_ERROR("connect %s:%d, %s", ip, port, ERRORSTR(ERRNO));
            CLOSE_SOCK((sk->fd));
            UD_FREE(cbs->ud_free, ud);
            return ERR_FAILED;
        }
    }
    skpool_args skargs = { .sk = { .fd = sk->fd, .index = (int32_t)CALC_WATCHER_INDEX(sk->fd, ctx->nthreads) },
                           .cbs = cbs, .ud = ud };
    evsock_ctx *evsk = (evsock_ctx *)_evpub_sk_new(&skargs);
    evsk->ev_cb = _usk_on_connect_cb;
    tcp_ctx *tcp = UPCAST(evsk, tcp_ctx, sock);
    if (setsess) {
        tcp->ud.sess = tcp->sock.sk.skid;
    }
    BIT_SET(tcp->status, STATUS_CLIENT);
    *sk = tcp->sock.sk;// 整体带出 fd/index/skid
#if WITH_SSL
    tcp->evssl = evssl;
#else
    (void)evssl;
#endif
    _cmd_connect(ctx, evsk, NULL);
    return ERR_OK;
}
void _uev_add_conn_inloop(watcher_ctx *watcher, evsock_ctx *evsk) {
    _evpub_sockel_add(watcher, evsk);
    if (ERR_OK != _uev_add_event(watcher, evsk->sk.fd, &evsk->events, EVENT_WRITE, evsk)) {
        tcp_ctx *tcp = UPCAST(evsk, tcp_ctx, sock);
        _usk_call_conn_cb(watcher->ev, tcp, ERR_FAILED);
        _evpub_sockel_remove(watcher, evsk->sk.fd);
        pool_push(&watcher->pool, evsk, 0);
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
        _uev_del_event(watcher, acpt->sock.sk.fd, &acpt->sock.events, EVENT_READ, &acpt->sock);
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
static void _usk_on_accept_cb(watcher_ctx *watcher, evsock_ctx *evsk, int32_t ev) {
    (void)ev;
    lsnsock_ctx *acpt = UPCAST(evsk, lsnsock_ctx, sock);
    SOCKET fd;
    watcher_ctx *to;
    int32_t unremove;
    while ((unremove = (0 == ATOMIC_GET(&acpt->lsn->remove)))) {
        fd = sock_accept_cloexec(acpt->sock.sk.fd, NULL, NULL, 1);
        if (INVALID_SOCK == fd) {
            if (ERR_OK == _usk_check_accept(watcher, acpt)) {
                continue;
            }
            break;
        }
        if (ERR_OK != sock_nodelay(fd)
            || ERR_OK != _evpub_tcp_keepalive(fd)) {
            CLOSE_SOCK(fd);
            continue;
        }
#if REUSEPORT_BALANCED
        // 内核已按连接散到各 listen fd,accept 落在哪个线程就留在哪个,省掉跨线程投递那一跳
        to = watcher;
#else
        to = CALC_WATCHER(watcher->ev->watcher, watcher->ev->nthreads, fd);
#endif
        if (_evpub_inloop(to)) {
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
                      watcher->index, (int32_t)acpt->sock.sk.fd, ERRORSTR(ERRNO));// 须在 CLOSE_SOCK 之前:close 会覆写 errno
            _evpub_sockel_remove(watcher, acpt->sock.sk.fd);
            CLOSE_SOCK(acpt->sock.sk.fd);
            acpt->sock.ev_cb = NULL;
        }
    }
}
static void _uev_add_acpfd_inloop_err(watcher_ctx *watcher, tcp_ctx *tcp) {
    _evpub_sockel_remove(watcher, tcp->sock.sk.fd);
    pool_push(&watcher->pool, &tcp->sock, 0);
}
// accept 出的 fd 在此挂上目标 watcher。同线程直挂与跨线程 CMD_ADDACP 两条路都汇到这里，
// 归属只由 watcher 形参定,与 fd 取模无关——新增每连接一次的初始化放这里,不要写到调用点去
void _uev_add_acpfd_inloop(watcher_ctx *watcher, SOCKET fd, listener_ctx *lsn) {
    skpool_args skargs = { .sk = { .fd = fd, .index = watcher->index }, .cbs = &lsn->cbs, .ud = &lsn->ud };
    evsock_ctx *evsk = pool_pop(&watcher->pool, &skargs, 0);
    tcp_ctx *tcp = UPCAST(evsk, tcp_ctx, sock);
    BIT_SET(tcp->status, STATUS_ESTABLISHED);// accept 出来的连接已连通
    _evpub_sockel_add(watcher, evsk);
#if WITH_SSL
    if (NULL != lsn->evssl) {// 默认启用ssl
        // 设置ssl
        tcp->ssl = evssl_setfd(lsn->evssl, tcp->sock.sk.fd);
        if (NULL == tcp->ssl) {
            _uev_add_acpfd_inloop_err(watcher, tcp);
            return;
        }
        BIT_SET(tcp->status, STATUS_AUTHSSL);
    }
#endif
    if (ERR_OK != _uev_add_event(watcher, fd, &evsk->events, EVENT_READ, evsk)) {
        _uev_add_acpfd_inloop_err(watcher, tcp);
        return;
    }
    if (ERR_OK != _usk_call_acp_cb(watcher->ev, tcp)) {
        _uev_disconnect(watcher, evsk);
        return;
    }
}
// 关闭监听socket（无SO_REUSEPORT只关闭第一个，否则关闭所有cnt个）
static void _usk_close_lsnsock(listener_ctx *lsn, int32_t cnt) {
#ifndef SO_REUSEPORT
    CLOSE_SOCK(lsn->lsnsock[0].sock.sk.fd);
#else
    for (int32_t i = 0; i < cnt; i++) {
        CLOSE_SOCK(lsn->lsnsock[i].sock.sk.fd);
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
        lsnsock->sock.sk.index = i;// 监听 socket 按数组下标挂 watcher[i],与 CALC_WATCHER_INDEX(fd) 无关
#ifndef SO_REUSEPORT
        lsnsock->sock.sk.fd = fd;
#else
        lsnsock->sock.sk.fd = _evpub_listen(&addr);
        if (sock_is_invalid(&lsnsock->sock.sk)) {
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
    lsn_arr_push_back(&ctx->arrlsn, &lsn);
    spin_unlock(&ctx->spin);
    SET_PTR(id, lsn->id);
    return ERR_OK;
}
void _uev_add_lsn_inloop(watcher_ctx *watcher, evsock_ctx *evsk) {
    _evpub_sockel_add(watcher, evsk);
    if (ERR_OK != _uev_add_event(watcher, evsk->sk.fd, &evsk->events, EVENT_READ, evsk)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        _evpub_sockel_remove(watcher, evsk->sk.fd);
        CLOSE_SOCK(evsk->sk.fd);
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
    qtn_que_push(&watcher->qtn, &e);
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
            _evpub_sk_free((evsock_ctx *)e->obj);
        } else {
            pool_push(&watcher->pool, (evsock_ctx *)e->obj, 0);
        }
        break;
    case QTN_UDP:
        _uev_free_udp((evsock_ctx *)e->obj);
        break;
    case QTN_LSN:
        _uev_freelsn((listener_ctx *)e->obj);
        break;
    }
}
void _uev_qtn_drain(watcher_ctx *watcher, uint64_t now_ms) {
    qtn_entry *e;
    while (NULL != (e = qtn_que_peek(&watcher->qtn))) {
        if (now_ms - e->enter_ms < QTN_MS) {
            break;
        }
        _uev_qtn_release(watcher, e, 0);
        qtn_que_pop(&watcher->qtn);
    }
}
void _uev_qtn_flush(watcher_ctx *watcher) {
    qtn_entry *e;
    while (NULL != (e = qtn_que_pop(&watcher->qtn))) {
        _uev_qtn_release(watcher, e, 1);
    }
    qtn_que_free(&watcher->qtn);
}
// 根据id从arrlsn中查找并移除listener_ctx（加自旋锁保护）
static listener_ctx * _usk_get_listener(ev_ctx *ctx, uint64_t id) {
    listener_ctx *lsn = NULL;
    listener_ctx **tmp;
    spin_lock(&ctx->spin);
    uint32_t n = lsn_arr_size(&ctx->arrlsn);
    for (uint32_t i = 0; i < n; i++) {
        tmp = lsn_arr_at(&ctx->arrlsn, (int32_t)i);
        if ((*tmp)->id == id) {
            lsn = *tmp;
            lsn_arr_del_nomove(&ctx->arrlsn, (int32_t)i);
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
    SOCKET fd = curlsn->sock.sk.fd;
    if (INVALID_SOCK != fd) {
        _evpub_sockel_remove(watcher, fd);
        _uev_drop_changes(watcher, &curlsn->sock);// 顺序理由见 _usk_detach
#ifdef MANUAL_REMOVE
        _uev_del_event(watcher, fd, &curlsn->sock.events, EVENT_READ, &curlsn->sock);
#endif
        CLOSE_SOCK(curlsn->sock.sk.fd);
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
// UDP接收处理：循环 recvmsg 直到 EAGAIN，一次事件尽量收干净，少等几轮事件循环
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
        rtn = (int32_t)recvmsg(udp->sock.sk.fd, &msg, 0);
        if (rtn >= 0) {
            nerr = 0;// 读到 datagram 即证 fd 正常,失败计数按"连续"而非累计,免高流量下偶发失败攒满上限误关
            if (msg.msg_flags & MSG_TRUNC) {
                // datagram 超过 MAX_RECVFROM_SIZE 被截断：残缺数据不上抛，告警丢弃后继续收（不关 socket）
                LOG_WARN("UDP datagram truncated on fd %d (exceeds %d bytes), dropped.",
                         (int32_t)udp->sock.sk.fd, MAX_RECVFROM_SIZE);
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
        LOG_WARN("UDP recvmsg dropped on fd %d: %s.", (int32_t)udp->sock.sk.fd, ERRORSTR(err));
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
    while (NULL != (buf = sbuf_que_peek(&udp->buf_s))) {
        snd = _usk_udp_sendmsg_once(udp->sock.sk.fd, buf->data, buf->len, &buf->addr);
        if (1 == snd) {
            break;
        }
        udp->wb_size -= buf->len;
        FREE(buf->data);
        sbuf_que_pop(&udp->buf_s);
        if (ERR_FAILED == snd) {
            return ERR_FAILED;
        }
    }
    if (sbuf_que_empty(&udp->buf_s)) {
        if (BIT_CHECK(udp->sock.events, EVENT_WRITE)) {// 直发快路径无EVENT_WRITE
            _uev_del_event(watcher, udp->sock.sk.fd, &udp->sock.events, EVENT_WRITE, &udp->sock);
        }
        return ERR_OK;
    }
    return _usk_keep_event(watcher, &udp->sock, EVENT_WRITE);
}
// UDP读写事件统一回调：STATUS_ERROR时入隔离队列延后释放，否则分别处理读写事件
static void _usk_on_udp_rw(watcher_ctx *watcher, evsock_ctx *evsk, int32_t ev) {
    udp_ctx *udp = UPCAST(evsk, udp_ctx, sock);
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
void _uev_add_bufs_sendto(watcher_ctx *watcher, evsock_ctx *evsk, sendto_ctx *buf, int32_t tried) {
    udp_ctx *udp = UPCAST(evsk, udp_ctx, sock);
    // ERROR 期拒收:否则绕过 _usk_on_udp_rw 的 STATUS_ERROR 检查直接触达 _usk_on_udp_wcb
    if (BIT_CHECK(udp->status, STATUS_ERROR)
        || !_evpub_sendqu_check_udp(sbuf_que_size(&udp->buf_s), evsk->sk.fd)) {
        FREE(buf->data);
        return;
    }
    int32_t was_empty = sbuf_que_empty(&udp->buf_s);
    udp->wb_size += buf->len;
    _evpub_sendqu_tda(&udp->tda, udp->wb_size, evsk->sk.fd, 0);
    sbuf_que_push(&udp->buf_s, buf);
    // 队列本来就非空：EVENT_WRITE 必然已经注册（否则数据早发不出去），无需重复处理
    if (!was_empty) {
        return;
    }
    if (tried) {
        // 调用方入队前已尝试过一次发送(EAGAIN)，此刻必然复现，跳过重试，仅确保写事件已注册
        if (ERR_OK != _usk_keep_event(watcher, evsk, EVENT_WRITE)) {
            _uev_disconnect(watcher, evsk);
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
int32_t _uev_try_sendto(watcher_ctx *watcher, evsock_ctx *evsk, const void *data, size_t len, netaddr_ctx *addr) {
    udp_ctx *udp = UPCAST(evsk, udp_ctx, sock);
    // 已在 error 关闭流程：fd 已知致命，无需再发起 sendmsg 尝试，也不需要调用方转入排队
    if (BIT_CHECK(udp->status, STATUS_ERROR)) {
        return 0;
    }
    if (!sbuf_que_empty(&udp->buf_s)) {
        return 1;
    }
    int32_t snd = _usk_udp_sendmsg_once(evsk->sk.fd, data, len, addr);
    if (ERR_OK == snd) {
        return 0;
    }
    if (ERR_FAILED == snd) {
        _uev_disconnect(watcher, evsk);
        return 0;
    }
    return 1;
}
// 分配并初始化UDP上下文
static evsock_ctx *_usk_new_udp(skpool_args *skargs) {
    udp_ctx *udp;
    MALLOC(udp, sizeof(udp_ctx));
    udp->sock.ev_cb = _usk_on_udp_rw;
    udp->sock.type = SOCK_DGRAM;
    udp->sock.sk = skargs->sk;// 口径同 _evpub_sk_new
    udp->sock.events = 0;
#ifdef COMMIT_NCHANGES
    udp->sock.chg_round = 0;
#endif
    udp->status = STATUS_NONE;
    udp->sock.sk.skid = createid();
    udp->cbs = *skargs->cbs;
    COPY_UD(udp->ud, skargs->ud);
    sbuf_que_init(&udp->buf_s, INIT_SENDBUF_LEN);
    udp->wb_size = 0;
    tda_init(&udp->tda, WB_WARN_INIT_SIZE);
    return &udp->sock;
}
void _uev_free_udp(evsock_ctx *evsk) {
    udp_ctx *udp = UPCAST(evsk, udp_ctx, sock);
    CLOSE_SOCK(udp->sock.sk.fd);
    _evpub_sendto_clear(&udp->buf_s);
    sbuf_que_free(&udp->buf_s);
    UD_FREE(udp->cbs.ud_free, &udp->ud);
    FREE(udp);
}
int32_t ev_udp(ev_ctx *ctx, const char *ip, const uint16_t port, cbs_ctx *cbs, ud_cxt *ud,
    sock_ctx *sk) {
    netaddr_ctx addr;
    if (ERR_OK != _evpub_sock_launch_check(ctx, ip, port, cbs, ud, 1, &addr)) {
        return ERR_FAILED;
    }
    sk->fd = _evpub_udp(&addr);
    if (sock_is_invalid(sk)) {
        LOG_ERROR("udp %s:%d error.", ip, port);
        UD_FREE(cbs->ud_free, ud);
        return ERR_FAILED;
    }
    skpool_args skargs = { .sk = { .fd = sk->fd, .index = (int32_t)CALC_WATCHER_INDEX(sk->fd, ctx->nthreads) },
                           .cbs = cbs, .ud = ud };
    evsock_ctx *evsk = _usk_new_udp(&skargs);
    *sk = evsk->sk;
    _cmd_add(&ctx->watcher[sk->index], evsk);
    return ERR_OK;
}
void _uev_add_fd_inloop(watcher_ctx *watcher, evsock_ctx *evsk) {
    _evpub_sockel_add(watcher, evsk);
    if (ERR_OK != _uev_add_event(watcher, evsk->sk.fd, &evsk->events, EVENT_READ, evsk)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        _evpub_sockel_remove(watcher, evsk->sk.fd);
        if (SOCK_STREAM == evsk->type) {
            pool_push(&watcher->pool, evsk, 0);
        } else {
            _usk_call_udp_close_cb(watcher->ev, UPCAST(evsk, udp_ctx, sock));
            _uev_free_udp(evsk);
        }
        return;
    }
}

#endif//EV_IOCP
