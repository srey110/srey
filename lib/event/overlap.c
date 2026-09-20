#include "event/event.h"
#include "event/iocp.h"
#include "utils/pool.h"
#include "utils/buffer.h"
#include "utils/netutils.h"
#include "utils/tda.h"
#include "containers/hashmap.h"

#ifdef EV_IOCP

#define REVIVE_SNAP_CAP 128// _olp_revive_dead 单轮最多快照复活的 listener 数，超出者下轮扫描补（实际 listener 数远小于此）
#define MAX_ACCEPTEX_CNT    128// 每个监听socket同时挂起的AcceptEx数量
#define ACCEPTEX_ADDR_LEN   (sizeof(struct sockaddr_storage) + 16)// AcceptEx 每段地址长度（Microsoft 要求 ≥ sizeof(sockaddr_XX) + 16）

// AcceptEx单个挂起操作的上下文
typedef struct overlap_acpt_ctx {
    evsock_ctx overlap;                       // 嵌入evsock_ctx，供IOCP回调使用
    struct listener_ctx *lsn;               // 所属监听器
    DWORD bytes;                            // AcceptEx接收到的字节数
    atomic_t dead;                          // 1=re-post失败已放弃,等 _olp_try_revive_slot 用CAS认领复活
    char addr[ACCEPTEX_ADDR_LEN * 2];      // 存储本地/对端地址的缓冲区
}overlap_acpt_ctx;
// IOCP监听器上下文
typedef struct listener_ctx {
    int32_t family;                                 // 地址族（AF_INET/AF_INET6）
    atomic_t remove;                                // 标记为待移除（ev_unlisten后设置）
    atomic_t ref;                                   // 引用计数（等于挂起的AcceptEx数量）
    atomic_t ndead;                                 // 当前dead槽位数,==0时_olp_try_revive_slot跳过整个扫描
    SOCKET fd;                                      // 监听socket句柄
#if WITH_SSL
    evssl_ctx *evssl;                               // SSL上下文（NULL表示不使用SSL）
#endif
    cbs_ctx cbs;                                    // 回调函数集合
    ud_cxt ud;                                      // 用户数据模板（每个accept连接复制一份）
    uint64_t id;                                    // 监听器唯一ID（ev_unlisten使用）
    struct ev_ctx *ev;                              // 所属ev_ctx（_iocp_freelsn递减存活计数nlsn）
    overlap_acpt_ctx overlap_acpt[MAX_ACCEPTEX_CNT]; // 预挂起的AcceptEx数组
}listener_ctx;
// IOCP TCP连接上下文（读写各用一个evsock_ctx / OVERLAPPED）
typedef struct overlap_tcp_ctx {
    evsock_ctx ol_r;          // 读操作的OVERLAPPED上下文
    evsock_ctx ol_s;          // 写操作的OVERLAPPED上下文
    int32_t status;         // 连接状态标志位（sock_status组合）
    DWORD bytes_r;          // WSARecv接收字节数
    DWORD bytes_s;          // WSASend发送字节数
    DWORD flag;             // WSARecv标志
#if WITH_SSL
    SSL *ssl;               // SSL会话（NULL表示普通TCP）
    struct evssl_ctx *evssl; // 待升级的SSL上下文（发送完毕后升级）
    uint64_t wpend_ms;      // STATUS_WPEND_SSL 的零进展起点，仅该位置位期间有效
    list_node wpend_node;   // 挂 watcher->wpends；只在 STATUS_WPEND_SSL 置位期间在链上
#endif
    size_t wb_size;         // 当前 buf_s 中字节累计
    tda_ctx tda;            // 字节告警翻倍状态
    IOV_TYPE wsabuf;        // WSARecv缓冲区描述符
    buffer_ctx buf_r;       // 接收缓冲区
    queue_ctx buf_s;        // 发送队列
    cbs_ctx cbs;            // 回调函数集合
    ud_cxt ud;              // 用户数据
}overlap_tcp_ctx;
// IOCP UDP连接上下文
typedef struct overlap_udp_ctx {
    evsock_ctx ol_r;          // 接收操作的OVERLAPPED上下文
    evsock_ctx ol_s;          // 发送操作的OVERLAPPED上下文
    int32_t addrlen;        // netaddr_ctx中地址结构的长度
    int32_t status;         // 状态标志位
    uint32_t retry_ms;      // 发送重试当前退避间隔(ms)
    DWORD bytes_r;          // 接收字节数
    DWORD bytes_s;          // 发送字节数
    DWORD flag;             // WSARecvFrom标志
    size_t wb_size;         // 当前 buf_s 中字节累计
    uint64_t retry_until;   // 发送重试的下次到期时刻（ms 绝对时间），0=未退避
    watcher_ctx *watcher;   // 所属 watcher（发送重试 tick 自摘用）
    ev_tick send_tick;      // 发送资源紧张时的重试节点（cb 非 NULL 表示已挂）
    tda_ctx tda;            // 字节告警翻倍状态
    cbs_ctx cbs;            // 回调函数集合
    IOV_TYPE wsabuf_s;      // 发送缓冲区描述符
    IOV_TYPE wsabuf_r;      // 接收缓冲区描述符（指向buf）
    queue_ctx buf_s;        // 发送队列
    netaddr_ctx addr_r;     // 接收到的对端地址（WSARecvFrom填充）
    netaddr_ctx addr_s;     // 在途 WSASendTo 目标地址（异步期间须持久,不可指向队列元素）
    ud_cxt ud;              // 用户数据
    char buf[MAX_RECVFROM_SIZE]; // 固定接收缓冲区
}overlap_udp_ctx;

static void _olp_on_recv_cb(watcher_ctx *watcher, evsock_ctx *evsk, DWORD bytes); // 前向声明：TCP接收完成回调
static void _olp_on_send_cb(watcher_ctx *watcher, evsock_ctx *evsk, DWORD bytes); // 前向声明：TCP发送完成回调
static int32_t _olp_sendto_drain(watcher_ctx *watcher, overlap_udp_ctx *oludp); // 前向声明：与下面的重试 tick 互相调用

void _iocp_sk_shutdown(evsock_ctx *evsk) {
#if WITH_SSL
    overlap_tcp_ctx *oltcp = UPCAST(evsk, overlap_tcp_ctx, ol_r);
    evssl_shutdown(oltcp->ssl, oltcp->ol_r.sk.fd);
#else
    shutdown(evsk->sk.fd, SHUT_RD);
#endif
}
void *_evpub_sk_new(void *args) {
    skpool_args *skargs = (skpool_args *)args;
    overlap_tcp_ctx *oltcp;
    MALLOC(oltcp, sizeof(overlap_tcp_ctx));
    oltcp->ol_r.type = SOCK_STREAM;
    oltcp->ol_r.sk = skargs->sk;// 带入 fd 与 index,skid 随后由 createid 覆写
    oltcp->ol_r.sk.skid = createid();
    oltcp->ol_r.ev_cb = _olp_on_recv_cb;
    oltcp->ol_s.type = SOCK_STREAM;
    oltcp->ol_s.sk = oltcp->ol_r.sk;// 读写两个 OVERLAPPED 共用同一连接标识,ol_r 为权威
    oltcp->ol_s.ev_cb = _olp_on_send_cb;
    oltcp->status = STATUS_NONE;
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
    overlap_tcp_ctx *oltcp = UPCAST((evsock_ctx *)sk, overlap_tcp_ctx, ol_r);
#if WITH_SSL
    FREE_SSL(oltcp->ssl);
#endif
    CLOSE_SOCK(oltcp->ol_r.sk.fd);
    buffer_free(&oltcp->buf_r);
    _evpub_off_buf_clear(&oltcp->buf_s);
    queue_free(&oltcp->buf_s);
    UD_FREE(oltcp->cbs.ud_free, &oltcp->ud);
    FREE(oltcp);
}
void _evpub_sk_clear(void *sk)  {
    overlap_tcp_ctx *oltcp = UPCAST((evsock_ctx *)sk, overlap_tcp_ctx, ol_r);
    oltcp->status = STATUS_NONE;
#if WITH_SSL
    FREE_SSL(oltcp->ssl);
    oltcp->evssl = NULL;
#endif
    CLOSE_SOCK(oltcp->ol_r.sk.fd);
    oltcp->ol_s.sk.fd = INVALID_SOCK;
    _evpub_off_buf_clear(&oltcp->buf_s);
    // clear 只归零不缩容,背压期涨上去的环会跟着对象一直待在池里,回池这步缩回初始容量
    if (queue_maxsize(&oltcp->buf_s) > INIT_SENDBUF_LEN) {
        queue_resize(&oltcp->buf_s, INIT_SENDBUF_LEN);
    }
    oltcp->wb_size = 0;
    tda_init(&oltcp->tda, WB_WARN_INIT_SIZE);
    buffer_drain(&oltcp->buf_r, buffer_size(&oltcp->buf_r));
    UD_FREE(oltcp->cbs.ud_free, &oltcp->ud);
}
void _evpub_sk_reset(void *sk, void *args) {
    skpool_args *skargs = (skpool_args *)args;
    overlap_tcp_ctx *oltcp = UPCAST((evsock_ctx *)sk, overlap_tcp_ctx, ol_r);
    oltcp->ol_r.sk = skargs->sk;// 同 _evpub_sk_new
    oltcp->ol_r.sk.skid = createid();
    oltcp->ol_r.ev_cb = _olp_on_recv_cb;
    oltcp->ol_s.sk = oltcp->ol_r.sk;// 同 _evpub_sk_new
    oltcp->ol_s.ev_cb = _olp_on_send_cb;
    oltcp->cbs = *skargs->cbs;
    COPY_UD(oltcp->ud, skargs->ud);
}
ud_cxt *_iocp_get_ud(evsock_ctx *evsk) {
    if (SOCK_STREAM == evsk->type) {
        return &UPCAST(evsk, overlap_tcp_ctx, ol_r)->ud;
    } else {
        return &UPCAST(evsk, overlap_udp_ctx, ol_r)->ud;
    }
}
int32_t _iocp_check_skid(evsock_ctx *evsk, const uint64_t skid) {
    // listener / cmd channel 的 skid 恒 0 而业务 skid 最小为 1，故无需再按 type 排除，见 evsock_ctx.sk
    return skid == evsk->sk.skid ? ERR_OK : ERR_FAILED;
}
void _iocp_disconnect(evsock_ctx *evsk) {
    if (SOCK_STREAM == evsk->type) {
        overlap_tcp_ctx *tcp = UPCAST(evsk, overlap_tcp_ctx, ol_r);
        if (BIT_CHECK(tcp->status, STATUS_ERROR)) {
            return;
        }
        // ev_send 在本平台只是入队并投 0 字节探针,payload 此刻还在 buf_s,不冲就是整包丢
        _evpub_close_flush_tcp(tcp->ol_s.sk.fd, &tcp->buf_s, tcp->status, &tcp->wb_size, TCP_SSL(tcp));
        BIT_SET(tcp->status, STATUS_ERROR);
        _iocp_sk_shutdown(evsk);
        CancelIoEx((HANDLE)evsk->sk.fd, NULL);
    } else {
        // UDP datagram 无连接,没有待发队列要冲
        overlap_udp_ctx *udp = UPCAST(evsk, overlap_udp_ctx, ol_r);
        if (BIT_CHECK(udp->status, STATUS_ERROR)) {
            return;
        }
        BIT_SET(udp->status, STATUS_ERROR);
        CancelIoEx((HANDLE)evsk->sk.fd, NULL);
    }
}
// 调用accept回调，返回值非ERR_OK则拒绝连接
static inline int32_t _olp_call_acp_cb(ev_ctx *ev, overlap_tcp_ctx *oltcp) {
    if (NULL != oltcp->cbs.acp_cb) {
        return oltcp->cbs.acp_cb(ev, &oltcp->ol_r.sk, &oltcp->ud);
    }
    return ERR_OK;
}
// 调用connect回调，返回值非ERR_OK则断开连接
static inline int32_t _olp_call_conn_cb(ev_ctx *ev, overlap_tcp_ctx *oltcp, int32_t err) {
    if (NULL != oltcp->cbs.conn_cb) {
        return oltcp->cbs.conn_cb(ev, &oltcp->ol_r.sk, err, &oltcp->ud);
    }
    return ERR_OK;
}
#if WITH_SSL
// 调用SSL握手完成回调，口径同 usock.c 的 _usk_call_ssl_exchanged_cb
static inline int32_t _olp_call_ssl_exchanged_cb(ev_ctx *ev, overlap_tcp_ctx *oltcp) {
    if (NULL != oltcp->cbs.exch_cb) {
        return oltcp->cbs.exch_cb(ev, &oltcp->ol_r.sk, SOCK_IS_CLIENT(oltcp->status), &oltcp->ud, TCP_SSL(oltcp));
    }
    return ERR_OK;
}
#endif
// 调用数据接收回调（nread > 0 才触发）
static inline void _olp_call_recv_cb(ev_ctx *ev, overlap_tcp_ctx *oltcp, size_t nread) {
    if (nread > 0) {
        oltcp->cbs.r_cb(ev, &oltcp->ol_r.sk, SOCK_IS_CLIENT(oltcp->status), &oltcp->buf_r, nread, &oltcp->ud);
    }
}
// 调用发送完成回调（nsend > 0 且有s_cb 才触发）
static inline void _olp_call_send_cb(ev_ctx *ev, overlap_tcp_ctx *oltcp, size_t nsend) {
    if (NULL != oltcp->cbs.s_cb
        && nsend > 0) {
        oltcp->cbs.s_cb(ev, &oltcp->ol_r.sk, SOCK_IS_CLIENT(oltcp->status), nsend, &oltcp->ud);
    }
}
// 调用连接关闭回调
static inline void _olp_call_close_cb(ev_ctx *ev, overlap_tcp_ctx *oltcp) {
    if (NULL != oltcp->cbs.c_cb) {
        oltcp->cbs.c_cb(ev, &oltcp->ol_r.sk, SOCK_IS_CLIENT(oltcp->status),
                        _evpub_close_type(oltcp->status), &oltcp->ud);
    }
}
// 调用UDP关闭回调。UDP 无 FIN 可言，STATUS_PEER_* 永不置位，close_type 恒为 LOCAL
static inline void _olp_call_udp_close_cb(ev_ctx *ev, overlap_udp_ctx *oludp) {
    if (NULL != oludp->cbs.c_cb) {
        oludp->cbs.c_cb(ev, &oludp->ol_r.sk, 0, _evpub_close_type(oludp->status), &oludp->ud);
    }
}
// 调用UDP接收回调；0 字节 datagram 由本函数过滤不向上抛（_olp_on_recvfrom_cb 仍不视为 EOF，
// 继续 _olp_post_recv_from 接收下一包），避免上层处理空 payload 的特殊路径
static inline void _olp_call_recvfrom_cb(ev_ctx *ev, overlap_udp_ctx *oludp, size_t nread) {
    if (nread > 0) {
        oludp->cbs.rf_cb(ev, &oludp->ol_r.sk, oludp->wsabuf_r.buf, nread, &oludp->addr_r, &oludp->ud);
    }
}
// 提交WSARecv异步接收请求（用于TCP和命令管道）
int32_t _iocp_post_recv(evsock_ctx *evsk, DWORD *bytes, DWORD *flag, IOV_TYPE *wsabuf, DWORD niov) {
    *flag = 0;
    *bytes = 0;
    ZERO(&evsk->overlapped, sizeof(evsk->overlapped));
    if (ERR_OK != WSARecv(evsk->sk.fd,
                          wsabuf,
                          niov,
                          bytes,
                          flag,
                          &evsk->overlapped,
                          NULL)) {
        if (ERROR_IO_PENDING != ERRNO) {
            return ERR_FAILED;
        }
    }
    return ERR_OK;
}
static inline int32_t _olp_post_recv(overlap_tcp_ctx *oltcp) {
    return _iocp_post_recv(&oltcp->ol_r, &oltcp->bytes_r, &oltcp->flag, &oltcp->wsabuf, 1);
}
// 提交WSASend异步发送请求
// 0 字节 wakeup：
// 成功 → kernel 有空间 → continue
// EWOULDBLOCK → kernel 满 → 0 字节 post pend → 等到 buffer 空出空间触发完成 → continue
static inline int32_t _olp_post_send(overlap_tcp_ctx *oltcp) {
    oltcp->bytes_s = 0;
    ZERO(&oltcp->ol_s.overlapped, sizeof(oltcp->ol_s.overlapped));
    if (ERR_OK != WSASend(oltcp->ol_s.sk.fd,
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
static inline int32_t _olp_wantwrite(overlap_tcp_ctx *oltcp) {
    if (!BIT_CHECK(oltcp->status, STATUS_SENDING)) {
        BIT_SET(oltcp->status, STATUS_SENDING);
        if (ERR_OK != _olp_post_send(oltcp)) {
            BIT_REMOVE(oltcp->status, STATUS_SENDING);
            return ERR_FAILED;
        }
    }
    return ERR_OK;
}
#if WITH_SSL
// SSL 握手；1 继续执行 return；0：返回
static int32_t _olp_ssl_do_handshake(watcher_ctx *watcher, overlap_tcp_ctx *oltcp, int32_t *err, int32_t isrecv) {
    *err = ERR_OK;
    int32_t rtn = BIT_CHECK(oltcp->status, STATUS_CLIENT) ?
                  evssl_tryconn(oltcp->ssl) :
                  evssl_tryacpt(oltcp->ssl);
    switch (rtn) {
    case ERR_OK://完成
        if (ERR_OK != _olp_call_ssl_exchanged_cb(watcher->ev, oltcp)) {
            *err = ERR_FAILED;
            return 1;
        }
        BIT_REMOVE(oltcp->status, STATUS_AUTHSSL);
        return 1;
    case 1://WANT_READ
        if (isrecv) {
            if (ERR_OK != _olp_post_recv(oltcp)) {
                *err = ERR_FAILED;
                return 1;
            }
        } else {
            BIT_REMOVE(oltcp->status, STATUS_SENDING);
        }
        return 0;
    case 2://WANT_WRITE
        if (isrecv) {
            if (ERR_OK != _olp_wantwrite(oltcp)) {
                *err = ERR_FAILED;
                return 1;
            }
            if (ERR_OK != _olp_post_recv(oltcp)) {
                *err = ERR_FAILED;
                return 1;
            }
        } else {
            if (ERR_OK != _olp_post_send(oltcp)) {
                *err = ERR_FAILED;
                return 1;
            }
        }
        return 0;
    default://错误
        *err = ERR_FAILED;
        return 1;
    }
}
#endif
// 从socket读取数据到接收缓冲区并触发recv回调，成功后重新提交WSARecv
static inline int32_t _olp_tcp_recv(watcher_ctx *watcher, overlap_tcp_ctx *oltcp) {
    size_t nread;
#if WITH_SSL
    // 有挂起的应用写就不能进 SSL_read(理由见 evpub.h 的 STATUS_WPEND_SSL)。
    // ol_r 不重投、只置 NORECV:发送探针在途,冲完由 _olp_tcp_send 恢复收
    if (BIT_CHECK(oltcp->status, STATUS_WPEND_SSL)) {
        BIT_SET(oltcp->status, STATUS_NORECV);
        return ERR_OK;
    }
#endif
    int32_t rtn = buffer_from_sock(&oltcp->buf_r, oltcp->ol_r.sk.fd, &nread, _evpub_sock_read, TCP_SSL(oltcp));
#if WITH_SSL
    if (ERR_OK == rtn
        && NULL != oltcp->ssl
        && !BIT_CHECK(oltcp->status, STATUS_SENDING) //忙则跟着业务自行完成
        && SSL_want_write(oltcp->ssl)) {// tls1.3 KeyUpdate探测
        BIT_SET(oltcp->status, STATUS_KEYUPDATE_WRITE);
    }
#endif
    _olp_call_recv_cb(watcher->ev, oltcp, nread);
    // 先记再往下走：下面 _olp_wantwrite / _olp_post_recv 会覆写 rtn，那是本地投递失败，不是对端关的
    if (ERR_OK != rtn) {
        _evpub_mark_close(&oltcp->status, rtn);
        return ERR_FAILED;// 分类已进 status, 不透传 evssl_* 的 1/2(口径同 _olp_tcp_send)
    }
    // 不 mark_close:这是本端按上限主动断,close_type 留在 LOCAL 档
    if (0 != _evpub_recvbuf_full(&oltcp->buf_r, oltcp->ol_r.sk.fd)) {
        return ERR_FAILED;
    }
#if WITH_SSL
    if (BIT_CHECK(oltcp->status, STATUS_KEYUPDATE_WRITE)) {// 处理 KeyUpdate 触发可写
        BIT_SET(oltcp->status, STATUS_NORECV);
        return _olp_wantwrite(oltcp);
    }
#endif
    return _olp_post_recv(oltcp);
}
#if WITH_SSL
// 挂起写看门狗，三者的职责、不变式与定义序的理由同 usock.c 的 _usk_wpend_unlink 那一块
static void _olp_wpend_unlink(watcher_ctx *watcher, overlap_tcp_ctx *oltcp) {
    if (!BIT_CHECK(oltcp->status, STATUS_WPEND_SSL)) {
        return;
    }
    BIT_REMOVE(oltcp->status, STATUS_WPEND_SSL);
    list_remove(&watcher->wpends, &oltcp->wpend_node);
}
static uint32_t _olp_wpend_tick(void *ud, uint64_t now_ms) {
    watcher_ctx *watcher = ud;
    list_node *head;
    overlap_tcp_ctx *oltcp;
    for (;;) {
        head = watcher->wpends.head;
        if (NULL == head) {
            break;
        }
        oltcp = UPCAST(head, overlap_tcp_ctx, wpend_node);
        if (now_ms - oltcp->wpend_ms < SSL_WPEND_MAX_MS) {
            return (uint32_t)(SSL_WPEND_MAX_MS - (now_ms - oltcp->wpend_ms));
        }
        LOG_WARN("ssl write pending %dms with no progress on fd %d, disconnect.",
                 SSL_WPEND_MAX_MS, (int32_t)oltcp->ol_r.sk.fd);
        _olp_wpend_unlink(watcher, oltcp);
        _evpub_mark_close(&oltcp->status, ERR_FAILED);
        _iocp_disconnect(&oltcp->ol_r);
    }
    watcher->wpend_tick.cb = NULL;
    _evpub_tick_remove(watcher, &watcher->wpend_tick);
    return EVENT_WAIT_TIMEOUT;
}
static void _olp_wpend_link(watcher_ctx *watcher, overlap_tcp_ctx *oltcp, int32_t progress) {
    if (BIT_CHECK(oltcp->status, STATUS_WPEND_SSL)) {
        if (0 == progress) {
            return;
        }
        list_remove(&watcher->wpends, &oltcp->wpend_node);
    } else {
        BIT_SET(oltcp->status, STATUS_WPEND_SSL);
    }
    oltcp->wpend_ms = timer_cur_ms(&watcher->timer);
    list_push_tail(&watcher->wpends, &oltcp->wpend_node);
    if (NULL == watcher->wpend_tick.cb) {
        watcher->wpend_tick.cb = _olp_wpend_tick;
        watcher->wpend_tick.ud = watcher;
        _evpub_tick_add(watcher, &watcher->wpend_tick);
    }
}
#endif
// 关闭TCP连接：若正在发送则标记延迟关闭，否则立即执行关闭回调并回收到对象池
static inline void _olp_on_recv_cb_err(watcher_ctx *watcher, overlap_tcp_ctx *oltcp) {
    BIT_SET(oltcp->status, STATUS_ERROR);
    if (BIT_CHECK(oltcp->status, STATUS_SENDING)) {
        BIT_SET(oltcp->status, STATUS_REMOVE);
    } else {
#if WITH_SSL
        // 对象回池前必须摘链,否则 watcher->wpends 留悬空节点(口径同 usock.c 的 _usk_close_tcp)
        _olp_wpend_unlink(watcher, oltcp);
#endif
        _olp_call_close_cb(watcher->ev, oltcp);
        _evpub_sockel_remove(watcher, oltcp->ol_r.sk.fd);
        pool_push(&watcher->pool, &oltcp->ol_r, 0);
    }
}
// IOCP TCP接收完成回调：处理SSL握手或普通数据接收
static void _olp_on_recv_cb(watcher_ctx *watcher, evsock_ctx *evsk, DWORD bytes) {
    overlap_tcp_ctx *oltcp = UPCAST(evsk, overlap_tcp_ctx, ol_r);
    // 本地已在关就不标 PEER_*，close_type 即 LOCAL。这道判定必须排在 iofail 之前——
    // ev_close 的 CancelIoEx 会让在途 ol_r 以"已取消"完成，iofail 同样为 1，颠倒就把本地关判成 ABORT
    if (BIT_CHECK(oltcp->status, STATUS_ERROR)) {
        _olp_on_recv_cb_err(watcher, oltcp);
        return;
    }
    // 完成状态非成功即 RST 一类的传输错；优雅 FIN 是"成功 + 0 字节"，落到下面 _olp_tcp_recv 去标
    if (ERROR_SUCCESS != oltcp->ol_r.overlapped.Internal) {
        _evpub_mark_close(&oltcp->status, ERR_FAILED);
        _olp_on_recv_cb_err(watcher, oltcp);
        return;
    }
#if WITH_SSL
    if (NULL != oltcp->ssl) {
        if (BIT_CHECK(oltcp->status, STATUS_AUTHSSL)) {// SSL握手
            int32_t err;
            if (!_olp_ssl_do_handshake(watcher, oltcp, &err, 1)) {
                return;
            }
            if (ERR_OK != err) {
                _olp_on_recv_cb_err(watcher, oltcp);
                return;
            }
        }
    }
#endif
    if (ERR_OK != _olp_tcp_recv(watcher, oltcp)) {
        _olp_on_recv_cb_err(watcher, oltcp);
        return;
    }
#if WITH_SSL
    // 方向 B 的出口，机制见 _olp_tcp_send 上方。KEYUPDATE_WRITE 时 ol_s 归探针、要先等它冲完；
    // STATUS_ERROR 时业务已在 recv_cb 里关了连接。
    // 失败必须走 _iocp_disconnect：此处 ol_r 已重投在途，而 _olp_on_recv_cb_err 在 !SENDING 时会立即 pool_push
    if (BIT_CHECK(oltcp->status, STATUS_KEYUPDATE_READ)
        && !BIT_CHECK(oltcp->status, STATUS_KEYUPDATE_WRITE)
        && !BIT_CHECK(oltcp->status, STATUS_ERROR)) {
        if (ERR_OK != _olp_wantwrite(oltcp)) {
            _iocp_disconnect(&oltcp->ol_r);
        }
    }
#endif
}
#if WITH_SSL
// 触发切ssl
static int32_t _olp_ssl_exchange_trigger(watcher_ctx *watcher, overlap_tcp_ctx *oltcp,
                                         struct evssl_ctx *evssl, int32_t *sending) {
    *sending = 0;
    oltcp->ssl = evssl_setfd(evssl, oltcp->ol_r.sk.fd);
    if (NULL == oltcp->ssl) {
        return ERR_FAILED;
    }
    if (BIT_CHECK(oltcp->status, STATUS_CLIENT)) {
        switch (evssl_tryconn(oltcp->ssl)) {
        case ERR_OK://握手完成
            return _olp_call_ssl_exchanged_cb(watcher->ev, oltcp);
        case 1://等待读就绪（WANT_READ）
            BIT_SET(oltcp->status, STATUS_AUTHSSL);
            return ERR_OK;
        case 2://等待写就绪（WANT_WRITE)
            *sending = 1;
            BIT_SET(oltcp->status, STATUS_AUTHSSL);
            if (ERR_OK != _olp_post_send(oltcp)) {
                *sending = 0;
                return ERR_FAILED;
            }
            return ERR_OK;
        default://错误
            return ERR_FAILED;
        }
    } else {
        BIT_SET(oltcp->status, STATUS_AUTHSSL);
        return ERR_OK;
    }
}
#endif
void _iocp_keyupdate(watcher_ctx *watcher, evsock_ctx *evsk, int32_t updatetype) {
#if WITH_SSL
    (void)watcher;
    if (SOCK_STREAM != evsk->type) {
        return;
    }
    overlap_tcp_ctx *oltcp = UPCAST(evsk, overlap_tcp_ctx, ol_r);
    // 口径同 usock.c 的 _uev_keyupdate
    if (NULL == oltcp->ssl
        || BIT_CHECK(oltcp->status, STATUS_AUTHSSL)
        || BIT_CHECK(oltcp->status, STATUS_ERROR)) {
        return;
    }
    if (ERR_OK != evssl_keyupdate(oltcp->ssl, updatetype)) {
        LOG_WARN("ssl keyupdate failed on fd %d.", (int32_t)evsk->sk.fd);
    }
#else
    (void)watcher;
    (void)evsk;
    (void)updatetype;
#endif
}
void _iocp_try_ssl_exchange(watcher_ctx *watcher, evsock_ctx *evsk, struct evssl_ctx *evssl, int32_t client) {
#if WITH_SSL
    if (SOCK_STREAM != evsk->type) {
        LOG_WARN("can't ssl exchange on udp.");
        return;
    }
    overlap_tcp_ctx *oltcp = UPCAST(evsk, overlap_tcp_ctx, ol_r);
    if (0 == _evpub_ssl_exchange_check(oltcp->ssl, &oltcp->status, client)) {
        return;
    }
    if (BIT_CHECK(oltcp->status, STATUS_SENDING)) {
        oltcp->evssl = evssl;
        BIT_SET(oltcp->status, STATUS_SSLEXCHANGE);// 标记发送队列为空则开始ssl握手
    } else {
        int32_t sending;
        if (ERR_OK != _olp_ssl_exchange_trigger(watcher, oltcp, evssl, &sending)) {
            _iocp_disconnect(evsk);
            LOG_ERROR("ssl exchange error.");
            return;
        }
        if (sending) {
            BIT_SET(oltcp->status, STATUS_SENDING);
        }
    }
#endif
}
// 排空 buf_s；没排完就投 0 字节探针接力。
// SSL 块是数据期 TLS1.3 的方向 B(SSL_write 说要先读)，与 usock.c 同条件同序：置 KEYUPDATE_READ
// 并交还 SENDING,靠 _olp_on_recv_cb 收到数据后接力。
// 顺序不能颠倒：SSL 块必须在 "0 == cnt" 之前,否则排空重试时走不到清位,残留的 KEYUPDATE_READ
// 会让 _iocp_add_bufs_trypost 永久拦住后续发送。
// 挂起态不变式：KEYUPDATE_READ=1 ⟹ ol_s 不在途 ∧ ol_r 在途 ∧ buf_s 非空
static inline int32_t _olp_tcp_send(watcher_ctx *watcher, overlap_tcp_ctx *oltcp) {
    size_t nsend;
    int32_t rtn = _evpub_sock_send(oltcp->ol_s.sk.fd, &oltcp->buf_s, &nsend, TCP_SSL(oltcp));
    oltcp->wb_size -= nsend;
    _olp_call_send_cb(watcher->ev, oltcp, nsend);
    if (ERR_OK != rtn) {
        _evpub_mark_close(&oltcp->status, rtn);
        return ERR_FAILED;
    }
    uint32_t cnt = queue_size(&oltcp->buf_s);
#if WITH_SSL
    if (NULL != oltcp->ssl) {
        // 先全清再按 SSL 在等哪一边挂一个:两档互斥,队列空则一个都不该挂
        int32_t waspend = BIT_CHECK(oltcp->status, STATUS_WPEND_SSL);
        BIT_REMOVE(oltcp->status, STATUS_KEYUPDATE_READ);
        int32_t wantr = 0;
        if (0 == cnt) {
            _olp_wpend_unlink(watcher, oltcp);
        } else {
            wantr = SSL_want_read(oltcp->ssl);
            if (0 == wantr
                && SSL_want_write(oltcp->ssl)) {
                _olp_wpend_link(watcher, oltcp, (0 != nsend) ? 1 : 0);
            } else {
                _olp_wpend_unlink(watcher, oltcp);
            }
        }
        // 之前为挂起写停了收,现在不挂了就恢复。必须排在下面早返回之前:
        // KEYUPDATE_READ 那一档靠读推进,收不恢复它就永远等不到
        if (0 != waspend
            && !BIT_CHECK(oltcp->status, STATUS_WPEND_SSL)
            && BIT_CHECK(oltcp->status, STATUS_NORECV)) {
            if (ERR_OK != _olp_post_recv(oltcp)) {
                return ERR_FAILED;
            }
            BIT_REMOVE(oltcp->status, STATUS_NORECV);
        }
        if (0 != wantr) {
            BIT_SET(oltcp->status, STATUS_KEYUPDATE_READ);
            BIT_REMOVE(oltcp->status, STATUS_SENDING);
            return ERR_OK;
        }
    }
#endif
    if (0 == cnt) {
#if WITH_SSL
        // 防止接收侧(_olp_tcp_recv)因 STATUS_SENDING 守卫未触发;
        // 此时 ol_r 仍在途(NORECV=0),投 ol_s 探针接力,等可写后由 flush 冲出
        if (NULL != oltcp->ssl
            && SSL_want_write(oltcp->ssl)) {
            if (ERR_OK != _olp_post_send(oltcp)) {
                return ERR_FAILED;
            }
            BIT_SET(oltcp->status, STATUS_KEYUPDATE_WRITE);
            return ERR_OK;// SENDING 保持,探针接力
        }
        if (BIT_CHECK(oltcp->status, STATUS_SSLEXCHANGE)) {// 数据发完，切ssl
            BIT_REMOVE(oltcp->status, STATUS_SSLEXCHANGE);
            int32_t sending;
            if (ERR_OK != _olp_ssl_exchange_trigger(watcher, oltcp, oltcp->evssl, &sending)) {
                LOG_ERROR("ssl exchange error.");
                return ERR_FAILED;
            }
            if (!sending) {
                BIT_REMOVE(oltcp->status, STATUS_SENDING);
            }
            return ERR_OK;
        }
#endif
        BIT_REMOVE(oltcp->status, STATUS_SENDING);
        return ERR_OK;
    }
    return _olp_post_send(oltcp);
}
#if WITH_SSL
// KeyUpdate 写就绪探针完成:重试 SSL_read 冲刷响应;没冲完重投探针,冲完按 NORECV 恢复 ol_r
static int32_t _olp_ssl_keyupdate_flush(watcher_ctx *watcher, overlap_tcp_ctx *oltcp) {
    size_t nread;
    int32_t rtn = buffer_from_sock(&oltcp->buf_r, oltcp->ol_r.sk.fd, &nread, _evpub_sock_read, oltcp->ssl);
    _olp_call_recv_cb(watcher->ev, oltcp, nread);
    if (ERR_OK != rtn) {
        _evpub_mark_close(&oltcp->status, rtn);
        return ERR_FAILED;
    }
    // 不 mark_close:这是本端按上限主动断,close_type 留在 LOCAL 档
    if (0 != _evpub_recvbuf_full(&oltcp->buf_r, oltcp->ol_r.sk.fd)) {
        return ERR_FAILED;
    }
    if (SSL_want_write(oltcp->ssl)) {// 没冲完,直接重投探针:SENDING 已持有,不走 wantwrite
        return _olp_post_send(oltcp);
    }
    BIT_REMOVE(oltcp->status, STATUS_KEYUPDATE_WRITE);
    if (!BIT_CHECK(oltcp->status, STATUS_NORECV)) {// 发侧（_olp_tcp_send）检测出的,此时 STATUS_NORECV = 0
        return ERR_OK;
    }
    rtn = _olp_post_recv(oltcp);// 收侧:暂停收已结束,重投恢复 ol_r
    if (ERR_OK == rtn) {
        BIT_REMOVE(oltcp->status, STATUS_NORECV);
    }
    return rtn;
}
#endif
static inline void _olp_send_close_tcp(watcher_ctx *watcher, overlap_tcp_ctx *oltcp) {
#if WITH_SSL
    int32_t norecv = BIT_CHECK(oltcp->status, STATUS_NORECV);
#else
    int32_t norecv = 0;
#endif
    if (0 != norecv ||
        BIT_CHECK(oltcp->status, STATUS_REMOVE)) {
#if WITH_SSL
        _olp_wpend_unlink(watcher, oltcp);
#endif
        _olp_call_close_cb(watcher->ev, oltcp);
        _evpub_sockel_remove(watcher, oltcp->ol_r.sk.fd);
        pool_push(&watcher->pool, &oltcp->ol_r, 0);
    } else {
        BIT_REMOVE(oltcp->status, STATUS_SENDING);
        _iocp_disconnect(&oltcp->ol_r);
    }
}
// IOCP TCP发送完成回调：消费发送队列，处理SSL升级，触发send回调
static void _olp_on_send_cb(watcher_ctx *watcher, evsock_ctx *evsk, DWORD bytes) {
    overlap_tcp_ctx *oltcp = UPCAST(evsk, overlap_tcp_ctx, ol_s);
    // 判定顺序同 _olp_on_recv_cb：先认本地关，再认传输错
    if (BIT_CHECK(oltcp->status, STATUS_ERROR)) {
        _olp_send_close_tcp(watcher, oltcp);
        return;
    }
    if (ERROR_SUCCESS != oltcp->ol_s.overlapped.Internal) {
        _evpub_mark_close(&oltcp->status, ERR_FAILED);
        _olp_send_close_tcp(watcher, oltcp);
        return;
    }
#if WITH_SSL
    if (NULL != oltcp->ssl) {
        if (BIT_CHECK(oltcp->status, STATUS_AUTHSSL)) {// SSL握手
            int32_t err;
            if (!_olp_ssl_do_handshake(watcher, oltcp, &err, 0)) {
                return;
            }
            if (ERR_OK != err) {
                _olp_send_close_tcp(watcher, oltcp);
                return;
            }
        } else {
            if (BIT_CHECK(oltcp->status, STATUS_KEYUPDATE_WRITE)) {// tls1.3 KeyUpdate 写就绪
                if (ERR_OK != _olp_ssl_keyupdate_flush(watcher, oltcp)) {
                    _olp_send_close_tcp(watcher, oltcp);
                    return;
                }
                if (BIT_CHECK(oltcp->status, STATUS_KEYUPDATE_WRITE)) {// 没冲完,探针已重投,等下次完成
                    return;
                }
            }
        }
    }
#endif
    if (ERR_OK != _olp_tcp_send(watcher, oltcp)) {
        _olp_send_close_tcp(watcher, oltcp);
        return;
    }
}
void _iocp_add_bufs_trypost(evsock_ctx *evsk, off_buf_ctx *buf) {
    overlap_tcp_ctx *oltcp = UPCAST(evsk, overlap_tcp_ctx, ol_r);
    // 已在关闭流程：拒收新数据
    if (BIT_CHECK(oltcp->status, STATUS_ERROR)) {
        _evpub_off_buf_release(buf);
        return;
    }
    if (!_evpub_sendqu_check_tcp(&oltcp->buf_s, oltcp->status, oltcp->ol_s.sk.fd)) {
        _evpub_off_buf_release(buf);
        _iocp_disconnect(&oltcp->ol_r);
        return;
    }
    oltcp->wb_size += buf->lens;
    _evpub_sendqu_tda(&oltcp->tda, oltcp->wb_size, oltcp->ol_s.sk.fd, 1);
    queue_push(&oltcp->buf_s, buf);
#if WITH_SSL
    // 方向 B 挂着未完成的 SSL_write，投探针只是空转；数据留队等 _olp_on_recv_cb 那次重试。
    // 对应 usock 的 _uev_add_bufs_send，那边还判 KEYUPDATE_WRITE——本平台方向 A 期 SENDING 必为 1，
    // 下面 _olp_wantwrite 的 !SENDING 已挡住
    if (BIT_CHECK(oltcp->status, STATUS_KEYUPDATE_READ)) {
        return;
    }
#endif
    if (ERR_OK != _olp_wantwrite(oltcp)) {
        _iocp_disconnect(&oltcp->ol_r);
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
    if (!_exfuncs.connectex(oltcp->ol_r.sk.fd,
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
static void _olp_on_connect_cb_err(watcher_ctx *watcher, overlap_tcp_ctx *oltcp) {
    _olp_call_conn_cb(watcher->ev, oltcp, ERR_FAILED);
    _evpub_sockel_remove(watcher, oltcp->ol_r.sk.fd);
    pool_push(&watcher->pool, &oltcp->ol_r, 0);
}
// ConnectEx完成回调：切换为读回调并提交WSARecv，触发conn回调，可选启动SSL
static void _olp_on_connect_cb(watcher_ctx *watcher, evsock_ctx *evsk, DWORD bytes) {
    evsk->ev_cb = _olp_on_recv_cb;
    overlap_tcp_ctx *oltcp = UPCAST(evsk, overlap_tcp_ctx, ol_r);
    if (BIT_CHECK(oltcp->status, STATUS_ERROR)
        || ERROR_SUCCESS != oltcp->ol_r.overlapped.Internal
        || ERR_OK != setsockopt(oltcp->ol_r.sk.fd, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0)) {
        _olp_on_connect_cb_err(watcher, oltcp);
        return;
    }
    BIT_SET(oltcp->status, STATUS_ESTABLISHED);
    if (ERR_OK != _evpub_tcp_keepalive(oltcp->ol_r.sk.fd)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        _olp_on_connect_cb_err(watcher, oltcp);
        return;
    }
#if WITH_SSL // 默认启用ssl，初始化
    if (NULL != oltcp->evssl) {
        oltcp->ssl = evssl_setfd(oltcp->evssl, oltcp->ol_r.sk.fd);
        if (NULL == oltcp->ssl) {
            _olp_on_connect_cb_err(watcher, oltcp);
            return;
        }
        BIT_SET(oltcp->status, STATUS_AUTHSSL);
    }
#endif
    if (ERR_OK != _olp_post_recv(oltcp)) {
        _olp_on_connect_cb_err(watcher, oltcp);
        return;
    }
    // 链接成功回调
    if (ERR_OK != _olp_call_conn_cb(watcher->ev, oltcp, ERR_OK)) {
        _iocp_disconnect(&oltcp->ol_r);
        return;
    }
#if WITH_SSL
    if (NULL != oltcp->ssl) {// 开始握手
        switch (evssl_tryconn(oltcp->ssl)) {
        case ERR_OK://握手完成
            BIT_REMOVE(oltcp->status, STATUS_AUTHSSL);
            if (ERR_OK != _olp_call_ssl_exchanged_cb(watcher->ev, oltcp)) {
                _iocp_disconnect(&oltcp->ol_r);
                return;
            }
            break;
        case 1:// WANT_READ
            break;
        case 2:// WANT_WRITE
            if (ERR_OK != _olp_wantwrite(oltcp)) {// 注册写
                _iocp_disconnect(&oltcp->ol_r);
                return;
            }
            break;
        default:
            _iocp_disconnect(&oltcp->ol_r);
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
    if (ERR_OK != _olp_trybind(sk->fd, netaddr_family(&addr))) {
        CLOSE_SOCK((sk->fd));
        UD_FREE(cbs->ud_free, ud);
        return ERR_FAILED;
    }
    skpool_args skargs = { .sk = { .fd = sk->fd, .index = (int32_t)CALC_WATCHER_INDEX(sk->fd, ctx->nthreads) },
                           .cbs = cbs, .ud = ud };
    evsock_ctx *evsk = (evsock_ctx *)_evpub_sk_new(&skargs);
    evsk->ev_cb = _olp_on_connect_cb;
    overlap_tcp_ctx *oltcp = UPCAST(evsk, overlap_tcp_ctx, ol_r);
    if (setsess) {
        oltcp->ud.sess = oltcp->ol_r.sk.skid;
    }
    BIT_SET(oltcp->status, STATUS_CLIENT);
    *sk = oltcp->ol_r.sk;// 整体带出 fd/index/skid
#if WITH_SSL
    oltcp->evssl = evssl;
#else
    (void)evssl;
#endif
    _cmd_connect(ctx, evsk, &addr);
    return ERR_OK;
}
void _iocp_add_conn_inloop(watcher_ctx *watcher, struct evsock_ctx *evsk, netaddr_ctx *addr) {
    overlap_tcp_ctx *oltcp = UPCAST(evsk, overlap_tcp_ctx, ol_r);
    if (ERR_OK != _iocp_join(watcher, evsk->sk.fd)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        _olp_call_conn_cb(watcher->ev, oltcp, ERR_FAILED);
        pool_push(&watcher->pool, &oltcp->ol_r, 0);
        return;
    }
    _evpub_sockel_add(watcher, evsk);
    if (ERR_OK != _olp_post_connect(oltcp, addr)) {
        _olp_call_conn_cb(watcher->ev, oltcp, ERR_FAILED);
        _evpub_sockel_remove(watcher, oltcp->ol_r.sk.fd);
        pool_push(&watcher->pool, &oltcp->ol_r, 0);
    }
}
// 原子取走 *pfd 并置为 INVALID_SOCK；ev_unlisten 与 AcceptEx 回调并发时只有一方拿到有效 fd
static inline SOCKET _olp_take_fd(SOCKET volatile *pfd) {
    return (SOCKET)(ULONG_PTR)InterlockedExchangePointer((volatile PVOID *)pfd,
        (PVOID)(ULONG_PTR)INVALID_SOCK);
}
static inline void _olp_take_close(SOCKET volatile *pfd) {
    SOCKET fd = _olp_take_fd(pfd);
    if (INVALID_SOCK != fd) {
        SOCK_CLOSE(fd);
    }
}
// 提交一个AcceptEx异步接受请求（创建新socket并挂起等待连接）
static inline int32_t _olp_post_accept(overlap_acpt_ctx *olacp) {
    // 这里就得设非阻塞:accept 完成回调只设 nodelay 与 keepalive,不再兜这一步
    SOCKET fd = sock_create_cloexec(olacp->lsn->family, SOCK_STREAM, 0, 1);
    if (INVALID_SOCK == fd) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    ZERO(&olacp->overlap.overlapped, sizeof(olacp->overlap.overlapped));
    olacp->bytes = 0;
    olacp->overlap.sk.fd = fd;
    if (!_exfuncs.acceptex(olacp->lsn->fd,//监听 socket
                           olacp->overlap.sk.fd,//预建的 accept socket
                           &olacp->addr,
                           0,
                           ACCEPTEX_ADDR_LEN,
                           ACCEPTEX_ADDR_LEN,
                           &olacp->bytes,
                           &olacp->overlap.overlapped)) {
        int32_t erro = ERRNO;
        if (ERROR_IO_PENDING != erro) {
            _olp_take_close(&olacp->overlap.sk.fd);
            LOG_ERROR("%s", ERRORSTR(erro));
            return ERR_FAILED;
        }
    }
    return ERR_OK;
}
// listener dead 槽计数与全局 ndead_total 单点同步，杜绝两者 drift
static inline void _olp_ndead_add(ev_ctx *ev, listener_ctx *lsn, int32_t delta) {
    ATOMIC_ADD(&lsn->ndead, delta);
    ATOMIC_ADD(&ev->ndead_total, delta);
}
// 重投该 listener 全部此前 _olp_post_accept 失败、已被放弃的槽位；
// 某槽重投再失败(fd 仍耗尽)即停止本轮(余下同样会失败)，留待下次 periodic 扫描
static void _olp_try_revive_slot(ev_ctx *ev, listener_ctx *lsn) {
    if (0 != ATOMIC_GET(&lsn->remove) || 0 == ATOMIC_GET(&lsn->ndead)) {
        return;
    }
    overlap_acpt_ctx *olacp;
    for (int32_t i = 0; i < MAX_ACCEPTEX_CNT; i++) {
        olacp = &lsn->overlap_acpt[i];
        if (!ATOMIC_CAS(&olacp->dead, 1, 0)) {
            continue;
        }
        _olp_ndead_add(ev, lsn, -1);
        ATOMIC_ADD(&lsn->ref, 1);
        if (ERR_OK != _olp_post_accept(olacp)) {
            ATOMIC_ADD(&lsn->ref, -1);
            ATOMIC_SET(&olacp->dead, 1);
            _olp_ndead_add(ev, lsn, 1);
            break;
        }
        if (0 != ATOMIC_GET(&lsn->remove)) {
            // _olp_post_accept 执行期间 ev_unlisten 可能运行但未能关闭新 fd(对齐 _olp_on_accept_cb 同一时机的处理)
            _olp_take_close(&olacp->overlap.sk.fd);
        }
    }
}
// dead 槽复活的唯一驱动（acpex 循环每 ACCEPT_BACKOFF_MS 调一次）：ndead_total==0 时免锁免遍历直接返回。
// 锁内只快照有 dead 槽的 listener 并各提 ref（纯内存），复活的 WSASocket/AcceptEx 系统调用移到锁外执行，
// 避免 128×N 次系统调用全程持锁阻塞 ev_listen/ev_unlisten；ref 保证快照期间 listener 不被 _iocp_freelsn 释放
void _olp_revive_dead(ev_ctx *ev) {
    if (0 == ATOMIC_GET(&ev->ndead_total)) {
        return;
    }
    listener_ctx *snap[REVIVE_SNAP_CAP];
    uint32_t cnt = 0;
    spin_lock(&ev->spin);
    uint32_t n = array_size(&ev->arrlsn);
    listener_ctx *lsn;
    for (uint32_t i = 0; i < n && cnt < REVIVE_SNAP_CAP; i++) {
        lsn = *(listener_ctx **)array_at(&ev->arrlsn, i);
        if (0 != ATOMIC_GET(&lsn->ndead)) {
            ATOMIC_ADD(&lsn->ref, 1);
            snap[cnt++] = lsn;
        }
    }
    spin_unlock(&ev->spin);
    for (uint32_t i = 0; i < cnt; i++) {
        _olp_try_revive_slot(ev, snap[i]);
        _iocp_try_freelsn(snap[i]);
    }
}
// AcceptEx完成回调：重新提交AcceptEx、设置socket选项、将新fd发送给对应watcher。
static void _olp_on_accept_cb(acceptex_ctx *acpctx, evsock_ctx *evsk, DWORD bytes) {
    overlap_acpt_ctx *olacp = UPCAST(evsk, overlap_acpt_ctx, overlap);
    listener_ctx *lsn = olacp->lsn;
    SOCKET fd = _olp_take_fd(&olacp->overlap.sk.fd);
    if (INVALID_SOCK == fd || 0 != ATOMIC_GET(&lsn->remove)) {
        if (INVALID_SOCK != fd) {
            SOCK_CLOSE(fd);
        }
        _iocp_try_freelsn(lsn);
        return;
    }
    int32_t acpfail = (ERROR_SUCCESS != olacp->overlap.overlapped.Internal);
    if (acpfail) {
        CLOSE_SOCK(fd);
    }
    // _olp_post_accept 写入新 fd 后 ev_unlisten 可立即关闭并触发 error completion 减 slot ref；
    // 须在此之前 +1 占位，否则 lsn 可能提前被释放
    ATOMIC_ADD(&lsn->ref, 1);
    if (ERR_OK != _olp_post_accept(olacp)) {
        SOCKET log_fd = lsn->fd;
        CLOSE_SOCK(fd);
        ATOMIC_SET(&olacp->dead, 1);
        _olp_ndead_add(acpctx->ev, lsn, 1);
        int32_t old = ATOMIC_ADD(&lsn->ref, -2);
        if (2 == old) {
            _iocp_freelsn(lsn);
        } else if (3 == old) {
            LOG_ERROR("AcceptEx slot failed to re-post, listener fd %d.", (int32_t)log_fd);
        }
        return;
    }
    // _olp_post_accept 执行期间 ev_unlisten 可能运行但未能关闭新 fd，返回后需补关
    if (0 != ATOMIC_GET(&lsn->remove)) {
        _olp_take_close(&olacp->overlap.sk.fd);
    }
    if (acpfail) {
        _iocp_try_freelsn(lsn);
        return;
    }
    if (ERR_OK != setsockopt(fd,
                             SOL_SOCKET,
                             SO_UPDATE_ACCEPT_CONTEXT,
                             (char *)&lsn->fd,
                             (int32_t)sizeof(lsn->fd))
        || ERR_OK != sock_nodelay(fd)
        || ERR_OK != _evpub_tcp_keepalive(fd)) {
        CLOSE_SOCK(fd);
        _iocp_try_freelsn(lsn);
        return;
    }
    // iocp lsn 是独立线程，不会有同线程免投递
    _cmd_add_acpfd(CALC_WATCHER(acpctx->ev->watcher, acpctx->ev->nthreads, fd), fd, lsn);
}
static void _iocp_add_acpfd_inloop_err(watcher_ctx *watcher, overlap_tcp_ctx *oltcp) {
    _evpub_sockel_remove(watcher, oltcp->ol_r.sk.fd);
    pool_push(&watcher->pool, &oltcp->ol_r, 0);
}
void _iocp_add_acpfd_inloop(watcher_ctx *watcher, SOCKET fd, listener_ctx *lsn) {
    if (ERR_OK != _iocp_join(watcher, fd)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        CLOSE_SOCK(fd);
        return;
    }
    skpool_args skargs = { .sk = { .fd = fd, .index = watcher->index }, .cbs = &lsn->cbs, .ud = &lsn->ud };
    evsock_ctx *evsk = pool_pop(&watcher->pool, &skargs, 0);
    overlap_tcp_ctx *oltcp = UPCAST(evsk, overlap_tcp_ctx, ol_r);
    BIT_SET(oltcp->status, STATUS_ESTABLISHED);// accept 出来的连接已连通
    _evpub_sockel_add(watcher, evsk);
#if WITH_SSL
    if (NULL != lsn->evssl) {// 默认启用ssl
        // 设置ssl
        oltcp->ssl = evssl_setfd(lsn->evssl, oltcp->ol_r.sk.fd);
        if (NULL == oltcp->ssl) {
            _iocp_add_acpfd_inloop_err(watcher, oltcp);
            return;
        }
        BIT_SET(oltcp->status, STATUS_AUTHSSL);// 等待握手
    }
#endif
    if (ERR_OK != _olp_post_recv(oltcp)) {
        _iocp_add_acpfd_inloop_err(watcher, oltcp);
        return;
    }
    if (ERR_OK != _olp_call_acp_cb(watcher->ev, oltcp)) {
        _iocp_disconnect(&oltcp->ol_r);
        return;
    }
}
// 关闭前cnt个AcceptEx挂起的socket（取消未完成的AcceptEx操作）
static void _olp_free_acceptex(listener_ctx *lsn, int32_t cnt) {
    for (int32_t i = 0; i < cnt; i++) {
        _olp_take_close(&lsn->overlap_acpt[i].overlap.sk.fd);
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
        olacp->overlap.sk.fd = INVALID_SOCK;
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
    netaddr_ctx addr;
    if (ERR_OK != _evpub_sock_launch_check(ctx, ip, port, cbs, ud, 0, &addr)) {
        return ERR_FAILED;
    }
    SOCKET fd = _evpub_listen(&addr);
    if (INVALID_SOCK == fd) {
        LOG_ERROR("listen %s:%d error.", ip, port);
        UD_FREE(cbs->ud_free, ud);
        return ERR_FAILED;
    }
    listener_ctx *lsn;
    // 显式初始化所有 overlap_acpt[].overlap.sk.fd = INVALID_SOCK，CreateIoCompletionPort 失败走 _iocp_freelsn 时
    // _olp_free_acceptex 中 _olp_take_close 仅跳过 INVALID_SOCK，避免脏值（含 fd=0）被误关
    CALLOC(lsn, 1, sizeof(listener_ctx));
    for (int32_t i = 0; i < MAX_ACCEPTEX_CNT; i++) {
        lsn->overlap_acpt[i].overlap.sk.fd = INVALID_SOCK;
    }
    lsn->ev = ctx;
    // 存活计数+1，须在任何走_iocp_freelsn的失败路径之前；_iocp_freelsn统一-1
    ATOMIC_ADD(&ctx->nlsn, 1);
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
    // 释放前扣除本 listener 尚未复活的 dead 槽，保持 ndead_total==sum(存活 lsn->ndead)
    int32_t ndead = (int32_t)ATOMIC_GET(&lsn->ndead);
    if (0 != ndead) {
        ATOMIC_ADD(&lsn->ev->ndead_total, -ndead);
    }
    ATOMIC_ADD(&lsn->ev->nlsn, -1);
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
    CancelIoEx((HANDLE)lsn->fd, NULL);
    _olp_free_acceptex(lsn, MAX_ACCEPTEX_CNT);
    // 减占位 ref；cb 都已完成时此处减到 0 释放，否则由最后一个 cb 释放
    _iocp_try_freelsn(lsn);
}
// ev_free关闭阶段：ev_unlisten掉所有仍在arrlsn中的listener（remove=1+取消在途AcceptEx）。
// 释放交由取消完成驱动（cb减ref至0走_iocp_freelsn），取代旧的强制_iocp_freelsn——
// 后者在内核异步写取消完成的内嵌OVERLAPPED之前FREE(lsn)，构成write-after-free。
void _iocp_unlisten_all(ev_ctx *ctx) {
    uint64_t id;
    for (;;) {
        spin_lock(&ctx->spin);
        if (0 == array_size(&ctx->arrlsn)) {
            spin_unlock(&ctx->spin);
            break;
        }
        id = (*(listener_ctx **)array_at(&ctx->arrlsn, 0))->id;
        spin_unlock(&ctx->spin);
        ev_unlisten(ctx, id);
    }
}
// ev_free排空阶段直接释放AcceptEx完成对应的listener引用：此时所有slot已remove=1+取fd，
// _olp_on_accept_cb必走释放分支且仅调_iocp_try_freelsn，故直接减ref跳过其余死分支
void _iocp_acpex_release(evsock_ctx *evsk) {
    overlap_acpt_ctx *olacp = UPCAST(evsk, overlap_acpt_ctx, overlap);
    _iocp_try_freelsn(olacp->lsn);
}
// 提交WSARecvFrom异步UDP接收请求
static inline int32_t _olp_post_recv_from(overlap_udp_ctx *oludp) {
    ZERO(&oludp->ol_r.overlapped, sizeof(oludp->ol_r.overlapped));
    oludp->flag = oludp->bytes_r = 0;
    oludp->addrlen = netaddr_size(&oludp->addr_r);
    if (ERR_OK != WSARecvFrom(oludp->ol_r.sk.fd,
                              &oludp->wsabuf_r,
                              1,
                              &oludp->bytes_r,
                              &oludp->flag,
                              netaddr_addr(&oludp->addr_r),
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
static inline void _olp_on_udp_close_r(watcher_ctx *watcher, overlap_udp_ctx *oludp) {
    _olp_call_udp_close_cb(watcher->ev, oludp);
    if (BIT_CHECK(oludp->status, STATUS_SENDING)) {
        BIT_SET(oludp->status, STATUS_REMOVE);
    } else {
        _evpub_sockel_remove(watcher, oludp->ol_r.sk.fd);
        _iocp_free_udp(&oludp->ol_r);
    }
}
// WSARecvFrom完成回调：触发recvfrom回调并重新提交接收
static void _olp_on_recvfrom_cb(watcher_ctx *watcher, evsock_ctx *evsk, DWORD bytes) {
    overlap_udp_ctx *oludp = UPCAST(evsk, overlap_udp_ctx, ol_r);
    if (BIT_CHECK(oludp->status, STATUS_ERROR)) {
        _olp_on_udp_close_r(watcher, oludp);
        return;
    }
    if (ERROR_SUCCESS != oludp->ol_r.overlapped.Internal) {
        // 超大 datagram WSAEMSGSIZE 截断等软错误：告警丢弃，不关 socket，重新提交接收（与 POSIX UDP 对齐）
        LOG_WARN("UDP recvfrom error on fd %d, dropped (socket kept).", (int32_t)oludp->ol_r.sk.fd);
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
// 提交WSASendTo异步UDP发送请求；addr 拷到 oludp->addr_s 持久化(队列元素异步期间可能被复用)。
// 四种返回,1 与 2 不能混——混了就是拿资源紧张当丢数据的理由,分类同 unix 的 _usk_udp_sendmsg_once:
//   ERR_OK      已投递,完成包稍后到,调用方停手等它
//   1           这一条彻底毁了、fd 还能用,丢掉接着发队列里的下一条
//   2           这一条只是暂时发不出去(资源紧张之类),原样留在队头等下次驱动
//   ERR_FAILED  fd 本身已废,须关连接
static inline int32_t _olp_post_sendto(overlap_udp_ctx *oludp, sendto_ctx *buf) {
    ZERO(&oludp->ol_s.overlapped, sizeof(oludp->ol_s.overlapped));
    oludp->bytes_s = 0;
    oludp->addr_s = buf->addr;
    oludp->wsabuf_s.IOV_PTR_FIELD = (char *)buf->data;
    oludp->wsabuf_s.IOV_LEN_FIELD = (IOV_LEN_TYPE)buf->len;
    if (ERR_OK != WSASendTo(oludp->ol_s.sk.fd,
                            &oludp->wsabuf_s,
                            1,
                            &oludp->bytes_s,
                            0,
                            netaddr_addr(&oludp->addr_s),
                            netaddr_size(&oludp->addr_s),
                            &oludp->ol_s.overlapped,
                            NULL)) {
        int32_t erro = (int32_t)ERRNO;
        if (ERROR_IO_PENDING != erro) {
            if (WSAEBADF == erro
                || WSAENOTSOCK == erro) {
                return ERR_FAILED;
            }
            // 资源紧张(非分页池吃紧、未决重叠 I/O 过多)是瞬时的,数据还好好的。
            // 与 unix 侧一致不打日志:重试路径会反复经过这里,记一条就是刷屏
            if (WSAEWOULDBLOCK == erro
                || WSAENOBUFS == erro) {
                return 2;
            }
            LOG_WARN("UDP sendto dropped one datagram on fd %d: %s.",
                     (int32_t)oludp->ol_s.sk.fd, ERRORSTR(erro));
            return 1;
        }
    }
    return ERR_OK;
}
// 摘除重试节点;幂等,未挂时直接返回。释放 oludp 前必须调用,否则 watcher->ticks 里留悬空节点
static inline void _olp_sendto_retry_stop(overlap_udp_ctx *oludp) {
    if (NULL == oludp->send_tick.cb) {
        return;
    }
    oludp->send_tick.cb = NULL;
    _evpub_tick_remove(oludp->watcher, &oludp->send_tick);
}
// 重试 tick:再排一次队。drain 内部按结果自行摘除或续挂,这里只负责退避与致命错误善后
static uint32_t _olp_on_sendto_retry(void *ud, uint64_t now_ms) {
    overlap_udp_ctx *oludp = ud;
    // 已进关闭流程:不再投递,摘掉自己等在途完成包把 oludp 释放掉
    if (BIT_CHECK(oludp->status, STATUS_ERROR)) {
        _olp_sendto_retry_stop(oludp);
        return EVENT_WAIT_TIMEOUT;
    }
    if (now_ms < oludp->retry_until) {
        return (uint32_t)(oludp->retry_until - now_ms);
    }
    BIT_SET(oludp->status, STATUS_SENDING);
    if (ERR_FAILED == _olp_sendto_drain(oludp->watcher, oludp)) {
        BIT_REMOVE(oludp->status, STATUS_SENDING);
        _iocp_disconnect(&oludp->ol_r);
        return EVENT_WAIT_TIMEOUT;
    }
    // drain 已摘除:投出去了或队列空了,不必再来
    if (NULL == oludp->send_tick.cb) {
        return EVENT_WAIT_TIMEOUT;
    }
    if (oludp->retry_ms < EVENT_WAIT_TIMEOUT) {
        oludp->retry_ms *= 2;
        if (oludp->retry_ms > EVENT_WAIT_TIMEOUT) {
            oludp->retry_ms = EVENT_WAIT_TIMEOUT;
        }
    }
    oludp->retry_until = now_ms + oludp->retry_ms;
    return oludp->retry_ms;
}
// 队头因资源紧张发不出去时挂上周期重试。IOCP 没有"可写"事件可等,不自驱就只能等下一次
// ev_sendto —— 队头堵住等于该 fd 整条发送路径停摆(后来的包全排在它后面),业务若就此不再
// 发送就一直停到关连接。做法与 usock.c 的 accept EMFILE 退避同构:cb 非 NULL 表示已挂。
// 从 EVENT_TICK_MIN 起步、翻倍到 EVENT_WAIT_TIMEOUT 封顶,免得资源持续紧张时 10ms 空转
static inline void _olp_sendto_retry(watcher_ctx *watcher, overlap_udp_ctx *oludp) {
    if (NULL != oludp->send_tick.cb) {
        return;
    }
    oludp->watcher = watcher;
    oludp->retry_ms = EVENT_TICK_MIN;
    oludp->retry_until = timer_cur_ms(&watcher->timer) + EVENT_TICK_MIN;
    oludp->send_tick.cb = _olp_on_sendto_retry;
    oludp->send_tick.ud = oludp;
    _evpub_tick_add(watcher, &oludp->send_tick);
}
// 同步发:成过就不必投 IRP,省掉一个完成包与一次回调派发。
static inline int32_t _olp_sendto_sync(overlap_udp_ctx *oludp, sendto_ctx *buf) {
    IOV_TYPE wsabuf;
    DWORD bytes = 0;
    wsabuf.IOV_PTR_FIELD = (char *)buf->data;
    wsabuf.IOV_LEN_FIELD = (IOV_LEN_TYPE)buf->len;
    if (SOCKET_ERROR != WSASendTo(oludp->ol_s.sk.fd,
                                  &wsabuf,
                                  1,
                                  &bytes,
                                  0,
                                  netaddr_addr(&buf->addr),
                                  netaddr_size(&buf->addr),
                                  NULL,
                                  NULL)) {
        return ERR_OK;
    }
    return ERR_FAILED;
}
// 从发送队列取下一条投递:毁掉的就地丢掉再取下一条,直到有一条投递出去(此时 data 归完成
// 回调释放,STATUS_SENDING 保持置位)、队列发空(清掉 STATUS_SENDING),或撞上 fd 级错误。
// 软错误不产生完成包,所以后续的包必须在这里接着发,不能坐等完成回调驱动。
// 暂时发不出去(_olp_post_sendto 返回 2)的那条原样留在队头,清掉 STATUS_SENDING 并挂上重试 tick
// 自驱;因此改用 peek:投递成功或确认丢弃才出队,期间不会有别的线程 push(IOCP 完成包由本 watcher 取)。
// 返回 ERR_FAILED 表示 fd 已废,由调用方关连接
static int32_t _olp_sendto_drain(watcher_ctx *watcher, overlap_udp_ctx *oludp) {
    int32_t rtn;
    void *data;
    sendto_ctx *sendbuf;
    while (NULL != (sendbuf = queue_peek(&oludp->buf_s))) {
        // 同步发成功就地回收接着发下一条,整队一次抽干;原来每条都要等一个完成包再回来
        if (ERR_OK == _olp_sendto_sync(oludp, sendbuf)) {
            oludp->wb_size -= sendbuf->len;
            data = sendbuf->data;
            queue_pop(&oludp->buf_s);
            FREE(data);
            continue;
        }
        rtn = _olp_post_sendto(oludp, sendbuf);
        if (2 == rtn) {
            _olp_sendto_retry(watcher, oludp);
            BIT_REMOVE(oludp->status, STATUS_SENDING);
            return ERR_OK;
        }
        oludp->wb_size -= sendbuf->len;
        data = sendbuf->data;
        queue_pop(&oludp->buf_s);
        if (ERR_OK == rtn) {
            _olp_sendto_retry_stop(oludp);
            return ERR_OK;
        }
        FREE(data);
        if (ERR_FAILED == rtn) {
            _olp_sendto_retry_stop(oludp);
            return ERR_FAILED;
        }
    }
    _olp_sendto_retry_stop(oludp);
    BIT_REMOVE(oludp->status, STATUS_SENDING);
    return ERR_OK;
}
// WSASendTo完成回调：释放当前缓冲区，继续发送队列中下一条或清除发送标志
static void _olp_on_sendto_cb(watcher_ctx *watcher, evsock_ctx *evsk, DWORD bytes) {
    overlap_udp_ctx *oludp = UPCAST(evsk, overlap_udp_ctx, ol_s);
    void *data = oludp->wsabuf_s.IOV_PTR_FIELD;
    FREE(data);
    if (BIT_CHECK(oludp->status, STATUS_ERROR)) {
        if (BIT_CHECK(oludp->status, STATUS_REMOVE)) {
            _evpub_sockel_remove(watcher, oludp->ol_r.sk.fd);
            _iocp_free_udp(&oludp->ol_r);
        } else {
            BIT_REMOVE(oludp->status, STATUS_SENDING);
        }
        return;
    }
    if (0 != oludp->ol_s.overlapped.Internal) {
        DWORD transferred, flags;
        WSAGetOverlappedResult(oludp->ol_s.sk.fd, &oludp->ol_s.overlapped, &transferred, FALSE, &flags);
        int32_t err = (int32_t)WSAGetLastError();
        if (WSAEBADF == err || WSAENOTSOCK == err) {
            BIT_REMOVE(oludp->status, STATUS_SENDING);
            _iocp_disconnect(&oludp->ol_r);
            return;
        }
        LOG_WARN("UDP sendto dropped one datagram on fd %d: %s.", (int32_t)oludp->ol_s.sk.fd, ERRORSTR(err));
    }
    if (ERR_OK != _olp_sendto_drain(watcher, oludp)) {
        BIT_REMOVE(oludp->status, STATUS_SENDING);
        _iocp_disconnect(&oludp->ol_r);
    }
}
int32_t _iocp_try_sendto(evsock_ctx *evsk, const void *data, size_t len, netaddr_ctx *addr) {
    overlap_udp_ctx *oludp = UPCAST(evsk, overlap_udp_ctx, ol_r);
    if (BIT_CHECK(oludp->status, STATUS_ERROR)) {
        return 0;
    }
    // 三种在途态都得让路,否则这条会插到已排队的前面去。STATUS_SENDING 单独列是因为
    // _olp_sendto_drain 投递成功后就 pop 了,存在队列已空而 IRP 仍在途的窗口
    if (0 != queue_size(&oludp->buf_s)
        || NULL != oludp->send_tick.cb
        || BIT_CHECK(oludp->status, STATUS_SENDING)) {
        return 1;
    }
    IOV_TYPE wsabuf;
    DWORD bytes = 0;
    wsabuf.IOV_PTR_FIELD = (char *)data;
    wsabuf.IOV_LEN_FIELD = (IOV_LEN_TYPE)len;
    if (SOCKET_ERROR != WSASendTo(oludp->ol_s.sk.fd,
                                  &wsabuf,
                                  1,
                                  &bytes,
                                  0,
                                  netaddr_addr(addr),
                                  netaddr_size(addr),
                                  NULL,
                                  NULL)) {
        return 0;
    }
    // 失败一律回落排队走原三档,不在这里分类也不关连接:调用方可能是 tick 迭代里的 kcp 输出
    return 1;
}
void _iocp_add_bufs_trysendto(watcher_ctx *watcher, evsock_ctx *evsk, sendto_ctx *buf) {
    overlap_udp_ctx *oludp = UPCAST(evsk, overlap_udp_ctx, ol_r);
    // 已在 error 关闭流程：拒收新数据
    if (BIT_CHECK(oludp->status, STATUS_ERROR)
        || !_evpub_sendqu_check_udp(&oludp->buf_s, oludp->ol_s.sk.fd)) {
        FREE(buf->data);
        return;
    }
    oludp->wb_size += buf->len;
    _evpub_sendqu_tda(&oludp->tda, oludp->wb_size, oludp->ol_s.sk.fd, 0);
    queue_push(&oludp->buf_s, buf);
    if (NULL != oludp->send_tick.cb
        || BIT_CHECK(oludp->status, STATUS_SENDING)) {
        return;
    }
    BIT_SET(oludp->status, STATUS_SENDING);
    if (ERR_OK != _olp_sendto_drain(watcher, oludp)) {
        BIT_REMOVE(oludp->status, STATUS_SENDING);
        _iocp_disconnect(&oludp->ol_r);
    }
}
// 分配并初始化UDP上下文（不使用对象池，因UDP不常关闭/新建）
static evsock_ctx *_olp_new_udp(skpool_args *skargs) {
    overlap_udp_ctx *oludp;
    MALLOC(oludp, sizeof(overlap_udp_ctx));
    oludp->ol_r.type = SOCK_DGRAM;
    oludp->ol_r.sk = skargs->sk;// 口径同 _evpub_sk_new
    oludp->ol_r.sk.skid = createid();
    oludp->ol_r.ev_cb = _olp_on_recvfrom_cb;
    oludp->ol_s.type = SOCK_DGRAM;
    oludp->ol_s.sk = oludp->ol_r.sk;// 同 _evpub_sk_new
    oludp->ol_s.ev_cb = _olp_on_sendto_cb;
    oludp->status = STATUS_NONE;
    oludp->cbs = *skargs->cbs;
    COPY_UD(oludp->ud, skargs->ud);
    netaddr_empty(&oludp->addr_r);
    oludp->addrlen = netaddr_size(&oludp->addr_r);
    oludp->wsabuf_r.buf = oludp->buf;
    oludp->wsabuf_r.len = sizeof(oludp->buf);
    queue_init(&oludp->buf_s, sizeof(sendto_ctx), INIT_SENDBUF_LEN);
    oludp->wb_size = 0;
    oludp->watcher = NULL;
    oludp->retry_ms = 0;
    oludp->retry_until = 0;
    oludp->send_tick.cb = NULL;
    tda_init(&oludp->tda, WB_WARN_INIT_SIZE);
    return &oludp->ol_r;
}
void _iocp_free_udp(evsock_ctx *evsk) {
    overlap_udp_ctx *oludp = UPCAST(evsk, overlap_udp_ctx, ol_r);
    _olp_sendto_retry_stop(oludp);// 先摘重试节点,否则 watcher->ticks 里留一个指向已释放内存的节点
    CLOSE_SOCK(oludp->ol_r.sk.fd);
    _evpub_sendto_clear(&oludp->buf_s);
    queue_free(&oludp->buf_s);
    UD_FREE(oludp->cbs.ud_free, &oludp->ud);
    FREE(oludp);
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
    evsock_ctx *evsk = _olp_new_udp(&skargs);
    *sk = evsk->sk;
    _cmd_add(&ctx->watcher[sk->index], evsk);
    return ERR_OK;
}
void _iocp_add_fd_inloop(watcher_ctx *watcher, evsock_ctx *evsk) {
    if (ERR_OK != _iocp_join(watcher, evsk->sk.fd)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        if (SOCK_STREAM == evsk->type) {
            pool_push(&watcher->pool, evsk, 0);
        } else {
            _olp_call_udp_close_cb(watcher->ev, UPCAST(evsk, overlap_udp_ctx, ol_r));
            _iocp_free_udp(evsk);
        }
        return;
    }
    _evpub_sockel_add(watcher, evsk);
    if (SOCK_STREAM == evsk->type) {
        overlap_tcp_ctx *tcp = UPCAST(evsk, overlap_tcp_ctx, ol_r);
        if (ERR_OK != _olp_post_recv(tcp)) {
            _evpub_sockel_remove(watcher, evsk->sk.fd);
            pool_push(&watcher->pool, evsk, 0);
        }
    } else {
        overlap_udp_ctx *udp = UPCAST(evsk, overlap_udp_ctx, ol_r);
        if (ERR_OK != _olp_post_recv_from(udp)) {
            _evpub_sockel_remove(watcher, evsk->sk.fd);
            _olp_call_udp_close_cb(watcher->ev, udp);
            _iocp_free_udp(evsk);
        }
    }
}

#endif
