#include "event/event.h"
#include "event/iocp.h"
#include "utils/pool.h"
#include "utils/buffer.h"
#include "utils/netutils.h"
#include "utils/tda.h"
#include "containers/hashmap.h"

#ifdef EV_IOCP

#define MAX_ACCEPTEX_CNT    128  // 每个监听socket同时挂起的AcceptEx数量
#define ACCEPTEX_ADDR_LEN   (sizeof(struct sockaddr_storage) + 16) // AcceptEx 每段地址长度（Microsoft 要求 ≥ sizeof(sockaddr_XX) + 16）

// AcceptEx单个挂起操作的上下文
typedef struct overlap_acpt_ctx {
    sock_ctx overlap;                       // 嵌入sock_ctx，供IOCP回调使用
    struct listener_ctx *lsn;               // 所属监听器
    DWORD bytes;                            // AcceptEx接收到的字节数
    char addr[ACCEPTEX_ADDR_LEN * 2];      // 存储本地/对端地址的缓冲区
}overlap_acpt_ctx;
// IOCP监听器上下文
typedef struct listener_ctx {
    int32_t family;                                 // 地址族（AF_INET/AF_INET6）
    atomic_t remove;                                // 标记为待移除（ev_unlisten后设置）
    atomic_t ref;                                   // 引用计数（等于挂起的AcceptEx数量）
    SOCKET fd;                                      // 监听socket句柄
#if WITH_SSL
    evssl_ctx *evssl;                               // SSL上下文（NULL表示不使用SSL）
#endif
    cbs_ctx cbs;                                    // 回调函数集合
    ud_cxt ud;                                      // 用户数据模板（每个accept连接复制一份）
    uint64_t id;                                    // 监听器唯一ID（ev_unlisten使用）
    overlap_acpt_ctx overlap_acpt[MAX_ACCEPTEX_CNT]; // 预挂起的AcceptEx数组
}listener_ctx;
// IOCP TCP连接上下文（读写各用一个sock_ctx / OVERLAPPED）
typedef struct overlap_tcp_ctx {
    sock_ctx ol_r;          // 读操作的OVERLAPPED上下文
    sock_ctx ol_s;          // 写操作的OVERLAPPED上下文
    int32_t status;         // 连接状态标志位（sock_status组合）
    DWORD bytes_r;          // WSARecv接收字节数
    DWORD bytes_s;          // WSASend发送字节数
    DWORD flag;             // WSARecv标志
#if WITH_SSL
    SSL *ssl;               // SSL会话（NULL表示普通TCP）
    struct evssl_ctx *evssl; // 待升级的SSL上下文（发送完毕后升级）
#endif
    size_t wb_size;         // 当前 buf_s 中字节累计
    uint64_t skid;          // 连接唯一ID
    tda_ctx tda;            // 字节告警翻倍状态
    IOV_TYPE wsabuf;        // WSARecv缓冲区描述符
    buffer_ctx buf_r;       // 接收缓冲区
    queue_ctx buf_s;        // 发送队列
    cbs_ctx cbs;            // 回调函数集合
    ud_cxt ud;              // 用户数据
}overlap_tcp_ctx;
// IOCP UDP连接上下文
typedef struct overlap_udp_ctx {
    sock_ctx ol_r;          // 接收操作的OVERLAPPED上下文
    sock_ctx ol_s;          // 发送操作的OVERLAPPED上下文
    int32_t addrlen;        // netaddr_ctx中地址结构的长度
    int32_t status;         // 状态标志位
    DWORD bytes_r;          // 接收字节数
    DWORD bytes_s;          // 发送字节数
    DWORD flag;             // WSARecvFrom标志
    size_t wb_size;         // 当前 buf_s 中字节累计
    uint64_t skid;          // 连接唯一ID
    tda_ctx tda;            // 字节告警翻倍状态
    cbs_ctx cbs;            // 回调函数集合
    IOV_TYPE wsabuf_s;      // 发送缓冲区描述符
    IOV_TYPE wsabuf_r;      // 接收缓冲区描述符（指向buf）
    queue_ctx buf_s;        // 发送队列
    netaddr_ctx addr;       // 接收到的对端地址（WSARecvFrom填充）
    ud_cxt ud;              // 用户数据
    char buf[MAX_RECVFROM_SIZE]; // 固定接收缓冲区
}overlap_udp_ctx;

static void _olp_on_recv_cb(watcher_ctx *watcher, sock_ctx *skctx, DWORD bytes); // 前向声明：TCP接收完成回调
static void _olp_on_send_cb(watcher_ctx *watcher, sock_ctx *skctx, DWORD bytes); // 前向声明：TCP发送完成回调

void _iocp_sk_shutdown(sock_ctx *skctx) {
#if WITH_SSL
    overlap_tcp_ctx *oltcp = UPCAST(skctx, overlap_tcp_ctx, ol_r);
    evssl_shutdown(oltcp->ssl, oltcp->ol_r.fd);
#else
    shutdown(skctx->fd, SHUT_RD);
#endif
}
void *_evpub_sk_new(void *args) {
    skpool_args *skargs = (skpool_args *)args;
    overlap_tcp_ctx *oltcp;
    MALLOC(oltcp, sizeof(overlap_tcp_ctx));
    oltcp->ol_r.type = SOCK_STREAM;
    oltcp->ol_r.fd = skargs->fd;
    oltcp->ol_r.ev_cb = _olp_on_recv_cb;
    oltcp->ol_s.type = SOCK_STREAM;
    oltcp->ol_s.fd = skargs->fd;
    oltcp->ol_s.ev_cb = _olp_on_send_cb;
    oltcp->status = STATUS_NONE;
    oltcp->skid = createid();
#if WITH_SSL
    oltcp->ssl = NULL;
    oltcp->evssl = NULL;
#endif
    oltcp->wsabuf.IOV_PTR_FIELD = NULL;
    oltcp->wsabuf.IOV_LEN_FIELD = 0;
    oltcp->cbs = *skargs->cbs;
    COPY_UD(oltcp->ud, skargs->ud);
    buffer_init(&oltcp->buf_r);
    queue_init(&oltcp->buf_s, sizeof(off_buf_ctx), INIT_SENDBUF_LEN);
    oltcp->wb_size = 0;
    tda_init(&oltcp->tda, WB_WARN_INIT_SIZE);
    return &oltcp->ol_r;
}
void _evpub_sk_free(void *sk) {
    overlap_tcp_ctx *oltcp = UPCAST((sock_ctx *)sk, overlap_tcp_ctx, ol_r);
#if WITH_SSL
    FREE_SSL(oltcp->ssl);
#endif
    CLOSE_SOCK(oltcp->ol_r.fd);
    buffer_free(&oltcp->buf_r);
    _evpub_off_buf_clear(&oltcp->buf_s);
    queue_free(&oltcp->buf_s);
    UD_FREE(oltcp->cbs.ud_free, &oltcp->ud);
    FREE(oltcp);
}
void _evpub_sk_clear(void *sk)  {
    overlap_tcp_ctx *oltcp = UPCAST((sock_ctx *)sk, overlap_tcp_ctx, ol_r);
    oltcp->status = STATUS_NONE;
#if WITH_SSL
    FREE_SSL(oltcp->ssl);
    oltcp->evssl = NULL;
#endif
    CLOSE_SOCK(oltcp->ol_r.fd);
    oltcp->ol_s.fd = INVALID_SOCK;
    _evpub_off_buf_clear(&oltcp->buf_s);
    oltcp->wb_size = 0;
    tda_init(&oltcp->tda, WB_WARN_INIT_SIZE);
    buffer_drain(&oltcp->buf_r, buffer_size(&oltcp->buf_r));
    UD_FREE(oltcp->cbs.ud_free, &oltcp->ud);
}
void _evpub_sk_reset(void *sk, void *args) {
    skpool_args *skargs = (skpool_args *)args;
    overlap_tcp_ctx *oltcp = UPCAST((sock_ctx *)sk, overlap_tcp_ctx, ol_r);
    oltcp->ol_r.fd = skargs->fd;
    oltcp->ol_r.ev_cb = _olp_on_recv_cb;
    oltcp->ol_s.fd = skargs->fd;
    oltcp->ol_s.ev_cb = _olp_on_send_cb;
    oltcp->cbs = *skargs->cbs;
    oltcp->skid = createid();
    COPY_UD(oltcp->ud, skargs->ud);
}
ud_cxt *_iocp_get_ud(sock_ctx *skctx) {
    if (SOCK_STREAM == skctx->type) {
        return &UPCAST(skctx, overlap_tcp_ctx, ol_r)->ud;
    } else {
        return &UPCAST(skctx, overlap_udp_ctx, ol_r)->ud;
    }
}
int32_t _iocp_check_skid(sock_ctx *skctx, const uint64_t skid) {
    if (SOCK_STREAM == skctx->type) {
        if (skid == UPCAST(skctx, overlap_tcp_ctx, ol_r)->skid) {
            return ERR_OK;
        }
    } else if (SOCK_DGRAM == skctx->type) {
        if (skid == UPCAST(skctx, overlap_udp_ctx, ol_r)->skid) {
            return ERR_OK;
        }
    }
    // type==0 (listener / cmd channel) 不属于业务 fd，直接拒绝；防误传fd时
    // UPCAST 强转读偏移到不可预测字段后碰巧匹配 skid 触发未定义行为
    return ERR_FAILED;
}
void _iocp_disconnect(sock_ctx *skctx, int32_t immed) {
    if (SOCK_STREAM == skctx->type) {
        overlap_tcp_ctx *tcp = UPCAST(skctx, overlap_tcp_ctx, ol_r);
        if (BIT_CHECK(tcp->status, STATUS_ERROR)) {
            return;
        }
        if (BIT_CHECK(tcp->status, STATUS_GRACEFUL_CLOSE)) {
            if (0 == immed) {
                return;
            }
            // graceful 中升级到 immed (如 _olp_on_send_cb 内 send 失败): 清 GRACEFUL_CLOSE 走 ERROR 路径
            BIT_REMOVE(tcp->status, STATUS_GRACEFUL_CLOSE);
        }
        // graceful 但已无待发数据 + 无 in-flight WSASend → 退化为立即关
        // (否则 _olp_on_send_cb 不会再触发,无人关闭连接)
        if (0 == immed
            && 0 == queue_size(&tcp->buf_s)
            && !BIT_CHECK(tcp->status, STATUS_SENDING)) {
            immed = 1;
        }
        if (0 != immed) {
            BIT_SET(tcp->status, STATUS_ERROR);
            _iocp_sk_shutdown(skctx);
            CancelIoEx((HANDLE)skctx->fd, NULL);
        } else {
            BIT_SET(tcp->status, STATUS_GRACEFUL_CLOSE);
            _iocp_sk_shutdown(skctx);
            // 不调 CancelIoEx; outstanding WSASend 完成后 _olp_on_send_cb 检测 GRACEFUL+empty → _olp_close_tcp
        }
    } else {
        // UDP datagram graceful 无意义,始终走立即关分支
        overlap_udp_ctx *udp = UPCAST(skctx, overlap_udp_ctx, ol_r);
        if (BIT_CHECK(udp->status, STATUS_ERROR)) {
            return;
        }
        BIT_SET(udp->status, STATUS_ERROR);
        CancelIoEx((HANDLE)skctx->fd, NULL);
    }
}
// 调用accept回调，返回值非ERR_OK则拒绝连接
static inline int32_t _olp_call_acp_cb(ev_ctx *ev, overlap_tcp_ctx *oltcp) {
    if (NULL != oltcp->cbs.acp_cb) {
        return oltcp->cbs.acp_cb(ev, oltcp->ol_r.fd, oltcp->skid, &oltcp->ud);
    }
    return ERR_OK;
}
// 调用connect回调，返回值非ERR_OK则断开连接
static inline int32_t _olp_call_conn_cb(ev_ctx *ev, overlap_tcp_ctx *oltcp, int32_t err) {
    if (NULL != oltcp->cbs.conn_cb) {
        return oltcp->cbs.conn_cb(ev, oltcp->ol_r.fd, oltcp->skid, err, &oltcp->ud);
    }
    return ERR_OK;
}
// 调用SSL握手完成回调，返回值非ERR_OK则断开连接
static inline int32_t _olp_call_ssl_exchanged_cb(ev_ctx *ev, overlap_tcp_ctx *oltcp) {
    if (NULL != oltcp->cbs.exch_cb) {
#if WITH_SSL
        return oltcp->cbs.exch_cb(ev, oltcp->ol_r.fd, oltcp->skid, BIT_CHECK(oltcp->status, STATUS_CLIENT), &oltcp->ud, oltcp->ssl);
#else
        return oltcp->cbs.exch_cb(ev, oltcp->ol_r.fd, oltcp->skid, BIT_CHECK(oltcp->status, STATUS_CLIENT), &oltcp->ud, NULL);
#endif
    }
    return ERR_OK;
}
// 调用数据接收回调（nread > 0 才触发）
static inline void _olp_call_recv_cb(ev_ctx *ev, overlap_tcp_ctx *oltcp, size_t nread) {
    if (nread > 0) {
        oltcp->cbs.r_cb(ev, oltcp->ol_r.fd, oltcp->skid, BIT_CHECK(oltcp->status, STATUS_CLIENT), &oltcp->buf_r, nread, &oltcp->ud);
    }
}
// 调用发送完成回调（nsend > 0 且有s_cb 才触发）
static inline void _olp_call_send_cb(ev_ctx *ev, overlap_tcp_ctx *oltcp, size_t nsend) {
    if (NULL != oltcp->cbs.s_cb
        && nsend > 0) {
        oltcp->cbs.s_cb(ev, oltcp->ol_s.fd, oltcp->skid, BIT_CHECK(oltcp->status, STATUS_CLIENT), nsend, &oltcp->ud);
    }
}
// 调用连接关闭回调
static inline void _olp_call_close_cb(ev_ctx *ev, overlap_tcp_ctx *oltcp) {
    if (NULL != oltcp->cbs.c_cb) {
        oltcp->cbs.c_cb(ev, oltcp->ol_r.fd, oltcp->skid, BIT_CHECK(oltcp->status, STATUS_CLIENT), &oltcp->ud);
    }
}
// 调用UDP接收回调；0 字节 datagram 由本函数过滤不向上抛（_olp_on_recvfrom_cb 仍不视为 EOF，
// 继续 _olp_post_recv_from 接收下一包），避免上层处理空 payload 的特殊路径
static inline void _olp_call_recvfrom_cb(ev_ctx *ev, overlap_udp_ctx *oludp, size_t nread) {
    if (nread > 0) {
        oludp->cbs.rf_cb(ev, oludp->ol_r.fd, oludp->skid, oludp->wsabuf_r.buf, nread, &oludp->addr, &oludp->ud);
    }
}
// 提交WSARecv异步接收请求（用于TCP和命令管道）
int32_t _iocp_post_recv(sock_ctx *skctx, DWORD  *bytes, DWORD  *flag, IOV_TYPE *wsabuf, DWORD niov) {
    *flag = 0;
    *bytes = 0;
    ZERO(&skctx->overlapped, sizeof(skctx->overlapped));
    if (ERR_OK != WSARecv(skctx->fd,
                          wsabuf,
                          niov,
                          bytes,
                          flag,
                          &skctx->overlapped,
                          NULL)) {
        if (ERROR_IO_PENDING != ERRNO) {
            return ERR_FAILED;
        }
    }
    return ERR_OK;
}
// 关闭TCP连接：若正在发送则标记延迟关闭，否则立即执行关闭回调并回收到对象池
static inline void _olp_close_tcp(watcher_ctx *watcher, overlap_tcp_ctx *oltcp) {
    if (BIT_CHECK(oltcp->status, STATUS_SENDING)) {
        BIT_SET(oltcp->status, STATUS_REMOVE);
    } else {
        _olp_call_close_cb(watcher->ev, oltcp);
        _evpub_sockel_remove(watcher, oltcp->ol_r.fd);
        pool_push(&watcher->pool, &oltcp->ol_r);
    }
}
// 从socket读取数据到接收缓冲区并触发recv回调，成功后重新提交WSARecv
static inline void _olp_tcp_recv(watcher_ctx *watcher, overlap_tcp_ctx *oltcp) {
    size_t nread;
#if WITH_SSL
    int32_t rtn = buffer_from_sock(&oltcp->buf_r, oltcp->ol_r.fd, &nread, _evpub_sock_read, oltcp->ssl);
#else
    int32_t rtn = buffer_from_sock(&oltcp->buf_r, oltcp->ol_r.fd, &nread, _evpub_sock_read, NULL);
#endif
    _olp_call_recv_cb(watcher->ev, oltcp, nread);
    if (ERR_OK == rtn) {
        rtn = _iocp_post_recv(&oltcp->ol_r, &oltcp->bytes_r, &oltcp->flag, &oltcp->wsabuf, 1);
    }
    if (ERR_OK != rtn) {
        BIT_SET(oltcp->status, STATUS_ERROR);
        _olp_close_tcp(watcher, oltcp);
    }
}
// IOCP TCP接收完成回调：处理SSL握手或普通数据接收
static void _olp_on_recv_cb(watcher_ctx *watcher, sock_ctx *skctx, DWORD bytes) {
    overlap_tcp_ctx *oltcp = UPCAST(skctx, overlap_tcp_ctx, ol_r);
    if (0 != oltcp->ol_r.overlapped.Internal) {
        BIT_SET(oltcp->status, STATUS_ERROR);
    }
#if WITH_SSL
    if (NULL != oltcp->ssl
        && BIT_CHECK(oltcp->status, STATUS_AUTHSSL)
        && !BIT_CHECK(oltcp->status, STATUS_ERROR)) {
        /* IOCP 无原生写就绪通知机制，无法像 epoll 那样注册 WANT_WRITE 等待。
         * 握手报文通常 < 16KB，socket 发送缓冲区 >= 8KB，绝大多数情况下
         * 首次调用即可成功；有界自旋（最多 16 次 CPU_PAUSE 重试）覆盖极端情况，
         * 仍无法写则视为不可恢复错误关闭连接。*/
        int32_t rtn;
        int32_t is_client = BIT_CHECK(oltcp->status, STATUS_CLIENT);
        for (int32_t i = 0; i < 16; i++) {
            rtn = is_client ? evssl_tryconn(oltcp->ssl) : evssl_tryacpt(oltcp->ssl);
            if (2 != rtn) {
                break;
            }
            CPU_PAUSE();
        }
        switch (rtn) {
        case ERR_FAILED://错误
            BIT_SET(oltcp->status, STATUS_ERROR);
            break;
        case ERR_OK://完成
            if (ERR_OK == _olp_call_ssl_exchanged_cb(watcher->ev, oltcp)) {
                BIT_REMOVE(oltcp->status, STATUS_AUTHSSL);
            } else {
                BIT_SET(oltcp->status, STATUS_ERROR);
            }
            break;
        case 1://等待更多数据（WANT_READ）
            if (ERR_OK != _iocp_post_recv(&oltcp->ol_r, &oltcp->bytes_r, &oltcp->flag, &oltcp->wsabuf, 1)) {
                BIT_SET(oltcp->status, STATUS_ERROR);
                break;
            } else {
                return;
            }
        case 2://有界重试耗尽，发送缓冲区持续满，关闭连接
            BIT_SET(oltcp->status, STATUS_ERROR);
            break;
        }
    }
#endif
    if (BIT_CHECK(oltcp->status, STATUS_ERROR)) {
        _olp_close_tcp(watcher, oltcp);
        return;
    }
    _olp_tcp_recv(watcher, oltcp);
}
#if WITH_SSL
// 在事件循环内启动SSL握手：设置SSL fd并根据角色调用connect/accept，客户端可能立即完成
static int32_t _olp_ssl_exchange(watcher_ctx *watcher, overlap_tcp_ctx *oltcp, struct evssl_ctx *evssl) {
    oltcp->ssl = evssl_setfd(evssl, oltcp->ol_r.fd);
    if (NULL == oltcp->ssl) {
        return ERR_FAILED;
    }
    if (BIT_CHECK(oltcp->status, STATUS_CLIENT)) {
        switch (evssl_tryconn(oltcp->ssl)) {
        case ERR_FAILED://错误
            return ERR_FAILED;
        case ERR_OK://完成
            return _olp_call_ssl_exchanged_cb(watcher->ev, oltcp);
        case 1://等待读就绪（WANT_READ）
            BIT_SET(oltcp->status, STATUS_AUTHSSL);
            break;
        case 2://等待写就绪（WANT_WRITE）：IOCP 无写就绪通知，设 STATUS_AUTHSSL 后
               // 由 _olp_on_recv_cb 的有界自旋重试处理
            BIT_SET(oltcp->status, STATUS_AUTHSSL);
            break;
        }
    } else {
        BIT_SET(oltcp->status, STATUS_AUTHSSL);
    }
    return ERR_OK;
}
#endif
void _iocp_try_ssl_exchange(watcher_ctx *watcher, sock_ctx *skctx, struct evssl_ctx *evssl, int32_t client) {
#if WITH_SSL
    if (SOCK_STREAM != skctx->type) {
        LOG_WARN("can't ssl exchange on udp.");
        return;
    }
    overlap_tcp_ctx *oltcp = UPCAST(skctx, overlap_tcp_ctx, ol_r);
    if (NULL != oltcp->ssl) {
        LOG_WARN("ssl already in use.");
        return;
    }
    if (BIT_CHECK(oltcp->status, STATUS_SSLEXCHANGE)) {
        LOG_WARN("repeat request ssl exchange.");
        return;
    }
    if (BIT_CHECK(oltcp->status, STATUS_ERROR)
        || BIT_CHECK(oltcp->status, STATUS_GRACEFUL_CLOSE)) {
        return;
    }
    if (client) {
        BIT_SET(oltcp->status, STATUS_CLIENT);
    } else {
        BIT_REMOVE(oltcp->status, STATUS_CLIENT);
    }
    if (BIT_CHECK(oltcp->status, STATUS_SENDING)) {
        oltcp->evssl = evssl;
        BIT_SET(oltcp->status, STATUS_SSLEXCHANGE);
    } else {
        if (ERR_OK != _olp_ssl_exchange(watcher, oltcp, evssl)) {
            _iocp_disconnect(skctx, 1);
            LOG_ERROR("ssl exchange error.");
        }
    }
#endif
}
// 提交WSASend异步发送请求
// 0 字节 wakeup：
// 成功 → kernel 有空间 → continue
// EWOULDBLOCK → kernel 满 → 0 字节 post pend → 等到 buffer 空出空间触发完成 → continue
static int32_t _olp_post_send(overlap_tcp_ctx *oltcp) {
    oltcp->bytes_s = 0;
    ZERO(&oltcp->ol_s.overlapped, sizeof(oltcp->ol_s.overlapped));
    if (ERR_OK != WSASend(oltcp->ol_s.fd,
                          &oltcp->wsabuf,
                          1,
                          &oltcp->bytes_s,
                          0,
                          &oltcp->ol_s.overlapped,
                          NULL)) {
        if (ERROR_IO_PENDING != ERRNO) {
            return ERR_FAILED;
        }
    }
    return ERR_OK;
}
// IOCP TCP发送完成回调：消费发送队列，处理SSL升级，触发send回调
static void _olp_on_send_cb(watcher_ctx *watcher, sock_ctx *skctx, DWORD bytes) {
    overlap_tcp_ctx *oltcp = UPCAST(skctx, overlap_tcp_ctx, ol_s);
    (void)bytes;
    // OVERLAPPED.Internal 是底层 NTSTATUS：非 0 表示 IOCP 已报错（对端 reset 等），
    // 提前置 STATUS_ERROR 省去下一轮同步 _evpub_sock_send 的无谓尝试
    if (0 != oltcp->ol_s.overlapped.Internal) {
        BIT_SET(oltcp->status, STATUS_ERROR);
    }
    if (BIT_CHECK(oltcp->status, STATUS_ERROR)) {
        if (BIT_CHECK(oltcp->status, STATUS_REMOVE)) {
            _olp_call_close_cb(watcher->ev, oltcp);
            _evpub_sockel_remove(watcher, oltcp->ol_r.fd);
            pool_push(&watcher->pool, &oltcp->ol_r);
        } else {
            BIT_REMOVE(oltcp->status, STATUS_SENDING);
        }
        return;
    }
    size_t nsend;
#if WITH_SSL
    int32_t rtn = _evpub_sock_send(oltcp->ol_s.fd, &oltcp->buf_s, &nsend, oltcp->ssl);
#else
    int32_t rtn = _evpub_sock_send(oltcp->ol_s.fd, &oltcp->buf_s, &nsend, NULL);
#endif
    oltcp->wb_size -= nsend;
    _olp_call_send_cb(watcher->ev, oltcp, nsend);
    if (ERR_OK != rtn) {
        BIT_REMOVE(oltcp->status, STATUS_SENDING);
        _iocp_disconnect(&oltcp->ol_r, 1);
        return;
    }
    if (0 == queue_size(&oltcp->buf_s)) {
        BIT_REMOVE(oltcp->status, STATUS_SENDING);
#if WITH_SSL
        if (BIT_CHECK(oltcp->status, STATUS_SSLEXCHANGE)) {
            BIT_REMOVE(oltcp->status, STATUS_SSLEXCHANGE);
            if (ERR_OK != _olp_ssl_exchange(watcher, oltcp, oltcp->evssl)) {
                LOG_ERROR("ssl exchange error.");
                _iocp_disconnect(&oltcp->ol_r, 1);
                return;
            }
        }
#endif
        // graceful close 数据发完:转 immed 关闭路径,经 CancelIoEx 取消 outstanding WSARecv,
        // 由 _olp_on_recv_cb 走 STATUS_ERROR 分支统一 _olp_close_tcp,避免在此立即关导致 ol_r 完成事件二次进入
        // (_iocp_disconnect 内部已处理 graceful→immed 升级,无需手动 BIT_REMOVE)
        if (BIT_CHECK(oltcp->status, STATUS_GRACEFUL_CLOSE)) {
            _iocp_disconnect(&oltcp->ol_r, 1);
        }
        return;
    }
    if (ERR_OK != _olp_post_send(oltcp)) {
        BIT_REMOVE(oltcp->status, STATUS_SENDING);
        _iocp_disconnect(&oltcp->ol_r, 1);
    }
}
void _iocp_add_bufs_trypost(sock_ctx *skctx, off_buf_ctx *buf) {
    overlap_tcp_ctx *oltcp = UPCAST(skctx, overlap_tcp_ctx, ol_r);
    // 已在 graceful/error 关闭流程：拒收新数据
    if (BIT_CHECK(oltcp->status, STATUS_GRACEFUL_CLOSE)
        || BIT_CHECK(oltcp->status, STATUS_ERROR)) {
        _evpub_off_buf_release(buf);
        return;
    }
#if WITH_SSL
    // SSL 握手期间禁止发送业务数据，否则会中断握手；命中即丢数据并立即关连接
    if (BIT_CHECK(oltcp->status, STATUS_AUTHSSL)
        || BIT_CHECK(oltcp->status, STATUS_SSLEXCHANGE)) {
        LOG_WARN("ev_send during SSL handshake on fd %d, disconnect.", (int32_t)oltcp->ol_s.fd);
        _evpub_off_buf_release(buf);
        _iocp_disconnect(&oltcp->ol_r, 1);
        return;
    }
#endif
    // TCP 慢消费者保护：发送队列超阈值丢数据并 disconnect，避免业务无脑写打爆内存
    if (0 != MAX_SENDQ_CNT
        && queue_size(&oltcp->buf_s) >= MAX_SENDQ_CNT) {
        LOG_WARN("TCP send queue overflow on fd %d (>= %d), disconnect.",
                 (int32_t)oltcp->ol_s.fd, MAX_SENDQ_CNT);
        _evpub_off_buf_release(buf);
        _iocp_disconnect(&oltcp->ol_r, 1);
        return;
    }
    oltcp->wb_size += buf->lens;
    if (tda_check(&oltcp->tda, oltcp->wb_size)) {
        LOG_WARN("TCP send buf growing on fd %d: %zu bytes.",
                 (int32_t)oltcp->ol_s.fd, oltcp->wb_size);
    }
    queue_push(&oltcp->buf_s, buf);
    if (BIT_CHECK(oltcp->status, STATUS_SENDING)
        || BIT_CHECK(oltcp->status, STATUS_ERROR)) {
        return;
    }
    BIT_SET(oltcp->status, STATUS_SENDING);
    if (ERR_OK != _olp_post_send(oltcp)) {
        BIT_SET(oltcp->status, STATUS_ERROR);
        BIT_REMOVE(oltcp->status, STATUS_SENDING);
    }
}
// 将socket绑定到通配地址（ConnectEx要求socket必须先bind）
static int32_t _olp_trybind(SOCKET fd, int32_t family) {
    int32_t rtn;
    netaddr_ctx addr;
    if (AF_INET == family) {
        rtn = netaddr_set(&addr, "0.0.0.0", 0);
    } else {
        rtn = netaddr_set(&addr, "::", 0);
    }
    if (ERR_OK != rtn) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    if (ERR_OK != bind(fd, netaddr_addr(&addr), netaddr_size(&addr))) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    return ERR_OK;
}
// 提交ConnectEx异步连接请求
static int32_t _olp_post_connect(overlap_tcp_ctx *oltcp, netaddr_ctx *addr) {
    oltcp->bytes_r = 0;
    ZERO(&oltcp->ol_r.overlapped, sizeof(oltcp->ol_r.overlapped));
    if (!_exfuncs.connectex(oltcp->ol_r.fd,
                            netaddr_addr(addr),
                            netaddr_size(addr),
                            NULL,
                            0,
                            &oltcp->bytes_r,
                            &oltcp->ol_r.overlapped)) {
        int32_t erro = ERRNO;
        if (ERROR_IO_PENDING != erro) {
            LOG_WARN("%s", ERRORSTR(erro));
            return ERR_FAILED;
        }
    }
    return ERR_OK;
}
// ConnectEx完成回调：切换为读回调并提交WSARecv，触发conn回调，可选启动SSL
static void _olp_on_connect_cb(watcher_ctx *watcher, sock_ctx *skctx, DWORD bytes) {
    skctx->ev_cb = _olp_on_recv_cb;
    overlap_tcp_ctx *oltcp = UPCAST(skctx, overlap_tcp_ctx, ol_r);
    if (ERROR_SUCCESS != oltcp->ol_r.overlapped.Internal
        || ERR_OK != setsockopt(oltcp->ol_r.fd, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0)
        || ERR_OK != _iocp_post_recv(&oltcp->ol_r, &oltcp->bytes_r, &oltcp->flag, &oltcp->wsabuf, 1)) {
        _olp_call_conn_cb(watcher->ev, oltcp, ERR_FAILED);
        _evpub_sockel_remove(watcher, oltcp->ol_r.fd);
        pool_push(&watcher->pool, &oltcp->ol_r);
        return;
    }
    if (ERR_OK != _olp_call_conn_cb(watcher->ev, oltcp, ERR_OK)) {
        _iocp_disconnect(skctx, 1);
        return;
    }
#if WITH_SSL
    if (NULL != oltcp->evssl) {
        if (ERR_OK != _olp_ssl_exchange(watcher, oltcp, oltcp->evssl)) {
            _iocp_disconnect(skctx, 1);
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
    netaddr_ctx *addr;
    MALLOC(addr, sizeof(netaddr_ctx));
    if (ERR_OK != netaddr_set(addr, ip, port)) {
        LOG_ERROR("netaddr_set %s:%d, %s", ip, port, ERRORSTR(ERRNO));
        UD_FREE(cbs->ud_free, ud);
        FREE(addr);
        return ERR_FAILED;
    }
    *fd = _evpub_create_sock(SOCK_STREAM, netaddr_family(addr));
    if (INVALID_SOCK == *fd) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        UD_FREE(cbs->ud_free, ud);
        FREE(addr);
        return ERR_FAILED;
    }
    sock_reuseaddr(*fd);
    _evpub_nodelay_nonblock(*fd);
    if (ERR_OK != _olp_trybind(*fd, netaddr_family(addr))) {
        CLOSE_SOCK((*fd));
        UD_FREE(cbs->ud_free, ud);
        FREE(addr);
        return ERR_FAILED;
    }
    skpool_args skargs = { *fd, cbs, ud };
    sock_ctx *skctx = (sock_ctx *)_evpub_sk_new(&skargs);
    skctx->ev_cb = _olp_on_connect_cb;
    overlap_tcp_ctx *oltcp = UPCAST(skctx, overlap_tcp_ctx, ol_r);
    BIT_SET(oltcp->status, STATUS_CLIENT);
    *skid = oltcp->skid;
#if WITH_SSL
    oltcp->evssl = evssl;
#else
    (void)evssl;
#endif
    _cmd_connect(ctx, skctx, addr);
    return ERR_OK;
}
void _iocp_add_conn_inloop(watcher_ctx *watcher, struct sock_ctx *skctx, netaddr_ctx *addr) {
    overlap_tcp_ctx *oltcp = UPCAST(skctx, overlap_tcp_ctx, ol_r);
    if (ERR_OK != _iocp_join(watcher, skctx->fd)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        _olp_call_conn_cb(watcher->ev, oltcp, ERR_FAILED);
        pool_push(&watcher->pool, &oltcp->ol_r);
        return;
    }
    _evpub_sockel_add(watcher, skctx);
    if (ERR_OK != _olp_post_connect(oltcp, addr)) {
        _olp_call_conn_cb(watcher->ev, oltcp, ERR_FAILED);
        _evpub_sockel_remove(watcher, oltcp->ol_r.fd);
        pool_push(&watcher->pool, &oltcp->ol_r);
    }
}
// 提交一个AcceptEx异步接受请求（创建新socket并挂起等待连接）
static int32_t _olp_post_accept(overlap_acpt_ctx *olacp) {
    SOCKET fd = _evpub_create_sock(SOCK_STREAM, olacp->lsn->family);
    if (INVALID_SOCK == fd) {
        olacp->overlap.fd = INVALID_SOCK;
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    olacp->bytes = 0;
    olacp->overlap.fd = fd;
    ZERO(&olacp->overlap.overlapped, sizeof(olacp->overlap.overlapped));
    if (!_exfuncs.acceptex(olacp->lsn->fd,//Listen Socket
                           olacp->overlap.fd,//Accept Socket
                           &olacp->addr,
                           0,
                           ACCEPTEX_ADDR_LEN,
                           ACCEPTEX_ADDR_LEN,
                           &olacp->bytes,
                           &olacp->overlap.overlapped)) {
        int32_t erro = ERRNO;
        if (ERROR_IO_PENDING != erro) {
            CLOSE_SOCK(olacp->overlap.fd);
            LOG_ERROR("%s", ERRORSTR(erro));
            return ERR_FAILED;
        }
    }
    return ERR_OK;
}
// AcceptEx完成回调：重新提交AcceptEx、设置socket选项、将新fd发送给对应watcher
static void _olp_on_accept_cb(acceptex_ctx *acpctx, sock_ctx *skctx, DWORD bytes) {
    overlap_acpt_ctx *olacp = UPCAST(skctx, overlap_acpt_ctx, overlap);
    listener_ctx *lsn = olacp->lsn;
    SOCKET fd = olacp->overlap.fd;
    if (0 != ATOMIC_GET(&lsn->remove)) {
        // ev_unlisten 或 _olp_acceptex 失败已标记卸载，最后一个减到 0 的 cb 释放 lsn
        _iocp_try_freelsn(lsn);
        return;
    }
    // 放这里为了让 _iocp_try_freelsn 能关闭
    olacp->overlap.fd = INVALID_SOCK;
    // lsn->ref 计「挂起 AcceptEx 槽位数 + ev_unlisten/ev_listen 占位 + 跨 watcher CMD_ADDACP 投递占位」。
    // _olp_post_accept 成功后槽位重新挂起，ref 不变；仅当 _olp_post_accept 失败时减 1。
    if (ERR_OK != _olp_post_accept(olacp)) {
        SOCKET log_fd = lsn->fd; // 在 atomic 减 ref 前拷贝，此时 ref >= 1，lsn 有效
        CLOSE_SOCK(fd);
        int32_t old = ATOMIC_ADD(&lsn->ref, -1);
        if (1 == old) {
            // 减到 0：其他 cb 与占位都已减完，独占释放
            _iocp_freelsn(lsn);
        } else if (2 == old) {
            // 减到 1：所有 cb 失败但占位 ref 还在；端口仍由 lsn->fd 占用，等用户调 ev_unlisten
            LOG_ERROR("all AcceptEx slots exhausted, listener fd %d (call ev_unlisten to release).", (int32_t)log_fd);
        }
        return;
    }
    // 防止执行 ev_unlisten 的时候 _olp_post_accept 里面还未设置 olacp->overlap.fd，造成该新的fd不能关闭
    if (0 != ATOMIC_GET(&lsn->remove)) {
        CLOSE_SOCK(olacp->overlap.fd);
    }
    // 此时 olacp 已成功重投 AcceptEx（ref 不变），fd 是本次接受的连接套接字。
    // 以下若失败只需关闭 fd，无需触碰 ref。
    if (ERR_OK != setsockopt(fd,
                             SOL_SOCKET,
                             SO_UPDATE_ACCEPT_CONTEXT,
                             (char *)&lsn->fd,
                             (int32_t)sizeof(lsn->fd))
        || ERR_OK != _evpub_nodelay_nonblock(fd)
        || ERR_OK != sock_keepalive(fd, KEEPALIVE_TIME, KEEPALIVE_INTERVAL)) {
        CLOSE_SOCK(fd);
        return;
    }
    // 跨 watcher 投递前 ref++ 占位：防 ev_unlisten 在目标 watcher 取出
    // CMD_ADDACP 前将 lsn ref 减到 0 释放，_on_cmd_addacp/_iocp_free_cmd 配对减
    ATOMIC_ADD(&lsn->ref, 1);
    _cmd_add_acpfd(GET_PTR(acpctx->ev->watcher, acpctx->ev->nthreads, fd), fd, lsn);
}
void _iocp_add_acpfd_inloop(watcher_ctx *watcher, SOCKET fd, listener_ctx *lsn) {
    if (ERR_OK != _iocp_join(watcher, fd)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        CLOSE_SOCK(fd);
        return;
    }
    skpool_args skargs = { fd, &lsn->cbs, &lsn->ud };
    sock_ctx *skctx = pool_pop(&watcher->pool, &skargs);
    overlap_tcp_ctx *oltcp = UPCAST(skctx, overlap_tcp_ctx, ol_r);
    _evpub_sockel_add(watcher, skctx);
    /* _iocp_post_recv 在 _olp_ssl_exchange（设置 STATUS_AUTHSSL）之前投递，但不存在竞态：
     * _iocp_join 将 fd 绑定到本 watcher 的 IOCP，fd 上的所有完成事件只投递到该队列；
     * 本 watcher 线程此刻正在执行本回调，不会再次进入 GetQueuedCompletionStatus，
     * 因此 WSARecv 完成只有在本函数返回后才能被取到，届时 STATUS_AUTHSSL 已设置。*/
    if (ERR_OK != _iocp_post_recv(&oltcp->ol_r, &oltcp->bytes_r, &oltcp->flag, &oltcp->wsabuf, 1)) {
        _evpub_sockel_remove(watcher, skctx->fd);
        pool_push(&watcher->pool, skctx);
        return;
    }
    if (ERR_OK != _olp_call_acp_cb(watcher->ev, oltcp)) {
        _iocp_disconnect(skctx, 1);
        return;
    }
#if WITH_SSL
    if (NULL != lsn->evssl) {
        if (ERR_OK != _olp_ssl_exchange(watcher, oltcp, lsn->evssl)) {
            _iocp_disconnect(skctx, 1);
            return;
        }
    }
#endif
}
// 关闭前cnt个AcceptEx挂起的socket（取消未完成的AcceptEx操作）
static void _olp_free_acceptex(listener_ctx *lsn, int32_t cnt) {
    for (int32_t i = 0; i < cnt; i++) {
        CLOSE_SOCK(lsn->overlap_acpt[i].overlap.fd);
    }
}
// 将监听socket关联到AcceptEx IOCP并预挂MAX_ACCEPTEX_CNT个AcceptEx操作
static int32_t _olp_acceptex(ev_ctx *ev, listener_ctx *lsn) {
    // 占位先 +1 提前到任何失败点之前：_olp_acceptex 失败时 ref 始终 >= 1，
    // ev_listen 失败路径统一减占位释放；同时保证 cb 任何路径减 ref 不会下溢
    ATOMIC_SET(&lsn->ref, 1);
    if (NULL == CreateIoCompletionPort((HANDLE)lsn->fd, ev->acpex[0].iocp, 0, ev->nacpex)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    overlap_acpt_ctx *olacp;
    for (int32_t i = 0; i < MAX_ACCEPTEX_CNT; i++) {
        olacp = &lsn->overlap_acpt[i];
        olacp->lsn = lsn;
        olacp->overlap.fd = INVALID_SOCK;
        olacp->overlap.ev_cb = _olp_on_accept_cb;
        // 投递前 +1：客户端在 ev_listen 窗口内连接触发 cb 时，ref 已包含本槽位
        ATOMIC_ADD(&lsn->ref, 1);
        if (ERR_OK != _olp_post_accept(olacp)) {
            // 投递失败回退本槽位；保留占位(1) + 前 i 个成功槽位。
            // _olp_free_acceptex 关 fd 触发取消 → cb 走 path 1 减 ref；remove=1 强制释放路径
            ATOMIC_ADD(&lsn->ref, -1);
            ATOMIC_SET(&lsn->remove, 1);
            _olp_free_acceptex(lsn, i);
            return ERR_FAILED;
        }
    }
    return ERR_OK;
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
    SOCKET fd = _evpub_listen(&addr);
    if (INVALID_SOCK == fd) {
        LOG_ERROR("listen %s:%d error.", ip, port);
        UD_FREE(cbs->ud_free, ud);
        return ERR_FAILED;
    }
    listener_ctx *lsn;
    // 显式初始化所有 overlap_acpt[].overlap.fd = INVALID_SOCK，CreateIoCompletionPort 失败走 _iocp_freelsn 时
    // _olp_free_acceptex 中 CLOSE_SOCK 仅跳过 INVALID_SOCK，避免脏值（含 fd=0）被误关
    CALLOC(lsn, 1, sizeof(listener_ctx));
    for (int32_t i = 0; i < MAX_ACCEPTEX_CNT; i++) {
        lsn->overlap_acpt[i].overlap.fd = INVALID_SOCK;
    }
    lsn->family = netaddr_family(&addr);
    lsn->fd = fd;
    lsn->cbs = *cbs;
    COPY_UD(lsn->ud, ud);
#if WITH_SSL
    lsn->evssl = evssl;
#else
    (void)evssl;
#endif
    if (ERR_OK != _olp_acceptex(ctx, lsn)) {
        // _olp_acceptex 在 CreateIoCompletionPort 之前已占位 ref=1，失败时 ref 始终 >= 1。
        // 减占位仲裁释放：cb 全减完时此处减到 0 释放，否则由最后一个 cb 释放。
        // _iocp_freelsn 内已 ud_free(lsn->ud)(浅拷贝同一资源),此处不再调业务侧 ud_free 避免 double-free
        _iocp_try_freelsn(lsn);
        return ERR_FAILED;
    }
    lsn->id = createid();
    spin_lock(&ctx->spin);
    array_push_back(&ctx->arrlsn, &lsn);
    spin_unlock(&ctx->spin);
    SET_PTR(id, lsn->id);
    return ERR_OK;
}
void _iocp_freelsn(listener_ctx *lsn) {
    if (0 == ATOMIC_GET(&lsn->remove)) {
        _olp_free_acceptex(lsn, MAX_ACCEPTEX_CNT);
    }
    CLOSE_SOCK(lsn->fd);
    UD_FREE(lsn->cbs.ud_free, &lsn->ud);
    FREE(lsn);
}
// 递减 lsn 引用计数；归零时释放 listener_ctx。listener_ctx 完整定义仅在本文件可见，
// 跨文件路径（cmds.c::_on_cmd_addacp / iocp.c::_iocp_free_cmd）须经此封装访问 lsn->ref
void _iocp_try_freelsn(listener_ctx *lsn) {
    if (1 == ATOMIC_ADD(&lsn->ref, -1)) {
        _iocp_freelsn(lsn);
    }
}
// 根据id从arrlsn中查找并移除listener_ctx（加自旋锁保护）
static listener_ctx * _olp_get_listener(ev_ctx *ctx, uint64_t id) {
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
    listener_ctx *lsn = _olp_get_listener(ctx, id);
    if (NULL == lsn) {
        return;
    }
    // remove=1 必须在 SOCK_CLOSE 之前置位：closesocket 全屏障保证 cb 取到取消完成时已见 remove==1
    ATOMIC_SET(&lsn->remove, 1);
    for (int32_t i = 0; i < MAX_ACCEPTEX_CNT; i++) {
        SOCK_CLOSE(lsn->overlap_acpt[i].overlap.fd);
    }
    // 减占位 ref；cb 都已完成时此处减到 0 释放，否则由最后一个 cb 释放
    _iocp_try_freelsn(lsn);
}
// 提交WSARecvFrom异步UDP接收请求
static int32_t _olp_post_recv_from(overlap_udp_ctx *oludp) {
    ZERO(&oludp->ol_r.overlapped, sizeof(oludp->ol_r.overlapped));
    oludp->flag = oludp->bytes_r = 0;
    oludp->addrlen = netaddr_size(&oludp->addr);
    if (ERR_OK != WSARecvFrom(oludp->ol_r.fd,
                              &oludp->wsabuf_r,
                              1,
                              &oludp->bytes_r,
                              &oludp->flag,
                              netaddr_addr(&oludp->addr),
                              &oludp->addrlen,
                              &oludp->ol_r.overlapped,
                              NULL)) {
        if (WSA_IO_PENDING != ERRNO) {
            return ERR_FAILED;
        }
    }
    return ERR_OK;
}
// 处理UDP接收侧关闭：若正在发送则标记延迟，否则直接移除并释放
static void _olp_on_udp_close_r(watcher_ctx *watcher, overlap_udp_ctx *oludp) {
    if (BIT_CHECK(oludp->status, STATUS_SENDING)) {
        BIT_SET(oludp->status, STATUS_REMOVE);
    } else {
        _evpub_sockel_remove(watcher, oludp->ol_r.fd);
        _iocp_free_udp(&oludp->ol_r);
    }
}
// WSARecvFrom完成回调：触发recvfrom回调并重新提交接收
static void _olp_on_recvfrom_cb(watcher_ctx *watcher, sock_ctx *skctx, DWORD bytes) {
    overlap_udp_ctx *oludp = UPCAST(skctx, overlap_udp_ctx, ol_r);
    if (BIT_CHECK(oludp->status, STATUS_ERROR)) {
        _olp_on_udp_close_r(watcher, oludp);
        return;
    }
    if (0 != oludp->ol_r.overlapped.Internal) {
        // 超大 datagram WSAEMSGSIZE 截断等软错误：告警丢弃，不关 socket，重新提交接收（与 POSIX UDP 对齐）
        LOG_WARN("UDP recvfrom error on fd %d, dropped (socket kept).", (int32_t)oludp->ol_r.fd);
    } else {
        _olp_call_recvfrom_cb(watcher->ev, oludp, (size_t)bytes);
        //防止_olp_call_recvfrom_cb 里面关掉
        if (BIT_CHECK(oludp->status, STATUS_ERROR)) {
            _olp_on_udp_close_r(watcher, oludp);
            return;
        }
    }
    if (ERR_OK != _olp_post_recv_from(oludp)) {
        BIT_SET(oludp->status, STATUS_ERROR);
        _olp_on_udp_close_r(watcher, oludp);
    }
}
// 提交WSASendTo异步UDP发送请求（buf中包含netaddr_ctx + 数据）
static int32_t _olp_post_sendto(overlap_udp_ctx *oludp, off_buf_ctx *buf) {
    ZERO(&oludp->ol_s.overlapped, sizeof(oludp->ol_s.overlapped));
    oludp->bytes_s = 0;
    netaddr_ctx *addr = (netaddr_ctx *)buf->data;
    oludp->wsabuf_s.IOV_PTR_FIELD = (char *)buf->data + sizeof(netaddr_ctx);
    oludp->wsabuf_s.IOV_LEN_FIELD = (IOV_LEN_TYPE)buf->lens;
    if (ERR_OK != WSASendTo(oludp->ol_s.fd,
                            &oludp->wsabuf_s,
                            1,
                            &oludp->bytes_s,
                            0,
                            netaddr_addr(addr),
                            netaddr_size(addr),
                            &oludp->ol_s.overlapped,
                            NULL)) {
        if (ERROR_IO_PENDING != ERRNO) {
            return ERR_FAILED;
        }
    }
    return ERR_OK;
}
// WSASendTo完成回调：释放当前缓冲区，继续发送队列中下一条或清除发送标志
static void _olp_on_sendto_cb(watcher_ctx *watcher, sock_ctx *skctx, DWORD bytes) {
    overlap_udp_ctx *oludp = UPCAST(skctx, overlap_udp_ctx, ol_s);
    void *data = oludp->wsabuf_s.IOV_PTR_FIELD - sizeof(netaddr_ctx);
    FREE(data);
    if (BIT_CHECK(oludp->status, STATUS_ERROR)) {
        if (BIT_CHECK(oludp->status, STATUS_REMOVE)) {
            _evpub_sockel_remove(watcher, oludp->ol_r.fd);
            _iocp_free_udp(&oludp->ol_r);
        } else {
            BIT_REMOVE(oludp->status, STATUS_SENDING);
        }
        return;
    }
    if (0 != oludp->ol_s.overlapped.Internal) {
        BIT_REMOVE(oludp->status, STATUS_SENDING);
        _iocp_disconnect(&oludp->ol_r, 1);
        return;
    }
    if (0 == queue_size(&oludp->buf_s)) {
        BIT_REMOVE(oludp->status, STATUS_SENDING);
        return;
    }
    off_buf_ctx *sendbuf = queue_pop(&oludp->buf_s);
    oludp->wb_size -= sendbuf->lens;
    if (ERR_OK != _olp_post_sendto(oludp, sendbuf)) {
        _evpub_off_buf_release(sendbuf);
        BIT_REMOVE(oludp->status, STATUS_SENDING);
        _iocp_disconnect(&oludp->ol_r, 1);
    }
}
void _iocp_add_bufs_trysendto(sock_ctx *skctx, off_buf_ctx *buf) {
    overlap_udp_ctx *oludp = UPCAST(skctx, overlap_udp_ctx, ol_r);
    // UDP 队列超阈值丢 datagram 不断 fd
    if (0 != MAX_SENDQ_CNT
        && queue_size(&oludp->buf_s) >= MAX_SENDQ_CNT) {
        LOG_WARN("UDP send queue overflow on fd %d (>= %d), drop datagram.",
                 (int32_t)oludp->ol_s.fd, MAX_SENDQ_CNT);
        _evpub_off_buf_release(buf);
        return;
    }
    oludp->wb_size += buf->lens;
    if (tda_check(&oludp->tda, oludp->wb_size)) {
        LOG_WARN("UDP send buf growing on fd %d: %zu bytes.",
                 (int32_t)oludp->ol_s.fd, oludp->wb_size);
    }
    queue_push(&oludp->buf_s, buf);
    if (BIT_CHECK(oludp->status, STATUS_SENDING)
        || BIT_CHECK(oludp->status, STATUS_ERROR)) {
        return;
    }
    BIT_SET(oludp->status, STATUS_SENDING);
    off_buf_ctx *sendbuf = queue_pop(&oludp->buf_s);
    oludp->wb_size -= sendbuf->lens;
    if (ERR_OK != _olp_post_sendto(oludp, sendbuf)) {
        _evpub_off_buf_release(sendbuf);
        BIT_REMOVE(oludp->status, STATUS_SENDING);
        _iocp_disconnect(&oludp->ol_r, 1);
    }
}
// 分配并初始化UDP上下文（不使用对象池，因UDP不常关闭/新建）
static sock_ctx *_olp_new_udp(SOCKET fd, cbs_ctx *cbs, ud_cxt *ud) {
    overlap_udp_ctx *oludp;
    MALLOC(oludp, sizeof(overlap_udp_ctx));
    oludp->ol_r.type = SOCK_DGRAM;
    oludp->ol_r.fd = fd;
    oludp->ol_r.ev_cb = _olp_on_recvfrom_cb;
    oludp->ol_s.type = SOCK_DGRAM;
    oludp->ol_s.fd = fd;
    oludp->ol_s.ev_cb = _olp_on_sendto_cb;
    oludp->status = STATUS_NONE;
    oludp->skid = createid();
    oludp->cbs = *cbs;
    COPY_UD(oludp->ud, ud);
    netaddr_empty(&oludp->addr);
    oludp->addrlen = netaddr_size(&oludp->addr);
    oludp->wsabuf_r.buf = oludp->buf;
    oludp->wsabuf_r.len = sizeof(oludp->buf);
    queue_init(&oludp->buf_s, sizeof(off_buf_ctx), INIT_SENDBUF_LEN);
    oludp->wb_size = 0;
    tda_init(&oludp->tda, WB_WARN_INIT_SIZE);
    return &oludp->ol_r;
}
void _iocp_free_udp(sock_ctx *skctx) {
    overlap_udp_ctx *oludp = UPCAST(skctx, overlap_udp_ctx, ol_r);
    CLOSE_SOCK(oludp->ol_r.fd);
    _evpub_off_buf_clear(&oludp->buf_s);
    queue_free(&oludp->buf_s);
    UD_FREE(oludp->cbs.ud_free, &oludp->ud);
    FREE(oludp);
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
    sock_ctx *skctx = _olp_new_udp(*fd, cbs, ud);
    overlap_udp_ctx *udp = UPCAST(skctx, overlap_udp_ctx, ol_r);
    *skid = udp->skid;
    _cmd_add(GET_PTR(ctx->watcher, ctx->nthreads, (*fd)), skctx);
    return ERR_OK;
}
void _iocp_add_fd_inloop(watcher_ctx *watcher, sock_ctx *skctx) {
    if (ERR_OK != _iocp_join(watcher, skctx->fd)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        if (SOCK_STREAM == skctx->type) {
            pool_push(&watcher->pool, skctx);
        } else {
            _iocp_free_udp(skctx);
        }
        return;
    }
    _evpub_sockel_add(watcher, skctx);
    if (SOCK_STREAM == skctx->type) {
        overlap_tcp_ctx *tcp = UPCAST(skctx, overlap_tcp_ctx, ol_r);
        if (ERR_OK != _iocp_post_recv(&tcp->ol_r, &tcp->bytes_r, &tcp->flag, &tcp->wsabuf, 1)) {
            _evpub_sockel_remove(watcher, skctx->fd);
            pool_push(&watcher->pool, skctx);
        }
    } else {
        overlap_udp_ctx *udp = UPCAST(skctx, overlap_udp_ctx, ol_r);
        if (ERR_OK != _olp_post_recv_from(udp)) {
            _evpub_sockel_remove(watcher, skctx->fd);
            _iocp_free_udp(skctx);
        }
    }
}

#endif
