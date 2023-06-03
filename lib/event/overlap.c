#include "event/event.h"
#include "event/iocp.h"
#include "event/skpool.h"
#include "buffer.h"
#include "loger.h"
#include "hashmap.h"
#include "netutils.h"

#ifdef EV_IOCP

#define MAX_ACCEPTEX_CNT    128
#define STATUS_SENDING      0x01
#define STATUS_ERROR        0x02
#define STATUS_REMOVE       0x04
#define STATUS_SERVER       0x08
#define STATUS_HANDSHAAKE   0x10

typedef struct overlap_acpt_ctx {
    sock_ctx overlap;
    struct listener_ctx *lsn;
    DWORD bytes;
    char addr[sizeof(struct sockaddr_storage)];
}overlap_acpt_ctx;
typedef struct listener_ctx {
    int32_t family;
    SOCKET fd;
#if WITH_SSL
    evssl_ctx *evssl;
#endif
    cbs_ctx cbs;
    ud_cxt ud;
    overlap_acpt_ctx overlap_acpt[MAX_ACCEPTEX_CNT];
}listener_ctx;
typedef struct overlap_tcp_ctx {
    sock_ctx ol_r;
    sock_ctx ol_s;
    int32_t status;
    DWORD bytes_r;
    DWORD bytes_s;
    DWORD flag;
#if WITH_SSL
    SSL *ssl;
#endif
    IOV_TYPE wsabuf;
    buffer_ctx buf_r;
    qu_bufs buf_s;
    cbs_ctx cbs;
    ud_cxt ud;
}overlap_tcp_ctx;
typedef struct overlap_udp_ctx {
    sock_ctx ol_r;
    sock_ctx ol_s;
    int32_t addrlen;
    int32_t status;
    DWORD bytes_r;
    DWORD bytes_s;
    DWORD flag;
    cbs_ctx cbs;
    IOV_TYPE wsabuf_s;
    IOV_TYPE wsabuf_r;
    qu_bufs buf_s;
    netaddr_ctx addr;
    ud_cxt ud;
    char buf[MAX_RECVFROM_SIZE];
}overlap_udp_ctx;

static void _on_recv_cb(watcher_ctx *watcher, sock_ctx *skctx, DWORD bytes);
static void _on_send_cb(watcher_ctx *watcher, sock_ctx *skctx, DWORD bytes);

void _sk_shutdown(sock_ctx *skctx) {
#if WITH_SSL
    overlap_tcp_ctx *oltcp = UPCAST(skctx, overlap_tcp_ctx, ol_r);
    evssl_shutdown(oltcp->ssl, oltcp->ol_r.fd);
#else
    shutdown(skctx->fd, SHUT_RD);
#endif
}
sock_ctx *_new_sk(SOCKET fd, cbs_ctx *cbs, ud_cxt *ud) {
    overlap_tcp_ctx *oltcp;
    MALLOC(oltcp, sizeof(overlap_tcp_ctx));
    oltcp->ol_r.type = SOCK_STREAM;
    oltcp->ol_r.fd = fd;
    oltcp->ol_r.ev_cb = _on_recv_cb;
    oltcp->ol_s.type = SOCK_STREAM;
    oltcp->ol_s.fd = fd;
    oltcp->ol_s.ev_cb = _on_send_cb;
    oltcp->status = 0;
#if WITH_SSL
    oltcp->ssl = NULL;
#endif
    oltcp->wsabuf.IOV_PTR_FIELD = NULL;
    oltcp->wsabuf.IOV_LEN_FIELD = 0;
    oltcp->cbs = *cbs;
    COPY_UD(oltcp->ud, ud);
    buffer_init(&oltcp->buf_r);
    qu_bufs_init(&oltcp->buf_s, INIT_SENDBUF_LEN);
    return &oltcp->ol_r;
}
void _free_sk(sock_ctx *skctx) {
    overlap_tcp_ctx *oltcp = UPCAST(skctx, overlap_tcp_ctx, ol_r);
#if WITH_SSL
    FREE_SSL(oltcp->ssl);
#endif
    CLOSE_SOCK(oltcp->ol_r.fd);
    buffer_free(&oltcp->buf_r);
    _bufs_clear(&oltcp->buf_s);
    qu_bufs_free(&oltcp->buf_s);
    if (NULL != oltcp->cbs.ud_free) {
        oltcp->cbs.ud_free(&oltcp->ud);
    }
    FREE(oltcp);
}
void _clear_sk(sock_ctx *skctx) {
    overlap_tcp_ctx *oltcp = UPCAST(skctx, overlap_tcp_ctx, ol_r);
    oltcp->status = 0;
#if WITH_SSL
    FREE_SSL(oltcp->ssl);
#endif
    CLOSE_SOCK(oltcp->ol_r.fd);
    oltcp->ol_s.fd = INVALID_SOCK;
    _bufs_clear(&oltcp->buf_s);
    buffer_drain(&oltcp->buf_r, buffer_size(&oltcp->buf_r));
    if (NULL != oltcp->cbs.ud_free) {
        oltcp->cbs.ud_free(&oltcp->ud);
    }
}
void _reset_sk(sock_ctx *skctx, SOCKET fd, cbs_ctx *cbs, ud_cxt *ud) {
    overlap_tcp_ctx *oltcp = UPCAST(skctx, overlap_tcp_ctx, ol_r);
    oltcp->ol_r.fd = fd;
    oltcp->ol_s.fd = fd;
    oltcp->cbs = *cbs;
    COPY_UD(oltcp->ud, ud);
}
void _set_error(sock_ctx *skctx) {
    if (SOCK_STREAM == skctx->type) {
        UPCAST(skctx, overlap_tcp_ctx, ol_r)->status |= STATUS_ERROR;
        return;
    }
    UPCAST(skctx, overlap_udp_ctx, ol_r)->status |= STATUS_ERROR;
}
void _add_fd(watcher_ctx *watcher, sock_ctx *skctx) {
    ASSERTAB(NULL == hashmap_set(watcher->element, &skctx), "socket repeat.");
}
void _remove_fd(watcher_ctx *watcher, SOCKET fd) {
    sock_ctx key;
    key.fd = fd;
    sock_ctx *pkey = &key;
    hashmap_delete(watcher->element, &pkey);
}
//recv
int32_t _post_recv(sock_ctx *skctx, DWORD  *bytes, DWORD  *flag, IOV_TYPE *wsabuf, DWORD niov) {
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
#if WITH_SSL
static inline int32_t _ssl_handshake_acpt(watcher_ctx *watcher, overlap_tcp_ctx *oltcp) {
    int32_t rtn = ERR_FAILED;
    switch (evssl_tryacpt(oltcp->ssl)) {
    case ERR_FAILED://错误
        _remove_fd(watcher, oltcp->ol_r.fd);
        pool_push(&watcher->pool, &oltcp->ol_r);
        break;
    case 1://完成
        if (ERR_OK != oltcp->cbs.acp_cb(watcher->ev, oltcp->ol_r.fd, &oltcp->ud)) {
            _remove_fd(watcher, oltcp->ol_r.fd);
            pool_push(&watcher->pool, &oltcp->ol_r);
            break;
        }
        oltcp->status |= STATUS_HANDSHAAKE;
        rtn = ERR_OK;
        break;
    case ERR_OK://等待更多数据
        if (ERR_OK != _post_recv(&oltcp->ol_r, &oltcp->bytes_r, &oltcp->flag, &oltcp->wsabuf, 1)) {
            _remove_fd(watcher, oltcp->ol_r.fd);
            pool_push(&watcher->pool, &oltcp->ol_r);
        }
        break;
    }
    return rtn;
}
static inline int32_t _ssl_handshake_conn(watcher_ctx *watcher, overlap_tcp_ctx *oltcp) {
    int32_t rtn = ERR_FAILED;
    switch (evssl_tryconn(oltcp->ssl)) {
    case ERR_FAILED://错误
        oltcp->cbs.conn_cb(watcher->ev, oltcp->ol_r.fd, ERR_FAILED, &oltcp->ud);
        _remove_fd(watcher, oltcp->ol_r.fd);
        pool_push(&watcher->pool, &oltcp->ol_r);
        break;
    case 1://完成
        if (ERR_OK != oltcp->cbs.conn_cb(watcher->ev, oltcp->ol_r.fd, ERR_OK, &oltcp->ud)) {
            _remove_fd(watcher, oltcp->ol_r.fd);
            pool_push(&watcher->pool, &oltcp->ol_r);
            break;
        }
        oltcp->status |= STATUS_HANDSHAAKE;
        rtn = ERR_OK;
        break;
    case ERR_OK://等待更多数据
        if (ERR_OK != _post_recv(&oltcp->ol_r, &oltcp->bytes_r, &oltcp->flag, &oltcp->wsabuf, 1)) {
            oltcp->cbs.conn_cb(watcher->ev, oltcp->ol_r.fd, ERR_FAILED, &oltcp->ud);
            _remove_fd(watcher, oltcp->ol_r.fd);
            pool_push(&watcher->pool, &oltcp->ol_r);
        }
        break;
    }
    return rtn;
}
static inline int32_t _ssl_handshake(watcher_ctx *watcher, overlap_tcp_ctx *oltcp) {
    if (oltcp->status & STATUS_SERVER) {
        return _ssl_handshake_acpt(watcher, oltcp);
    }
    return _ssl_handshake_conn(watcher, oltcp);
}
#endif
static inline void _tcp_recv(watcher_ctx *watcher, overlap_tcp_ctx *oltcp) {
    size_t nread;
#if WITH_SSL
    int32_t rtn = buffer_from_sock(&oltcp->buf_r, oltcp->ol_r.fd, &nread, _sock_read, oltcp->ssl);
#else
    int32_t rtn = buffer_from_sock(&oltcp->buf_r, oltcp->ol_r.fd, &nread, _sock_read, NULL);
#endif
    if (0 != nread) {
        oltcp->cbs.r_cb(watcher->ev, oltcp->ol_r.fd, &oltcp->buf_r, nread, &oltcp->ud);
    }
    if (ERR_OK == rtn) {
        rtn = _post_recv(&oltcp->ol_r, &oltcp->bytes_r, &oltcp->flag, &oltcp->wsabuf, 1);
    }
    if (ERR_OK != rtn) {
        oltcp->status |= STATUS_ERROR;
        if (NULL != oltcp->cbs.c_cb) {
            oltcp->cbs.c_cb(watcher->ev, oltcp->ol_r.fd, &oltcp->ud);
        }
        if (oltcp->status & STATUS_SENDING) {
            oltcp->status |= STATUS_REMOVE;
        } else {
            _remove_fd(watcher, oltcp->ol_r.fd);
            pool_push(&watcher->pool, &oltcp->ol_r);
        }
    }
}
static void _on_recv_cb(watcher_ctx *watcher, sock_ctx *skctx, DWORD bytes) {
    overlap_tcp_ctx *oltcp = UPCAST(skctx, overlap_tcp_ctx, ol_r);
    if (oltcp->status & STATUS_ERROR) {
        int32_t handshake = 1;
#if WITH_SSL
        if (NULL != oltcp->ssl
            && !(oltcp->status & STATUS_HANDSHAAKE)) {
            handshake = 0;
        }
#endif
        if (handshake) {
            if (NULL != oltcp->cbs.c_cb) {
                oltcp->cbs.c_cb(watcher->ev, oltcp->ol_r.fd, &oltcp->ud);
            }
        } else {
            if (!(oltcp->status & STATUS_SERVER)) {
                oltcp->cbs.conn_cb(watcher->ev, oltcp->ol_r.fd, ERR_FAILED, &oltcp->ud);
            }
        }
        if (oltcp->status & STATUS_SENDING) {
            oltcp->status |= STATUS_REMOVE;
        } else {
            _remove_fd(watcher, oltcp->ol_r.fd);
            pool_push(&watcher->pool, &oltcp->ol_r);
        }
        return;
    }
#if WITH_SSL
    if (NULL != oltcp->ssl
        && !(oltcp->status & STATUS_HANDSHAAKE)) {
        if (ERR_OK != _ssl_handshake(watcher, oltcp)) {
            return;
        }
    }
#endif
    _tcp_recv(watcher, oltcp);
}
//send
static inline int32_t _post_send(overlap_tcp_ctx *oltcp) {
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
static void _on_send_cb(watcher_ctx *watcher, sock_ctx *skctx, DWORD bytes) {
    overlap_tcp_ctx *oltcp = UPCAST(skctx, overlap_tcp_ctx, ol_s);
    if (oltcp->status & STATUS_ERROR) {
        if (oltcp->status & STATUS_REMOVE) {
            _remove_fd(watcher, oltcp->ol_r.fd);
            pool_push(&watcher->pool, &oltcp->ol_r);
        } else {
            oltcp->status = oltcp->status & ~STATUS_SENDING;
        }
        return;
    }
    size_t nsend;
#if WITH_SSL
    int32_t rtn = _sock_send(oltcp->ol_s.fd, &oltcp->buf_s, &nsend, oltcp->ssl);
#else
    int32_t rtn = _sock_send(oltcp->ol_s.fd, &oltcp->buf_s, &nsend, NULL);
#endif
    if (NULL != oltcp->cbs.s_cb
        && 0 != nsend) {
        oltcp->cbs.s_cb(watcher->ev, oltcp->ol_s.fd, nsend, &oltcp->ud);
    }
    if (ERR_OK != rtn) {
        oltcp->status |= STATUS_ERROR;
        oltcp->status = oltcp->status & ~STATUS_SENDING;
        return;
    }
    if (0 == qu_bufs_size(&oltcp->buf_s)) {
        oltcp->status = oltcp->status & ~STATUS_SENDING;
        return;
    }
    if (ERR_OK != _post_send(oltcp)) {
        oltcp->status |= STATUS_ERROR;
        oltcp->status = oltcp->status & ~STATUS_SENDING;
    }
}
void _add_bufs_trypost(sock_ctx *skctx, bufs_ctx *buf) {
    overlap_tcp_ctx *oltcp = UPCAST(skctx, overlap_tcp_ctx, ol_r);
    qu_bufs_push(&oltcp->buf_s, buf);
    if (!(oltcp->status & STATUS_SENDING)
        && !(oltcp->status & STATUS_ERROR)) {
        oltcp->status |= STATUS_SENDING;
        if (ERR_OK != _post_send(oltcp)) {
            oltcp->status |= STATUS_ERROR;
            oltcp->status = oltcp->status & ~STATUS_SENDING;
        }
    }
}
//connect
static inline int32_t _trybind(SOCKET fd, int32_t family) {
    int32_t rtn;
    netaddr_ctx addr;
    if (AF_INET == family) {
        rtn = netaddr_sethost(&addr, "127.0.0.1", 0);
    } else {
        rtn = netaddr_sethost(&addr, "::1", 0);
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
static inline int32_t _post_connect(overlap_tcp_ctx *oltcp, netaddr_ctx *addr) {
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
static inline void _on_connect_cb(watcher_ctx *watcher, sock_ctx *skctx, DWORD bytes) {
    skctx->ev_cb = _on_recv_cb;
    overlap_tcp_ctx *oltcp = UPCAST(skctx, overlap_tcp_ctx, ol_r);
    if (ERR_OK != sock_checkconn(oltcp->ol_r.fd)) {
        oltcp->cbs.conn_cb(watcher->ev, oltcp->ol_r.fd, ERR_FAILED, &oltcp->ud);
        pool_push(&watcher->pool, &oltcp->ol_r);
        return;
    }
    int32_t handshake = 1;
#if WITH_SSL
    if (NULL != oltcp->ssl) {
        switch (evssl_tryconn(oltcp->ssl)) {
        case ERR_FAILED://错误
            oltcp->cbs.conn_cb(watcher->ev, oltcp->ol_r.fd, ERR_FAILED, &oltcp->ud);
            pool_push(&watcher->pool, &oltcp->ol_r);
            return;
        case 1://完成
            oltcp->status |= STATUS_HANDSHAAKE;
            break;
        case ERR_OK://等待更多数据
            handshake = 0;
            break;
        }
    }
#endif
    _add_fd(watcher, &oltcp->ol_r);
    if (handshake) {
        if (ERR_OK != oltcp->cbs.conn_cb(watcher->ev, oltcp->ol_r.fd, ERR_OK, &oltcp->ud)) {
            _remove_fd(watcher, oltcp->ol_r.fd);
            pool_push(&watcher->pool, &oltcp->ol_r);
            return;
        }
    }
    if (ERR_OK != _post_recv(&oltcp->ol_r, &oltcp->bytes_r, &oltcp->flag, &oltcp->wsabuf, 1)) {
        if (NULL != oltcp->cbs.c_cb
            && handshake) {
            oltcp->cbs.c_cb(watcher->ev, oltcp->ol_r.fd, &oltcp->ud);
        }
        _remove_fd(watcher, oltcp->ol_r.fd);
        pool_push(&watcher->pool, &oltcp->ol_r);
    }
}
SOCKET ev_connect(ev_ctx *ctx, struct evssl_ctx *evssl, const char *host, const uint16_t port, cbs_ctx *cbs, ud_cxt *ud) {
    ASSERTAB(NULL != cbs && NULL != cbs->conn_cb && NULL != cbs->r_cb, ERRSTR_NULLP);
    netaddr_ctx addr;
    if (ERR_OK != netaddr_sethost(&addr, host, port)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return INVALID_SOCK;
    }
    SOCKET fd = _create_sock(SOCK_STREAM, netaddr_family(&addr));
    if (INVALID_SOCK == fd) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return INVALID_SOCK;
    }
    sock_raddr(fd);
    _set_sockops(fd);
    if (ERR_OK != _trybind(fd, netaddr_family(&addr))) {
        CLOSE_SOCK(fd);
        return INVALID_SOCK;
    }
    uint64_t hs = FD_HASH(fd);
    watcher_ctx *watcher = GET_PTR(ctx->watcher, ctx->nthreads, hs);
    if (ERR_OK != _join_iocp(watcher, fd)) {
        CLOSE_SOCK(fd);
        return INVALID_SOCK;
    }
    sock_ctx *skctx = _new_sk(fd, cbs, ud);
    skctx->ev_cb = _on_connect_cb;
    overlap_tcp_ctx *oltcp = UPCAST(skctx, overlap_tcp_ctx, ol_r);
#if WITH_SSL
    if (NULL != evssl) {
        oltcp->ssl = evssl_setfd(evssl, fd);
        if (NULL == oltcp->ssl) {
            _free_sk(skctx);
            return INVALID_SOCK;
        }
    }
#endif
    if (ERR_OK != _post_connect(oltcp, &addr)) {
        _free_sk(skctx);
        return INVALID_SOCK;
    }
    return fd;
}
//listen
static inline int32_t _post_accept(overlap_acpt_ctx *olacp) {
    SOCKET fd = _create_sock(SOCK_STREAM, olacp->lsn->family);
    if (INVALID_SOCK == fd) {
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
                           sizeof(olacp->addr) / 2,
                           sizeof(olacp->addr) / 2,
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
static inline void _on_accept_cb(acceptex_ctx *acpctx, sock_ctx *skctx, DWORD bytes) {
    overlap_acpt_ctx *olacp = UPCAST(skctx, overlap_acpt_ctx, overlap);
    SOCKET fd = olacp->overlap.fd;
    if (ERR_OK != _post_accept(olacp)
        || ERR_OK != setsockopt(fd, 
                                SOL_SOCKET, 
                                SO_UPDATE_ACCEPT_CONTEXT, 
                                (char *)&olacp->lsn->fd, 
                                (int32_t)sizeof(olacp->lsn->fd))
        || ERR_OK != _set_sockops(fd)) {
        CLOSE_SOCK(fd);
        return;
    }
    uint64_t hs = FD_HASH(fd);
    _cmd_add_acpfd(GET_PTR(acpctx->ev->watcher, acpctx->ev->nthreads, hs), fd, olacp->lsn, hs);
}
void _add_acpfd_inloop(watcher_ctx *watcher, SOCKET fd, listener_ctx *lsn) {
    if (ERR_OK != _join_iocp(watcher, fd)) {
        CLOSE_SOCK(fd);
        return;
    }
    int32_t handshake = 1;
    sock_ctx *skctx = pool_pop(&watcher->pool, fd, &lsn->cbs, &lsn->ud);
    overlap_tcp_ctx *oltcp = UPCAST(skctx, overlap_tcp_ctx, ol_r);
#if WITH_SSL
    if (NULL != lsn->evssl) {
        oltcp->ssl = evssl_setfd(lsn->evssl, skctx->fd);
        if (NULL == oltcp->ssl) {
            pool_push(&watcher->pool, skctx);
            return;
        }
        handshake = 0;
        oltcp->status |= STATUS_SERVER;
    }
#endif
    _add_fd(watcher, skctx);
    if (handshake) {
        if (ERR_OK != lsn->cbs.acp_cb(watcher->ev, skctx->fd, &oltcp->ud)) {
            _remove_fd(watcher, skctx->fd);
            pool_push(&watcher->pool, skctx);
            return;
        }
    }
    if (ERR_OK != _post_recv(&oltcp->ol_r, &oltcp->bytes_r, &oltcp->flag, &oltcp->wsabuf, 1)) {
        if (NULL != lsn->cbs.c_cb
            && handshake) {
            lsn->cbs.c_cb(watcher->ev, skctx->fd, &lsn->ud);
        }
        _remove_fd(watcher, skctx->fd);
        pool_push(&watcher->pool, skctx);
    }
}
static inline void _free_acceptex(listener_ctx *lsn, int32_t cnt) {
    for (int32_t i = 0; i < cnt; i++) {
        CLOSE_SOCK(lsn->overlap_acpt[i].overlap.fd);
    }
}
static inline int32_t _acceptex(ev_ctx *ev, listener_ctx *lsn) {
    if (NULL == CreateIoCompletionPort((HANDLE)lsn->fd, ev->acpex[0].iocp, 0, ev->nacpex)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    overlap_acpt_ctx *olacp;
    for (int32_t i = 0; i < MAX_ACCEPTEX_CNT; i++) {
        olacp = &lsn->overlap_acpt[i];
        olacp->lsn = lsn;
        olacp->overlap.fd = INVALID_SOCK;
        olacp->overlap.ev_cb = _on_accept_cb;
        if (ERR_OK != _post_accept(olacp)) {
            _free_acceptex(lsn, i);
            return ERR_FAILED;
        }
    }
    return ERR_OK;
}
int32_t ev_listen(ev_ctx *ctx, struct evssl_ctx *evssl, const char *host, const uint16_t port, cbs_ctx *cbs, ud_cxt *ud) {
    ASSERTAB(NULL != cbs && NULL != cbs->acp_cb && NULL != cbs->r_cb, ERRSTR_NULLP);
    netaddr_ctx addr;
    if (ERR_OK != netaddr_sethost(&addr, host, port)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    SOCKET fd = _listen(&addr);
    if (INVALID_SOCK == fd) {
        return ERR_FAILED;
    }
    listener_ctx *lsn;
    MALLOC(lsn, sizeof(listener_ctx));
    lsn->family = netaddr_family(&addr);
    lsn->fd = fd;
    lsn->cbs = *cbs;
    COPY_UD(lsn->ud, ud);
#if WITH_SSL
    lsn->evssl = evssl;
#endif
    if (ERR_OK != _acceptex(ctx, lsn)) {
        CLOSE_SOCK(fd);
        FREE(lsn);
        return ERR_FAILED;
    }
    mutex_lock(&ctx->qulsnlck);
    qu_lsn_push(&ctx->qulsn, &lsn);
    mutex_unlock(&ctx->qulsnlck);
    return ERR_OK;
}
void _freelsn(listener_ctx *lsn) {
    _free_acceptex(lsn, MAX_ACCEPTEX_CNT);
    CLOSE_SOCK(lsn->fd);
    FREE(lsn);
}
//UDP
static inline int32_t _post_recv_from(overlap_udp_ctx *oludp) {
    ZERO(&oludp->ol_r.overlapped, sizeof(oludp->ol_r.overlapped));
    oludp->flag = oludp->bytes_r = 0;
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
static inline void _on_udp_close_r(watcher_ctx *watcher, overlap_udp_ctx *oludp) {
    if (oludp->status & STATUS_SENDING) {
        oludp->status |= STATUS_REMOVE;
    } else {
        _remove_fd(watcher, oludp->ol_r.fd);
        _free_udp(&oludp->ol_r);
    }
}
static inline void _on_recvfrom_cb(watcher_ctx *watcher, sock_ctx *skctx, DWORD bytes) {
    overlap_udp_ctx *oludp = UPCAST(skctx, overlap_udp_ctx, ol_r);
    if (0 == bytes) {
        oludp->status |= STATUS_ERROR;
        _on_udp_close_r(watcher, oludp);
        return;
    }
    oludp->cbs.rf_cb(watcher->ev, oludp->ol_r.fd, oludp->wsabuf_r.buf, (size_t)bytes, &oludp->addr, &oludp->ud);
    if (oludp->status & STATUS_ERROR) {//多处理一个数据包
        _on_udp_close_r(watcher, oludp);
        return;
    }
    if (ERR_OK != _post_recv_from(oludp)) {
        oludp->status |= STATUS_ERROR;
        _on_udp_close_r(watcher, oludp);
    }
}
static inline int32_t _post_sendto(overlap_udp_ctx *oludp, bufs_ctx *buf) {
    ZERO(&oludp->ol_s.overlapped, sizeof(oludp->ol_s.overlapped));
    oludp->bytes_s = 0;
    netaddr_ctx *addr = (netaddr_ctx *)buf->data;
    oludp->wsabuf_s.IOV_PTR_FIELD = (char *)buf->data + sizeof(netaddr_ctx);
    oludp->wsabuf_s.IOV_LEN_FIELD = (IOV_LEN_TYPE)buf->len;
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
static inline void _on_sendto_cb(watcher_ctx *watcher, sock_ctx *skctx, DWORD bytes) {
    overlap_udp_ctx *oludp = UPCAST(skctx, overlap_udp_ctx, ol_s); 
    void *data = oludp->wsabuf_s.IOV_PTR_FIELD - sizeof(netaddr_ctx);
    FREE(data);
    if (oludp->status & STATUS_ERROR) {
        if (oludp->status & STATUS_REMOVE) {
            _remove_fd(watcher, oludp->ol_r.fd);
            _free_udp(&oludp->ol_r);
        } else {
            oludp->status = oludp->status & ~STATUS_SENDING;
        }
        return;
    }
    if (0 == qu_bufs_size(&oludp->buf_s)) {
        oludp->status = oludp->status & ~STATUS_SENDING;
        return;
    }
    bufs_ctx *sendbuf = qu_bufs_pop(&oludp->buf_s);
    if (ERR_OK != _post_sendto(oludp, sendbuf)) {
        FREE(sendbuf->data);
        oludp->status |= STATUS_ERROR;
        oludp->status = oludp->status & ~STATUS_SENDING;
    }
}
void _add_bufs_trysendto(sock_ctx *skctx, bufs_ctx *buf) {
    overlap_udp_ctx *oludp = UPCAST(skctx, overlap_udp_ctx, ol_r);
    qu_bufs_push(&oludp->buf_s, buf);
    if (!(oludp->status & STATUS_SENDING)
        && !(oludp->status & STATUS_ERROR)) {
        oludp->status |= STATUS_SENDING;
        bufs_ctx *sendbuf = qu_bufs_pop(&oludp->buf_s);
        if (ERR_OK != _post_sendto(oludp, sendbuf)) {
            FREE(sendbuf->data);
            oludp->status |= STATUS_ERROR;
            oludp->status = oludp->status & ~STATUS_SENDING;
        }
    }
}
static inline sock_ctx *_new_udp(netaddr_ctx *addr, SOCKET fd, cbs_ctx *cbs, ud_cxt *ud) {
    overlap_udp_ctx *oludp;
    MALLOC(oludp, sizeof(overlap_udp_ctx));
    oludp->ol_r.type = SOCK_DGRAM;
    oludp->ol_r.fd = fd;
    oludp->ol_r.ev_cb = _on_recvfrom_cb;
    oludp->ol_s.type = SOCK_DGRAM;
    oludp->ol_s.fd = fd;
    oludp->ol_s.ev_cb = _on_sendto_cb;
    oludp->status = 0;
    oludp->cbs = *cbs;
    COPY_UD(oludp->ud, ud);
    netaddr_empty_addr(&oludp->addr, netaddr_family(addr));
    oludp->addrlen = netaddr_size(&oludp->addr);
    oludp->wsabuf_r.buf = oludp->buf;
    oludp->wsabuf_r.len = sizeof(oludp->buf);
    qu_bufs_init(&oludp->buf_s, INIT_SENDBUF_LEN);
    return &oludp->ol_r;
}
void _free_udp(sock_ctx *skctx) {
    overlap_udp_ctx *oludp = UPCAST(skctx, overlap_udp_ctx, ol_r);
    CLOSE_SOCK(oludp->ol_r.fd);
    _bufs_clear(&oludp->buf_s);
    qu_bufs_free(&oludp->buf_s);
    if (NULL != oludp->cbs.ud_free) {
        oludp->cbs.ud_free(&oludp->ud);
    }
    FREE(oludp);
}
SOCKET ev_udp(ev_ctx *ctx, const char *host, const uint16_t port, cbs_ctx *cbs, ud_cxt *ud) {
    ASSERTAB(NULL != cbs->rf_cb, ERRSTR_NULLP);
    netaddr_ctx addr;
    if (ERR_OK != netaddr_sethost(&addr, host, port)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return INVALID_SOCK;
    }
    SOCKET fd = _udp(&addr);
    if (INVALID_SOCK == fd) {
        return INVALID_SOCK;
    }
    uint64_t hs = FD_HASH(fd);
    watcher_ctx *watcher = GET_PTR(ctx->watcher, ctx->nthreads, hs);
    if (ERR_OK != _join_iocp(watcher, fd)) {
        CLOSE_SOCK(fd);
        return INVALID_SOCK;
    }
    sock_ctx *skctx = _new_udp(&addr, fd, cbs, ud);
    _cmd_add(watcher, skctx, hs);
    if (ERR_OK != _post_recv_from(UPCAST(skctx, overlap_udp_ctx, ol_r))) {
        _cmd_remove(watcher, fd, hs);
        return INVALID_SOCK;
    }
    return fd;
}

#endif
