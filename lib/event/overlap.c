#include "event/event.h"
#include "event/iocp.h"
#include "event/skpool.h"
#include "buffer.h"
#include "loger.h"
#include "hashmap.h"
#include "netutils.h"

#ifdef EV_IOCP

#define MAX_ACCEPTEX_CNT    128

typedef struct overlap_acpt_ctx
{
    sock_ctx overlap;
    struct listener_ctx *lsn;
    DWORD bytes;
    char addr[sizeof(struct sockaddr_storage)];
}overlap_acpt_ctx;
typedef struct listener_ctx
{
    int32_t family;
    SOCKET fd;
#if WITH_SSL
    evssl_ctx *evssl;
#endif
    cbs_ctx cbs;
    ud_cxt ud;
    overlap_acpt_ctx overlap_acpt[MAX_ACCEPTEX_CNT];
}listener_ctx;
typedef struct overlap_tcp_ctx
{
    sock_ctx ol_r;
    sock_ctx ol_s;
    volatile int32_t sending;
    volatile int32_t erro;
    DWORD bytes_r;
    DWORD bytes_s;
    DWORD flag;
#if WITH_SSL
    int32_t server;
    int32_t handshake;
    SSL *ssl;
#endif
    IOV_TYPE wsabuf;
    buffer_ctx buf_r;
    qu_bufs buf_s;
    cbs_ctx cbs;
    ud_cxt ud;
}overlap_tcp_ctx;
typedef struct overlap_udp_ctx
{
    sock_ctx ol_r;
    sock_ctx ol_s;
    int32_t addrlen;
    uint32_t niov;
    volatile int32_t sending;
    volatile int32_t erro;
    DWORD bytes_r;
    DWORD bytes_s;
    DWORD flag;
    recvfrom_cb rf_cb;
    IOV_TYPE wsabuf_s;
    IOV_TYPE wsabuf_r[MAX_EXPAND_NIOV];
    buffer_ctx buf_r;
    qu_bufs buf_s;
    netaddr_ctx addr;
    ud_cxt ud;
}overlap_udp_ctx;

static void _on_recv_cb(watcher_ctx *watcher, sock_ctx *skctx, DWORD bytes);
static void _on_send_cb(watcher_ctx *watcher, sock_ctx *skctx, DWORD bytes);

void _sk_shutdown(sock_ctx *skctx)
{
#if WITH_SSL
    overlap_tcp_ctx *ol = UPCAST(skctx, overlap_tcp_ctx, ol_r);
    evssl_shutdown(ol->ssl, ol->ol_r.fd);
#else
    shutdown(skctx->fd, SHUT_RD);
#endif
}
sock_ctx *_new_sk(SOCKET fd, cbs_ctx *cbs, ud_cxt *ud)
{
    overlap_tcp_ctx *ol;
    MALLOC(ol, sizeof(overlap_tcp_ctx));
    ol->ol_r.type = SOCK_STREAM;
    ol->ol_r.fd = fd;
    ol->ol_r.ev_cb = _on_recv_cb;
    ol->ol_s.type = SOCK_STREAM;
    ol->ol_s.fd = fd;
    ol->ol_s.ev_cb = _on_send_cb;
    ol->sending = 0;
    ol->erro = 0;
#if WITH_SSL
    ol->ssl = NULL;
#endif
    ol->wsabuf.IOV_PTR_FIELD = NULL;
    ol->wsabuf.IOV_LEN_FIELD = 0;
    ol->cbs = *cbs;
    COPY_UD(ol->ud, ud);
    buffer_init(&ol->buf_r);
    qu_bufs_init(&ol->buf_s, INIT_SENDBUF_LEN);
    return &ol->ol_r;
}
void _free_sk(sock_ctx *skctx)
{
    overlap_tcp_ctx *ol = UPCAST(skctx, overlap_tcp_ctx, ol_r);
#if WITH_SSL
    FREE_SSL(ol->ssl);
#endif
    CLOSE_SOCK(ol->ol_r.fd);
    buffer_free(&ol->buf_r);
    _bufs_clear(&ol->buf_s);
    qu_bufs_free(&ol->buf_s);
    FREE(ol);
}
void _clear_sk(sock_ctx *skctx)
{
    overlap_tcp_ctx *ol = UPCAST(skctx, overlap_tcp_ctx, ol_r);
    ol->sending = 0;
    ol->erro = 0;
#if WITH_SSL
    FREE_SSL(ol->ssl);
#endif
    CLOSE_SOCK(ol->ol_r.fd);
    ol->ol_s.fd = INVALID_SOCK;
    _bufs_clear(&ol->buf_s);
    buffer_drain(&ol->buf_r, buffer_size(&ol->buf_r));
}
void _reset_sk(sock_ctx *skctx, SOCKET fd, cbs_ctx *cbs, ud_cxt *ud)
{
    overlap_tcp_ctx *ol = UPCAST(skctx, overlap_tcp_ctx, ol_r);
    ol->ol_r.fd = fd;
    ol->ol_s.fd = fd;
    ol->cbs = *cbs;
    COPY_UD(ol->ud, ud);
}
int32_t _check_canfree(sock_ctx *skctx)
{
    if (SOCK_STREAM == skctx->type)
    {
        return UPCAST(skctx, overlap_tcp_ctx, ol_r)->sending;
    }
    return UPCAST(skctx, overlap_udp_ctx, ol_r)->sending;
}
void _add_fd(watcher_ctx *watcher, sock_ctx *skctx)
{
    map_element el;
    el.fd = skctx->fd;
    el.sock = skctx;
    ASSERTAB(NULL == hashmap_set(watcher->element, &el), "socket repeat.");
}
void _remove_fd(watcher_ctx *watcher, SOCKET fd)
{
    map_element key;
    key.fd = fd;
    hashmap_delete(watcher->element, &key);
}
//recv
int32_t _post_recv(sock_ctx *skctx, DWORD  *bytes, DWORD  *flag, IOV_TYPE *wsabuf, DWORD niov)
{
    *flag = 0;
    *bytes = 0;
    ZERO(&skctx->overlapped, sizeof(skctx->overlapped));
    if (ERR_OK != WSARecv(skctx->fd,
                          wsabuf,
                          niov,
                          bytes,
                          flag,
                          &skctx->overlapped,
                          NULL))
    {
        if (ERROR_IO_PENDING != ERRNO)
        {
            return ERR_FAILED;
        }
    }
    return ERR_OK;
}
#if WITH_SSL
static inline int32_t _ssl_handshake_acpt(watcher_ctx *watcher, overlap_tcp_ctx *ol)
{
    int32_t rtn = ERR_FAILED;
    switch (evssl_tryacpt(ol->ssl))
    {
    case ERR_FAILED:
        _remove_fd(watcher, ol->ol_r.fd);
        pool_push(&watcher->pool, &ol->ol_r);
        break;
    case 1:
        if (ERR_OK != ol->cbs.acp_cb(watcher->ev, ol->ol_r.fd, &ol->ud))
        {
            _remove_fd(watcher, ol->ol_r.fd);
            pool_push(&watcher->pool, &ol->ol_r);
            break;
        }
        ol->handshake = 1;
        rtn = ERR_OK;
        break;
    case ERR_OK:
        if (ERR_OK != _post_recv(&ol->ol_r, &ol->bytes_r, &ol->flag, &ol->wsabuf, 1))
        {
            _remove_fd(watcher, ol->ol_r.fd);
            pool_push(&watcher->pool, &ol->ol_r);
        }
        break;
    }
    return rtn;
}
static inline int32_t _ssl_handshake_conn(watcher_ctx *watcher, overlap_tcp_ctx *ol)
{
    int32_t rtn = ERR_FAILED;
    switch (evssl_tryconn(ol->ssl))
    {
    case ERR_FAILED:
        ol->cbs.conn_cb(watcher->ev, INVALID_SOCK, &ol->ud);
        _remove_fd(watcher, ol->ol_r.fd);
        pool_push(&watcher->pool, &ol->ol_r);
        break;
    case 1:
        if (ERR_OK != ol->cbs.conn_cb(watcher->ev, ol->ol_r.fd, &ol->ud))
        {
            _remove_fd(watcher, ol->ol_r.fd);
            pool_push(&watcher->pool, &ol->ol_r);
            break;
        }
        ol->handshake = 1;
        rtn = ERR_OK;
        break;
    case ERR_OK:
        if (ERR_OK != _post_recv(&ol->ol_r, &ol->bytes_r, &ol->flag, &ol->wsabuf, 1))
        {
            ol->cbs.conn_cb(watcher->ev, INVALID_SOCK, &ol->ud);
            _remove_fd(watcher, ol->ol_r.fd);
            pool_push(&watcher->pool, &ol->ol_r);
        }
        break;
    }
    return rtn;
}
static inline int32_t _ssl_handshake(watcher_ctx *watcher, overlap_tcp_ctx *ol)
{
    if (ol->server)
    {
        return _ssl_handshake_acpt(watcher, ol);
    }
    return _ssl_handshake_conn(watcher, ol);
}
#endif
static inline void _tcp_recv(watcher_ctx *watcher, overlap_tcp_ctx *ol)
{
    size_t nread;
#if WITH_SSL
    int32_t rtn = buffer_from_sock(&ol->buf_r, ol->ol_r.fd, &nread, _sock_read, ol->ssl);
#else
    int32_t rtn = buffer_from_sock(&ol->buf_r, ol->ol_r.fd, &nread, _sock_read, NULL);
#endif
    if (0 != nread)
    {
        ol->cbs.r_cb(watcher->ev, ol->ol_r.fd, &ol->buf_r, nread, &ol->ud);
    }
    if (ERR_OK == rtn)
    {
        rtn = _post_recv(&ol->ol_r, &ol->bytes_r, &ol->flag, &ol->wsabuf, 1);
    }
    if (ERR_OK != rtn)
    {
        ol->erro = 1;
        if (NULL != ol->cbs.c_cb)
        {
            ol->cbs.c_cb(watcher->ev, ol->ol_r.fd, &ol->ud);
        }
        _remove(watcher, &ol->ol_r);
    }
}
static void _on_recv_cb(watcher_ctx *watcher, sock_ctx *skctx, DWORD bytes)
{
    overlap_tcp_ctx *ol = UPCAST(skctx, overlap_tcp_ctx, ol_r);
#if WITH_SSL
    if (NULL != ol->ssl
        && !ol->handshake)
    {
        if (ERR_OK != _ssl_handshake(watcher, ol))
        {
            return;
        }
    }
#endif
    _tcp_recv(watcher, ol);
}
//send
static inline int32_t _post_send(overlap_tcp_ctx *ol)
{
    ol->bytes_s = 0;
    ZERO(&ol->ol_s.overlapped, sizeof(ol->ol_s.overlapped));
    if (ERR_OK != WSASend(ol->ol_s.fd,
                          &ol->wsabuf,
                          1,
                          &ol->bytes_s,
                          0,
                          &ol->ol_s.overlapped,
                          NULL))
    {
        if (ERROR_IO_PENDING != ERRNO)
        {
            return ERR_FAILED;
        }
    }
    return ERR_OK;
}
static void _on_send_cb(watcher_ctx *watcher, sock_ctx *skctx, DWORD bytes)
{
    overlap_tcp_ctx *ol = UPCAST(skctx, overlap_tcp_ctx, ol_s);
    size_t nsend;
#if WITH_SSL
    int32_t rtn = _sock_send(ol->ol_s.fd, &ol->buf_s, &nsend, ol->ssl);
#else
    int32_t rtn = _sock_send(ol->ol_s.fd, &ol->buf_s, &nsend, NULL);
#endif
    if (NULL != ol->cbs.s_cb
        && 0 != nsend
        && 0 == ol->erro)
    {
        ol->cbs.s_cb(watcher->ev, ol->ol_s.fd, nsend, &ol->ud);
    }
    if (0 != ol->erro
        || ERR_OK != rtn)
    {
        ol->erro = 1;
        ol->sending = 0;
        return;
    }
    if (qu_bufs_size(&ol->buf_s) > 0)
    {
        if (ERR_OK != _post_send(ol))
        {
            ol->erro = 1;
            ol->sending = 0;
        }
    }
    else
    {
        ol->sending = 0;
    }
}
void _add_bufs_trypost(sock_ctx *skctx, bufs_ctx *buf)
{
    overlap_tcp_ctx *ol = UPCAST(skctx, overlap_tcp_ctx, ol_r);
    qu_bufs_push(&ol->buf_s, buf);
    if (0 == ol->sending
        && 0 == ol->erro)
    {
        ol->sending = 1;
        if (ERR_OK != _post_send(ol))
        {
            ol->erro = 1;
            ol->sending = 0;
        }
    }
}
//connect
static inline int32_t _trybind(SOCKET fd, int32_t family)
{
    int32_t rtn;
    netaddr_ctx addr;
    if (AF_INET == family)
    {
        rtn = netaddr_sethost(&addr, "127.0.0.1", 0);
    }
    else
    {
        rtn = netaddr_sethost(&addr, "::1", 0);
    }
    if (ERR_OK != rtn)
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    if (ERR_OK != bind(fd, netaddr_addr(&addr), netaddr_size(&addr)))
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    return ERR_OK;
}
static inline int32_t _post_connect(overlap_tcp_ctx *ol, netaddr_ctx *addr)
{
    ol->bytes_r = 0;
    ZERO(&ol->ol_r.overlapped, sizeof(ol->ol_r.overlapped));
    if (!_exfuncs.connectex(ol->ol_r.fd,
                            netaddr_addr(addr),
                            netaddr_size(addr),
                            NULL,
                            0,
                            &ol->bytes_r,
                            &ol->ol_r.overlapped))
    {
        int32_t erro = ERRNO;
        if (ERROR_IO_PENDING != erro)
        {
            LOG_WARN("%s", ERRORSTR(erro));
            return ERR_FAILED;
        }
    }
    return ERR_OK;
}
static inline void _on_connect_cb(watcher_ctx *watcher, sock_ctx *skctx, DWORD bytes)
{
    skctx->ev_cb = _on_recv_cb;
    overlap_tcp_ctx *ol = UPCAST(skctx, overlap_tcp_ctx, ol_r);
    if (ERR_OK != sock_checkconn(ol->ol_r.fd))
    {
        ol->cbs.conn_cb(watcher->ev, INVALID_SOCK, &ol->ud);
        pool_push(&watcher->pool, &ol->ol_r);
        return;
    }
    int32_t handshake = 1;
#if WITH_SSL
    if (NULL != ol->ssl)
    {
        ol->server = 0;
        int32_t rtn = evssl_tryconn(ol->ssl);
        if (ERR_OK == rtn)
        {
            handshake = 0;
            ol->handshake = 0;
        }
        else if(1 == rtn)
        {
            ol->handshake = 1;
        }
        else//-1
        {
            ol->cbs.conn_cb(watcher->ev, INVALID_SOCK, &ol->ud);
            pool_push(&watcher->pool, &ol->ol_r);
            return;
        }
    }
#endif
    _add_fd(watcher, &ol->ol_r);
    if (handshake)
    {
        if (ERR_OK != ol->cbs.conn_cb(watcher->ev, ol->ol_r.fd, &ol->ud))
        {
            _remove_fd(watcher, ol->ol_r.fd);
            pool_push(&watcher->pool, &ol->ol_r);
            return;
        }
    }
    if (ERR_OK != _post_recv(&ol->ol_r, &ol->bytes_r, &ol->flag, &ol->wsabuf, 1))
    {
        if (NULL != ol->cbs.c_cb
            && handshake)
        {
            ol->cbs.c_cb(watcher->ev, ol->ol_r.fd, &ol->ud);
        }
        _remove_fd(watcher, ol->ol_r.fd);
        pool_push(&watcher->pool, &ol->ol_r);
    }
}
SOCKET ev_connect(ev_ctx *ctx, struct evssl_ctx *evssl, const char *host, const uint16_t port, cbs_ctx *cbs, ud_cxt *ud)
{
    ASSERTAB(NULL != cbs && NULL != cbs->conn_cb && NULL != cbs->r_cb, ERRSTR_NULLP);
    netaddr_ctx addr;
    if (ERR_OK != netaddr_sethost(&addr, host, port))
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return INVALID_SOCK;
    }
    SOCKET fd = _create_sock(SOCK_STREAM, netaddr_family(&addr));
    if (INVALID_SOCK == fd)
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return INVALID_SOCK;
    }
    sock_raddr(fd);
    _set_sockops(fd);
    if (ERR_OK != _trybind(fd, netaddr_family(&addr)))
    {
        CLOSE_SOCK(fd);
        return INVALID_SOCK;
    }
    uint64_t hs = FD_HASH(fd);
    watcher_ctx *watcher = GET_PTR(ctx->watcher, ctx->nthreads, hs);
    if (ERR_OK != _join_iocp(watcher, fd))
    {
        CLOSE_SOCK(fd);
        return INVALID_SOCK;
    }    
    sock_ctx *rwctx = _new_sk(fd, cbs, ud);
    rwctx->ev_cb = _on_connect_cb;
    overlap_tcp_ctx *ol = UPCAST(rwctx, overlap_tcp_ctx, ol_r);
#if WITH_SSL
    if (NULL != evssl)
    {
        ol->ssl = evssl_setfd(evssl, fd);
        if (NULL == ol->ssl)
        {
            _free_sk(rwctx);
            return INVALID_SOCK;
        }
    }
#endif
    if (ERR_OK != _post_connect(ol, &addr))
    {
        _free_sk(rwctx);
        return INVALID_SOCK;
    }
    return fd;
}
//listen
static inline int32_t _post_accept(overlap_acpt_ctx *ol)
{
    SOCKET fd = _create_sock(SOCK_STREAM, ol->lsn->family);
    if (INVALID_SOCK == fd)
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    ol->bytes = 0;
    ol->overlap.fd = fd;
    ZERO(&ol->overlap.overlapped, sizeof(ol->overlap.overlapped));
    if (!_exfuncs.acceptex(ol->lsn->fd,//Listen Socket
                           ol->overlap.fd,//Accept Socket
                           &ol->addr,
                           0,
                           sizeof(ol->addr) / 2,
                           sizeof(ol->addr) / 2,
                           &ol->bytes,
                           &ol->overlap.overlapped))
    {
        int32_t erro = ERRNO;
        if (ERROR_IO_PENDING != erro)
        {
            CLOSE_SOCK(ol->overlap.fd);
            LOG_ERROR("%s", ERRORSTR(erro));
            return ERR_FAILED;
        }
    }
    return ERR_OK;
}
static inline void _on_accept_cb(watcher_ctx *watcher, sock_ctx *skctx, DWORD bytes)
{
    overlap_acpt_ctx *acpol = UPCAST(skctx, overlap_acpt_ctx, overlap);
    SOCKET fd = acpol->overlap.fd;
    uint64_t hs = FD_HASH(fd);
    watcher_ctx *to = GET_PTR(watcher->ev->watcher, watcher->ev->nthreads, hs);
    if (ERR_OK != _post_accept(acpol)
        || ERR_OK != setsockopt(fd, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, 
                                (char *)&acpol->lsn->fd, (int32_t)sizeof(acpol->lsn->fd))
        || ERR_OK != _set_sockops(fd)
        || ERR_OK != _join_iocp(to, fd))
    {
        CLOSE_SOCK(fd);
        return;
    }
    if (to->index == watcher->index)
    {
        _add_acpfd_inloop(to, fd, acpol->lsn);
    }
    else
    {
        _cmd_add_acpfd(to, fd, acpol->lsn, hs);
    }
}
void _add_acpfd_inloop(watcher_ctx *watcher, SOCKET fd, struct listener_ctx *lsn)
{
    int32_t handshake = 1;
    sock_ctx *rwctx = pool_pop(&watcher->pool, fd, &lsn->cbs, &lsn->ud);
    overlap_tcp_ctx *olrw = UPCAST(rwctx, overlap_tcp_ctx, ol_r);
#if WITH_SSL
    if (NULL != lsn->evssl)
    {
        olrw->ssl = evssl_setfd(lsn->evssl, fd);
        if (NULL == olrw->ssl)
        {
            pool_push(&watcher->pool, rwctx);
            return;
        }
        handshake = 0;
        olrw->handshake = 0;
        olrw->server = 1;
    }
#endif
    _add_fd(watcher, rwctx);
    if (handshake)
    {
        if (ERR_OK != lsn->cbs.acp_cb(watcher->ev, fd, &lsn->ud))
        {
            _remove_fd(watcher, rwctx->fd);
            pool_push(&watcher->pool, rwctx);
            return;
        }
    }
    if (ERR_OK != _post_recv(&olrw->ol_r, &olrw->bytes_r, &olrw->flag, &olrw->wsabuf, 1))
    {
        if (NULL != lsn->cbs.c_cb
            && handshake)
        {
            lsn->cbs.c_cb(watcher->ev, fd, &lsn->ud);
        }
        _remove_fd(watcher, rwctx->fd);
        pool_push(&watcher->pool, rwctx);
    }
}
static inline void _free_acceptex(listener_ctx *lsn, int32_t cnt)
{
    for (int32_t i = 0; i < cnt; i++)
    {
        CLOSE_SOCK(lsn->overlap_acpt[i].overlap.fd);
    }
}
static inline int32_t _acceptex(ev_ctx *ev, listener_ctx *lsn)
{
    if (ERR_OK != _join_iocp(GET_PTR(ev->watcher, ev->nthreads, FD_HASH(lsn->fd)), lsn->fd))
    {
        return ERR_FAILED;
    }
    overlap_acpt_ctx *ol;
    for (int32_t i = 0; i < MAX_ACCEPTEX_CNT; i++)
    {
        ol = &lsn->overlap_acpt[i];
        ol->lsn = lsn;
        ol->overlap.fd = INVALID_SOCK;
        ol->overlap.ev_cb = _on_accept_cb;
        if (ERR_OK != _post_accept(ol))
        {
            _free_acceptex(lsn, i);
            return ERR_FAILED;
        }
    }
    return ERR_OK;
}
int32_t ev_listen(ev_ctx *ctx, struct evssl_ctx *evssl, const char *host, const uint16_t port, cbs_ctx *cbs, ud_cxt *ud)
{
    ASSERTAB(NULL != cbs && NULL != cbs->acp_cb && NULL != cbs->r_cb, ERRSTR_NULLP);
    netaddr_ctx addr;
    if (ERR_OK != netaddr_sethost(&addr, host, port))
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    SOCKET fd = _listen(&addr);
    if (INVALID_SOCK == fd)
    {
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
    if (ERR_OK != _acceptex(ctx, lsn))
    {
        CLOSE_SOCK(fd);
        FREE(lsn);
        return ERR_FAILED;
    }
    mutex_lock(&ctx->qulsnlck);
    qu_lsn_push(&ctx->qulsn, &lsn);
    mutex_unlock(&ctx->qulsnlck);
    return ERR_OK;
}
void _freelsn(listener_ctx *lsn)
{
    _free_acceptex(lsn, MAX_ACCEPTEX_CNT);
    CLOSE_SOCK(lsn->fd);
    FREE(lsn);
}
//UDP
static inline int32_t _post_recv_from(overlap_udp_ctx *ol)
{
    ZERO(&ol->ol_r.overlapped, sizeof(ol->ol_r.overlapped));
    ol->flag = ol->bytes_r = 0;
    ol->niov = buffer_expand(&ol->buf_r, MAX_RECVFROM_SIZE, ol->wsabuf_r, MAX_EXPAND_NIOV);
    if (ERR_OK != WSARecvFrom(ol->ol_r.fd,
                              ol->wsabuf_r,
                              (DWORD)ol->niov,
                              &ol->bytes_r,
                              &ol->flag,
                              netaddr_addr(&ol->addr),
                              &ol->addrlen,
                              &ol->ol_r.overlapped,
                              NULL))
    {
        if (WSA_IO_PENDING != ERRNO)
        {
            return ERR_FAILED;
        }
    }
    return ERR_OK;
}
static inline void _on_recvfrom_cb(watcher_ctx *watcher, sock_ctx *skctx, DWORD bytes)
{
    overlap_udp_ctx *ol = UPCAST(skctx, overlap_udp_ctx, ol_r);
    if (0 == bytes)
    {
        ol->erro = 1;
        _remove(watcher, &ol->ol_r);
        return;
    }
    buffer_commit_expand(&ol->buf_r, (size_t)bytes, ol->wsabuf_r, ol->niov);
    ol->rf_cb(watcher->ev, ol->ol_r.fd, &ol->buf_r, (size_t)bytes, &ol->addr, &ol->ud);
    if (ERR_OK != _post_recv_from(ol))
    {
        ol->erro = 1;
        _remove(watcher, &ol->ol_r);
    }
}
static inline int32_t _post_sendto(overlap_udp_ctx *ol, bufs_ctx *buf)
{
    ZERO(&ol->ol_s.overlapped, sizeof(ol->ol_s.overlapped));
    ol->bytes_s = 0;
    netaddr_ctx *addr = (netaddr_ctx *)buf->data;
    ol->wsabuf_s.IOV_PTR_FIELD = (char *)buf->data + sizeof(netaddr_ctx);
    ol->wsabuf_s.IOV_LEN_FIELD = (IOV_LEN_TYPE)buf->len;
    if (ERR_OK != WSASendTo(ol->ol_s.fd,
                            &ol->wsabuf_s,
                            1,
                            &ol->bytes_s,
                            0,
                            netaddr_addr(addr),
                            netaddr_size(addr),
                            &ol->ol_s.overlapped,
                            NULL))
    {
        if (ERROR_IO_PENDING != ERRNO)
        {
            return ERR_FAILED;
        }
    }
    return ERR_OK;
}
static inline void _on_sendto_cb(watcher_ctx *watcher, sock_ctx *skctx, DWORD bytes)
{
    overlap_udp_ctx *ol = UPCAST(skctx, overlap_udp_ctx, ol_s); 
    void *data = ol->wsabuf_s.IOV_PTR_FIELD - sizeof(netaddr_ctx);
    FREE(data);
    if (0 != ol->erro)
    {
        ol->sending = 0;
        return;
    }
    if (qu_bufs_size(&ol->buf_s) > 0)
    {
        bufs_ctx *sendbuf = qu_bufs_pop(&ol->buf_s);
        if (ERR_OK != _post_sendto(ol, sendbuf))
        {
            FREE(sendbuf->data);
            ol->erro = 1;
            ol->sending = 0;
        }
    }
    else
    {
        ol->sending = 0;
    }
}
void _add_bufs_trysendto(sock_ctx *skctx, bufs_ctx *buf)
{
    overlap_udp_ctx *ol = UPCAST(skctx, overlap_udp_ctx, ol_r);
    qu_bufs_push(&ol->buf_s, buf);
    if (0 == ol->sending
        && 0 == ol->erro)
    {
        ol->sending = 1;
        bufs_ctx *sendbuf = qu_bufs_pop(&ol->buf_s);
        if (ERR_OK != _post_sendto(ol, sendbuf))
        {
            FREE(sendbuf->data);
            ol->erro = 1;
            ol->sending = 0;
        }
    }
}
static inline sock_ctx *_new_udp(netaddr_ctx *addr, SOCKET fd, recvfrom_cb rf_cb, ud_cxt *ud)
{
    overlap_udp_ctx *ol;
    MALLOC(ol, sizeof(overlap_udp_ctx));
    ol->ol_r.type = SOCK_DGRAM;
    ol->ol_r.fd = fd;
    ol->ol_r.ev_cb = _on_recvfrom_cb;
    ol->ol_s.type = SOCK_DGRAM;
    ol->ol_s.fd = fd;
    ol->ol_s.ev_cb = _on_sendto_cb;
    ol->sending = 0;
    ol->erro = 0;
    ol->rf_cb = rf_cb;
    COPY_UD(ol->ud, ud);
    netaddr_empty_addr(&ol->addr, netaddr_family(addr));
    ol->addrlen = netaddr_size(&ol->addr);
    buffer_init(&ol->buf_r);
    qu_bufs_init(&ol->buf_s, INIT_SENDBUF_LEN);
    return &ol->ol_r;
}
void _free_udp(sock_ctx *skctx)
{
    overlap_udp_ctx *ol = UPCAST(skctx, overlap_udp_ctx, ol_r);
    CLOSE_SOCK(ol->ol_r.fd);
    buffer_free(&ol->buf_r);
    _bufs_clear(&ol->buf_s);
    qu_bufs_free(&ol->buf_s);
    FREE(ol);
}
SOCKET ev_udp(ev_ctx *ctx, const char *host, const uint16_t port, recvfrom_cb rf_cb, ud_cxt *ud)
{
    ASSERTAB(NULL != rf_cb, ERRSTR_NULLP);
    netaddr_ctx addr;
    if (ERR_OK != netaddr_sethost(&addr, host, port))
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return INVALID_SOCK;
    }
    SOCKET fd = _udp(&addr);
    if (INVALID_SOCK == fd)
    {
        return INVALID_SOCK;
    }
    uint64_t hs = FD_HASH(fd);
    watcher_ctx *watcher = GET_PTR(ctx->watcher, ctx->nthreads, hs);
    if (ERR_OK != _join_iocp(watcher, fd))
    {
        CLOSE_SOCK(fd);
        return INVALID_SOCK;
    }
    sock_ctx *ol = _new_udp(&addr, fd, rf_cb, ud);
    _cmd_add(watcher, ol, hs);
    if (ERR_OK != _post_recv_from(UPCAST(ol, overlap_udp_ctx, ol_r)))
    {
        _cmd_remove(watcher, fd, hs);
        return INVALID_SOCK;
    }
    return fd;
}

#endif
