#include "event/iocp.h"
#include "event/evworker.h"

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
    SOCKET sock;
    accept_cb acp_cb;
    free_ud f_cb;
    ud_cxt ud;
    overlap_acpt_ctx overlap_acpt[MAX_ACCEPTEX_CNT];
}listener_ctx;
typedef struct overlap_conn_ctx
{
    sock_ctx overlap;
    DWORD bytes;
    ud_cxt ud;
    connect_cb conn_cb;
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
}overlap_tcp_ctx;

static inline int32_t _join_iocp(ev_ctx *ctx, SOCKET sock)
{
    if (NULL == CreateIoCompletionPort((HANDLE)sock, ctx->watcher[0].iocp, 0, ctx->nthreads))
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    return ERR_OK;
}
//listen
static inline int32_t _post_accept(overlap_acpt_ctx *acpol)
{
    SOCKET sock = _ev_sock(acpol->lsn->family);
    if (INVALID_SOCK == sock)
    {
        return ERRNO;
    }
    acpol->bytes = 0;
    acpol->overlap.sock = sock;
    ZERO(&acpol->overlap.overlapped, sizeof(acpol->overlap.overlapped));
    if (!_exfuncs.acceptex(acpol->lsn->sock,//Listen Socket
                           acpol->overlap.sock,              //Accept Socket
                           &acpol->addr,
                           0,
                           sizeof(acpol->addr) / 2,
                           sizeof(acpol->addr) / 2,
                           &acpol->bytes,
                           &acpol->overlap.overlapped))
    {
        if (ERROR_IO_PENDING != ERRNO)
        {
            CLOSE_SOCK(acpol->overlap.sock);
            return ERR_FAILED;
        }
    }
    return ERR_OK;
}
static void _on_accept_cb(watcher_ctx *watcher, DWORD bytes, sock_ctx *sock)
{
    overlap_acpt_ctx *acpol = UPCAST(sock, overlap_acpt_ctx, overlap);
    SOCKET fd = acpol->overlap.sock;
    if (ERR_OK != _post_accept(acpol))
    {
        CLOSE_SOCK(fd);
        return;
    }
    if (ERR_OK > setsockopt(fd, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
        (char *)&acpol->lsn->sock, sizeof(&acpol->lsn->sock)))
    {
        CLOSE_SOCK(fd);
        return;
    }
    if (ERR_OK != _join_iocp(watcher->ev, fd))
    {
        CLOSE_SOCK(fd);
        return;
    }
    ewcmd_accept(watcher->ev->worker, fd, acpol->lsn->acp_cb, &acpol->lsn->ud);
}
static void _free_acceptex(listener_ctx *lsn, int32_t cnt)
{
    for (int32_t j = 0; j < cnt; j++)
    {
        CLOSE_SOCK(lsn->overlap_acpt[j].overlap.sock);
    }
}
static int32_t _acceptex(ev_ctx *ctx, listener_ctx *lsn)
{
    if (ERR_OK != _join_iocp(ctx, lsn->sock))
    {
        return ERR_FAILED;
    }
    overlap_acpt_ctx *acpol;
    for (int32_t i = 0; i < MAX_ACCEPTEX_CNT; i++)
    {
        acpol = &lsn->overlap_acpt[i];
        acpol->lsn = lsn; 
        acpol->overlap.iocp = ctx->watcher[0].iocp;
        acpol->overlap.sock = INVALID_SOCK;
        acpol->overlap.ev_cb = _on_accept_cb;
        if (ERR_OK != _post_accept(acpol))
        {
            _free_acceptex(lsn, i);
            return ERR_FAILED;
        }
    }
    return ERR_OK;
}
int32_t ev_listener(ev_ctx *ctx, const char *host, const uint16_t port,
    accept_cb cb, free_ud f_cb, ud_cxt *ud)
{
    ASSERTAB(NULL != cb, ERRSTR_NULLP);
    netaddr_ctx addr;
    if (ERR_OK != netaddr_sethost(&addr, host, port))
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    SOCKET sock = _ev_listen(&addr);
    if (INVALID_SOCK == sock)
    {
        return ERR_FAILED;
    }
    listener_ctx *lsn;
    MALLOC(lsn, sizeof(listener_ctx));
    lsn->family = netaddr_family(&addr);
    lsn->sock = sock;
    lsn->acp_cb = cb;
    lsn->f_cb = f_cb;
    COPY_UD(lsn->ud, ud);
    if (ERR_OK != _acceptex(ctx, lsn))
    {
        CLOSE_SOCK(sock);
        FREE(lsn);
        return ERR_FAILED;
    }
    mutex_lock(&ctx->mulsn);
    qu_lsn_push(&ctx->qulsn, &lsn);
    mutex_unlock(&ctx->mulsn);
    return ERR_OK;
}
void _freelsn(listener_ctx *lsn)
{
    _free_acceptex(lsn, MAX_ACCEPTEX_CNT);
    CLOSE_SOCK(lsn->sock);
    if (NULL != lsn->f_cb)
    {
        lsn->f_cb(&lsn->ud);
    }
    FREE(lsn);
}
//connect
static int32_t _trybind(SOCKET sock, const int32_t family)
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
    if (ERR_OK != bind(sock, netaddr_addr(&addr), netaddr_size(&addr)))
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    return ERR_OK;
}
static int32_t _post_connect(overlap_conn_ctx *ol, netaddr_ctx *paddr)
{
    ol->bytes = 0;
    ZERO(&ol->overlap.overlapped, sizeof(ol->overlap.overlapped));
    if (!_exfuncs.connectex(ol->overlap.sock,
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
static void _on_connect_cb(watcher_ctx *watcher, DWORD bytes, sock_ctx *sock)
{
    overlap_conn_ctx *ol = UPCAST(sock, overlap_conn_ctx, overlap);
    ewcmd_connect(watcher->ev->worker, ol->overlap.sock, ol->conn_cb, &ol->ud);
    FREE(ol);
}
int32_t ev_connecter(ev_ctx *ctx, const char *host, const uint16_t port, connect_cb conn_cb, ud_cxt *ud)
{
    ASSERTAB(NULL != conn_cb, ERRSTR_NULLP);
    netaddr_ctx addr;
    if (ERR_OK != netaddr_sethost(&addr, host, port))
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    SOCKET sock = _ev_sock(netaddr_family(&addr));
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
    ol->overlap.iocp = ctx->watcher[0].iocp;
    ol->overlap.sock = sock;
    ol->overlap.ev_cb = _on_connect_cb;
    ol->conn_cb = conn_cb;
    COPY_UD(ol->ud, ud);
    if (ERR_OK != _post_connect(ol, &addr))
    {
        CLOSE_SOCK(sock);
        FREE(ol);
        return ERR_FAILED;
    }
    return ERR_OK;
}
//send
static void _on_send_cb(watcher_ctx *watcher, DWORD bytes, sock_ctx *sock)
{
    ewcmd_canwrite(watcher->ev->worker, sock->sock);
}
int32_t _post_send(sock_ctx *sock)
{
    overlap_tcp_ctx *ol = UPCAST(sock, overlap_tcp_ctx, ol_r);
    ol->bytes_s = 0;
    ZERO(&ol->ol_s.overlapped, sizeof(ol->ol_s.overlapped));
    if (ERR_OK != WSASend(ol->ol_s.sock, &ol->wsabuf, 1, &ol->bytes_s, 0, &ol->ol_s.overlapped, NULL))
    {
        if (ERROR_IO_PENDING != ERRNO)
        {
            return ERR_FAILED;
        }
    }
    return ERR_OK;
}
int32_t _post_recv(sock_ctx *sock)
{
    overlap_tcp_ctx *ol = UPCAST(sock, overlap_tcp_ctx, ol_r);
    ol->flag = ol->bytes_r = 0;
    ZERO(&ol->ol_r.overlapped, sizeof(ol->ol_r.overlapped));
    if (ERR_OK != WSARecv(ol->ol_r.sock,
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
static void _on_recv_cb(watcher_ctx *watcher, DWORD bytes, sock_ctx *sock)
{
    ewcmd_canread(watcher->ev->worker, sock->sock);
}
struct sock_ctx *_new_sockctx(ev_ctx *ctx, SOCKET sock)
{
    overlap_tcp_ctx *ol;
    MALLOC(ol, sizeof(overlap_tcp_ctx));
    ol->ol_r.iocp = ctx->watcher[0].iocp;
    ol->ol_r.sock = sock;
    ol->ol_r.ev_cb = _on_recv_cb;
    ol->ol_s.iocp = ctx->watcher[0].iocp;
    ol->ol_s.sock = sock;
    ol->ol_s.ev_cb = _on_send_cb;
    ol->wsabuf.IOV_PTR_FIELD = NULL;
    ol->wsabuf.IOV_LEN_FIELD = 0;
    buffer_init(&ol->buf_r);
    return &ol->ol_r;
}
void _free_sockctx(sock_ctx *sock)
{
    overlap_tcp_ctx *ol = UPCAST(sock, overlap_tcp_ctx, ol_r);
    (void)shutdown(ol->ol_r.sock, SHUT_RD);
    CLOSE_SOCK(ol->ol_r.sock);
    buffer_free(&ol->buf_r);
    FREE(ol);
}
void _invalid_sockctx(sock_ctx *sock)
{
    overlap_tcp_ctx *ol = UPCAST(sock, overlap_tcp_ctx, ol_r);
    ol->ol_r.sock = INVALID_SOCK;
    ol->ol_s.sock = INVALID_SOCK;
}
buffer_ctx *_get_buffer_r(sock_ctx *sock)
{
    overlap_tcp_ctx *ol = UPCAST(sock, overlap_tcp_ctx, ol_r);
    return &ol->buf_r;
}

#endif//EV_IOCP
