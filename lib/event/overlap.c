#include "event/event.h"
#include "event/iocp.h"
#include "event/worker.h"
#include "buffer.h"
#include "loger.h"
#include "netutils.h"

#ifdef EV_IOCP

#define MAX_ACCEPTEX_CNT    512

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
    lsn_cb_ctx lsn_cb;
    freeud_cb fud_cb;
    ud_cxt ud;
    overlap_acpt_ctx overlap_acpt[MAX_ACCEPTEX_CNT];
}listener_ctx;
typedef struct overlap_conn_ctx
{
    sock_ctx overlap;
    DWORD bytes;
    ud_cxt ud;
    conn_cb_ctx conn_cb;
}overlap_conn_ctx;
typedef struct overlap_tcp_ctx
{
    sock_ctx ol_r;
    sock_ctx ol_s;
    DWORD bytes_r;
    DWORD bytes_s;
    DWORD flag;
    buffer_ctx buf_r;
    IOV_TYPE wsabuf;
    qu_bufs qubuf;
}overlap_tcp_ctx;

buffer_ctx *_get_recv_buf(sock_ctx *skctx)
{
    overlap_tcp_ctx *ol = UPCAST(skctx, overlap_tcp_ctx, ol_r);
    return &ol->buf_r;
}
qu_bufs *_get_send_buf(sock_ctx *skctx)
{
    overlap_tcp_ctx *ol = UPCAST(skctx, overlap_tcp_ctx, ol_r);
    return &ol->qubuf;
}
void _free_sockctx(sock_ctx *skctx)
{
    overlap_tcp_ctx *ol = UPCAST(skctx, overlap_tcp_ctx, ol_r);
    CLOSE_SOCK(ol->ol_r.fd);
    buffer_free(&ol->buf_r);
    _qu_bufs_clear(&ol->qubuf);
    qu_bufs_free(&ol->qubuf);
    FREE(ol);
}
static inline int32_t _join_iocp(ev_ctx *ev, SOCKET fd)
{
    if (NULL == CreateIoCompletionPort((HANDLE)fd, ev->watcher[0].iocp, 0, ev->nthreads))
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    return ERR_OK;
}
//recv
static inline void _on_recv_cb(watcher_ctx *watcher, sock_ctx *skctx)
{
    worker_canread(watcher->ev->worker, skctx->fd);
}
int32_t _post_recv(sock_ctx *skctx)
{
    overlap_tcp_ctx *ol = UPCAST(skctx, overlap_tcp_ctx, ol_r);
    ol->flag = ol->bytes_r = 0;
    ZERO(&ol->ol_r.overlapped, sizeof(ol->ol_r.overlapped));
    if (ERR_OK != WSARecv(ol->ol_r.fd,
        &ol->wsabuf,
        1,
        &ol->bytes_r,
        &ol->flag,
        &ol->ol_r.overlapped,
        NULL))
    {
        if (ERROR_IO_PENDING != ERRNO)
        {
            return ERR_FAILED;
        }
    }
    return ERR_OK;
}
//send
static inline void _on_send_cb(watcher_ctx *watcher, sock_ctx *skctx)
{
    worker_canwrite(watcher->ev->worker, skctx->fd);
}
int32_t _post_send(sock_ctx *skctx)
{
    overlap_tcp_ctx *ol = UPCAST(skctx, overlap_tcp_ctx, ol_r);
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
static inline sock_ctx *_new_sockctx(SOCKET fd)
{
    overlap_tcp_ctx *ol;
    MALLOC(ol, sizeof(overlap_tcp_ctx));
    ol->ol_r.fd = fd;
    ol->ol_r.ev_cb = _on_recv_cb;
    ol->ol_s.fd = fd;
    ol->ol_s.ev_cb = _on_send_cb;
    ol->wsabuf.IOV_PTR_FIELD = NULL;
    ol->wsabuf.IOV_LEN_FIELD = 0;
    buffer_init(&ol->buf_r);
    qu_bufs_init(&ol->qubuf, INIT_SENDBUF_LEN);
    return &ol->ol_r;
}
static inline int32_t _post_connect(overlap_conn_ctx *ol, netaddr_ctx *paddr)
{
    ol->bytes = 0;
    ZERO(&ol->overlap.overlapped, sizeof(ol->overlap.overlapped));
    if (!_exfuncs.connectex(ol->overlap.fd,
        netaddr_addr(paddr),
        netaddr_size(paddr),
        NULL,
        0,
        &ol->bytes,
        &ol->overlap.overlapped))
    {
        if (ERROR_IO_PENDING != ERRNO)
        {
            return ERR_FAILED;
        }
    }
    return ERR_OK;
}
static inline void _on_connect_cb(watcher_ctx *watcher, sock_ctx *skctx)
{
    overlap_conn_ctx *ol = UPCAST(skctx, overlap_conn_ctx, overlap);
    if (ERR_OK != sock_checkconn(ol->overlap.fd))
    {
        ol->conn_cb.conn_cb(watcher->ev, INVALID_SOCK, &ol->ud);
        CLOSE_SOCK(ol->overlap.fd);
        FREE(ol);
        return;
    }
    sock_ctx *rwctx = _new_sockctx(ol->overlap.fd);
    worker_add(watcher->ev->worker, ol->overlap.fd, rwctx, &ol->conn_cb.rw_cb, &ol->ud);
    if (ERR_OK != ol->conn_cb.conn_cb(watcher->ev, ol->overlap.fd, &ol->ud))
    {
        worker_remove(watcher->ev->worker, ol->overlap.fd);
    }
    else
    {
        _post_recv(rwctx);
    }
    FREE(ol);
}
int32_t ev_connect(ev_ctx *ctx, const char *host, const uint16_t port, 
    conn_cb_ctx *cbs, ud_cxt *ud)
{
    ASSERTAB(NULL != cbs, ERRSTR_NULLP);
    netaddr_ctx addr;
    if (ERR_OK != netaddr_sethost(&addr, host, port))
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    SOCKET sock = _create_sock(netaddr_family(&addr));
    if (INVALID_SOCK == sock)
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    sock_raddr(sock);
    _set_sockops(sock);
    if (ERR_OK != _trybind(sock, netaddr_family(&addr)))
    {
        CLOSE_SOCK(sock);
        return ERR_FAILED;
    }
    if (ERR_OK != _join_iocp(ctx, sock))
    {
        CLOSE_SOCK(sock);
        return ERR_FAILED;
    }
    overlap_conn_ctx *ol;
    MALLOC(ol, sizeof(overlap_conn_ctx));
    ol->overlap.fd = sock;
    ol->overlap.ev_cb = _on_connect_cb;
    ol->conn_cb = *cbs;
    COPY_UD(ol->ud, ud);
    if (ERR_OK != _post_connect(ol, &addr))
    {
        CLOSE_SOCK(sock);
        FREE(ol);
        return ERR_FAILED;
    }
    return ERR_OK;
}
//listen
static inline int32_t _post_accept(overlap_acpt_ctx *ol)
{
    SOCKET fd = _create_sock(ol->lsn->family);
    if (INVALID_SOCK == fd)
    {
        return ERRNO;
    }
    ol->bytes = 0;
    ol->overlap.fd = fd;
    ZERO(&ol->overlap.overlapped, sizeof(ol->overlap.overlapped));
    if (!_exfuncs.acceptex(ol->lsn->fd,//Listen Socket
        ol->overlap.fd,              //Accept Socket
        &ol->addr,
        0,
        sizeof(ol->addr) / 2,
        sizeof(ol->addr) / 2,
        &ol->bytes,
        &ol->overlap.overlapped))
    {
        if (ERROR_IO_PENDING != ERRNO)
        {
            CLOSE_SOCK(ol->overlap.fd);
            return ERR_FAILED;
        }
    }
    return ERR_OK;
}
static inline void _on_accept_cb(watcher_ctx *watcher, sock_ctx *skctx)
{
    overlap_acpt_ctx *acpol = UPCAST(skctx, overlap_acpt_ctx, overlap);
    SOCKET lsnfd = acpol->lsn->fd;
    SOCKET fd = acpol->overlap.fd;
    if (ERR_OK != _post_accept(acpol)
        || ERR_OK != setsockopt(fd, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (char *)&lsnfd, (int32_t)sizeof(lsnfd))
        || ERR_OK != _set_sockops(fd)
        || ERR_OK != _join_iocp(watcher->ev, fd))
    {
        CLOSE_SOCK(fd);
        return;
    }
    sock_ctx *rwctx = _new_sockctx(fd);
    worker_add(watcher->ev->worker, fd, rwctx, &acpol->lsn->lsn_cb.rw_cb, &acpol->lsn->ud);
    if (ERR_OK != acpol->lsn->lsn_cb.acp_cb(watcher->ev, fd, &acpol->lsn->ud))
    {
        worker_remove(watcher->ev->worker, fd);
        return;
    }
    _post_recv(rwctx);
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
    if (ERR_OK != _join_iocp(ev, lsn->fd))
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
int32_t ev_listen(ev_ctx *ctx, const char *host, const uint16_t port, 
    lsn_cb_ctx *cbs, freeud_cb fud_cb, ud_cxt *ud)
{
    ASSERTAB(NULL != cbs, ERRSTR_NULLP);
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
    lsn->lsn_cb = *cbs;
    lsn->fud_cb = fud_cb;
    COPY_UD(lsn->ud, ud);
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
    if (NULL != lsn->fud_cb)
    {
        lsn->fud_cb(&lsn->ud);
    }
    FREE(lsn);
}

#endif
