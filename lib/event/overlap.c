#include "event/iocp.h"

#ifdef EV_IOCP

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
    void *ud;
    overlap_acpt_ctx overlap_acpt[MAX_ACCEPTEX_CNT];
}listener_ctx;
typedef struct overlap_conn_ctx
{
    sock_ctx overlap;
    DWORD bytes;
    void *ud;
    connect_cb conn_cb;
}overlap_conn_ctx;
typedef struct overlap_disconn_ctx
{
    sock_ctx overlap;
}overlap_disconn_ctx;
typedef struct overlap_send_ctx
{
    sock_ctx overlap;
    DWORD bytes;
    void *ud;
    send_cb s_cb;
    mutex_ctx oplck;
    WSABUF wsabuf;
}overlap_send_ctx;
typedef struct overlap_recv_ctx
{
    sock_ctx overlap;
    int32_t niov;
    DWORD bytes;
    DWORD flag;
    recv_cb r_cb;
    close_cb c_cb;
    void *ud;
    buffer_ctx buf;
    mutex_ctx oplck;
    IOV_TYPE wsabuf[MAX_RECV_IOV_COUNT];
}overlap_recv_ctx;

static void _on_send_cb(ev_ctx *ctx, int32_t err, DWORD bytes, sock_ctx *sock)
{
    overlap_send_ctx *ol = UPCAST(sock, overlap_send_ctx, overlap);
    if (NULL != ol->s_cb)
    {
        ol->s_cb(ctx, ol->overlap.sock, ol->wsabuf.buf, ol->wsabuf.len, ol->ud, err);
    }

    mutex_lock(&ol->oplck);    
    mutex_unlock(&ol->oplck);

    mutex_free(&ol->oplck);
    FREE(ol->wsabuf.buf);
    FREE(ol);
}
int32_t _post_send(ev_ctx *ctx, SOCKET sock, send_cb cb, void *data, size_t len, void *ud)
{
    overlap_send_ctx *ol;
    MALLOC(ol, sizeof(overlap_send_ctx));
    ZERO(&ol->overlap.overlapped, sizeof(ol->overlap.overlapped));
    ol->overlap.sock = sock;
    ol->overlap.ev_cb = _on_send_cb;
    ol->ud = ud;
    ol->s_cb = cb;
    ol->bytes = 0;
    ol->wsabuf.buf = data;
    ol->wsabuf.len = (ULONG)len;
    mutex_init(&ol->oplck);

    mutex_lock(&ol->oplck);
    int32_t rtn = WSASend(sock, &ol->wsabuf, 1, &ol->bytes, 0, &ol->overlap.overlapped, NULL);
    mutex_unlock(&ol->oplck);
    if (ERR_OK != rtn)
    {
        rtn = ERRNO;
        if (WSA_IO_PENDING != rtn)
        {
            mutex_free(&ol->oplck);
            FREE(ol);
            return rtn;
        }
    }
    return ERR_OK;
}
static int32_t _post_accept(ev_ctx *ctx, overlap_acpt_ctx *acpol)
{
    SOCKET sock = _ev_sock(acpol->lsn->family);
    if (INVALID_SOCK == sock)
    {
        return ERRNO;
    }
    ZERO(&acpol->overlap.overlapped, sizeof(acpol->overlap.overlapped));
    acpol->bytes = 0;
    acpol->overlap.sock = sock;
    if (!_exfuncs.acceptex(acpol->lsn->sock,//Listen Socket
            acpol->overlap.sock,              //Accept Socket
            &acpol->addr,
            0,
            sizeof(acpol->addr) / 2,
            sizeof(acpol->addr) / 2,
            &acpol->bytes,
            &acpol->overlap.overlapped))
    {
        int32_t rtn = ERRNO;
        if (ERROR_IO_PENDING != rtn)
        {
            CLOSE_SOCK(acpol->overlap.sock);
            return rtn;
        }
    }
    return ERR_OK;
}
static int32_t _add_iocp(ev_ctx *ctx, SOCKET sock)
{
    if (NULL == CreateIoCompletionPort((HANDLE)sock, ctx->iocp, 0, ctx->nthreads))
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    /*UCHAR flags = FILE_SKIP_SET_EVENT_ON_HANDLE | FILE_SKIP_COMPLETION_PORT_ON_SUCCESS;
    if (!SetFileCompletionNotificationModes((HANDLE)sock, flags))
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }*/
    return ERR_OK;
}
static void _on_accept_cb(ev_ctx *ctx, int32_t err, DWORD bytes, sock_ctx *sock)
{
    overlap_acpt_ctx *acpol = UPCAST(sock, overlap_acpt_ctx, overlap);
    SOCKET fd = acpol->overlap.sock;
    int32_t rtn = _post_accept(ctx, acpol);
    if (ERR_OK != rtn
        || ERR_OK != err)
    {
        CLOSE_SOCK(fd);
        return;
    }
    rtn = setsockopt(fd, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
        (char *)&acpol->lsn->sock, sizeof(&acpol->lsn->sock));
    if (rtn < ERR_OK)
    {
        rtn = ERRNO;
        CLOSE_SOCK(fd);
        return;
    }
    if (ERR_OK != _add_iocp(ctx, fd))
    {
        CLOSE_SOCK(fd);
        return;
    }
    acpol->lsn->acp_cb(ctx, fd, acpol->lsn->ud);
}
static int32_t _acceptex(ev_ctx *ctx, listener_ctx *lsn)
{
    if (ERR_OK != _add_iocp(ctx, lsn->sock))
    {
        return ERR_FAILED;
    }
    for (int32_t i = 0; i < MAX_ACCEPTEX_CNT; i++)
    {
        overlap_acpt_ctx *acpol = &lsn->overlap_acpt[i];
        acpol->overlap.sock = INVALID_SOCK;
        acpol->overlap.ev_cb = _on_accept_cb;
        acpol->lsn = lsn;
        if (ERR_OK != _post_accept(ctx, acpol))
        {
            for (int32_t j = 0; j < i; j++)
            {
                acpol = &lsn->overlap_acpt[j];
                CLOSE_SOCK(acpol->overlap.sock);
            }
            return ERR_FAILED;
        }
    }
    return ERR_OK;
}
int32_t ev_listener(ev_ctx *ctx, const char *host, const uint16_t port, accept_cb cb, void *ud)
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
    lsn->ud = ud;
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
    for (int32_t i = 0; i < MAX_ACCEPTEX_CNT; i++)
    {
        CLOSE_SOCK(lsn->overlap_acpt[i].overlap.sock);
    }
    CLOSE_SOCK(lsn->sock);
    FREE(lsn);
}
static int32_t _post_recv(overlap_recv_ctx *ol)
{
    ZERO(&ol->overlap.overlapped, sizeof(ol->overlap.overlapped));
    ol->flag = ol->bytes = 0;
    ol->niov = buffer_write_iov_application(&ol->buf, MAX_RECV_IOV_SIZE, ol->wsabuf, MAX_RECV_IOV_COUNT);

    mutex_lock(&ol->oplck);
    int32_t rtn = WSARecv(ol->overlap.sock,
        ol->wsabuf,
        (DWORD)ol->niov,
        &ol->bytes,
        &ol->flag,
        &ol->overlap.overlapped,
        NULL);
    mutex_unlock(&ol->oplck);
    if (ERR_OK != rtn)
    {
        rtn = ERRNO;
        if (WSA_IO_PENDING != rtn)
        {
            return rtn;
        }
    }
    return ERR_OK;
}
static void _free_recvol(overlap_recv_ctx *ol)
{
    mutex_free(&ol->oplck);
    buffer_free(&ol->buf);
    FREE(ol);
}
static void _on_close(ev_ctx *ctx, overlap_recv_ctx *ol)
{
    if (NULL != ol->c_cb)
    {
        ol->c_cb(ctx, ol->overlap.sock, ol->ud);
    }
    _cmd_remove(ctx, ol->overlap.sock);
    mutex_lock(&ol->oplck);
    mutex_unlock(&ol->oplck);
    _free_recvol(ol);
}
static void _on_recv_cb(ev_ctx *ctx, int32_t err, DWORD bytes, sock_ctx *sock)
{
    overlap_recv_ctx *ol = UPCAST(sock, overlap_recv_ctx, overlap);
    if (0 == bytes
        || ERR_OK != err)
    {
        _on_close(ctx, ol);
        return;
    }
    buffer_write_iov_commit(&ol->buf, (size_t)bytes, ol->wsabuf, ol->niov);
    ol->r_cb(ctx, ol->overlap.sock, &ol->buf, ol->ud);
    if (ERR_OK != _post_recv(ol))
    {
        _on_close(ctx, ol);
        return;
    }
} 
int32_t ev_loop(ev_ctx *ctx, SOCKET sock, recv_cb r_cb, close_cb c_cb, send_cb s_cb, void *ud)
{
    ASSERTAB(NULL != r_cb, ERRSTR_NULLP);
    overlap_recv_ctx *ol;
    MALLOC(ol, sizeof(overlap_recv_ctx));
    ol->overlap.sock = sock;
    ol->overlap.ev_cb = _on_recv_cb;
    ol->r_cb = r_cb;
    ol->c_cb = c_cb;
    ol->ud = ud;
    buffer_init(&ol->buf);
    mutex_init(&ol->oplck);

    sock_linger(sock);
    sock_nodelay(sock);
    sock_kpa(sock, SOCKKPA_DELAY, SOCKKPA_INTVL);
    sock_nbio(sock);
    
    _cmd_add(ctx, sock, s_cb, ud);
    if (ERR_OK != _post_recv(ol))
    {
        _cmd_remove(ctx, sock);
        _free_recvol(ol);
        return ERR_FAILED;
    }
    return ERR_OK;
}
static void _on_disconnex_cb(ev_ctx *ctx, int32_t err, DWORD bytes, sock_ctx *sock)
{
    overlap_disconn_ctx *ol = UPCAST(sock, overlap_disconn_ctx, overlap);
    FREE(ol);
}
void _post_disconn(ev_ctx *ctx, SOCKET sock)
{
    overlap_disconn_ctx *ol;
    MALLOC(ol, sizeof(overlap_disconn_ctx));
    ZERO(&ol->overlap.overlapped, sizeof(ol->overlap.overlapped));
    ol->overlap.sock = sock;
    ol->overlap.ev_cb = _on_disconnex_cb;
    BOOL rtn = _exfuncs.disconnectex(sock, &ol->overlap.overlapped, 0, 0);
    if (!rtn)
    {
        int32_t err = ERRNO;
        if (ERROR_IO_PENDING != err)
        {
            CLOSE_SOCK(sock);
            FREE(ol);
            return;
        }
    }
}
static int32_t _connectex(ev_ctx *ctx, overlap_conn_ctx *ol, netaddr_ctx *paddr)
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
        int32_t rtn = ERRNO;
        if (ERROR_IO_PENDING != rtn)
        {
            return rtn;
        }
    }
    return ERR_OK;
}
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
static void _on_connect_cb(ev_ctx *ctx, int32_t err, DWORD bytes, sock_ctx *sock)
{
    overlap_conn_ctx *ol = UPCAST(sock, overlap_conn_ctx, overlap);
    ol->conn_cb(ctx, err, ol->overlap.sock, ol->ud);
    if (ERR_OK != err)
    {
        CLOSE_SOCK(ol->overlap.sock);
    }
    FREE(ol);
}
int32_t ev_connecter(ev_ctx *ctx, const char *host, const uint16_t port, connect_cb conn_cb, void *ud)
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
    if (ERR_OK != _trybind(sock, netaddr_family(&addr)))
    {
        CLOSE_SOCK(sock);
        return ERR_FAILED;
    }
    if (ERR_OK != _add_iocp(ctx, sock))
    {
        CLOSE_SOCK(sock);
        return ERR_FAILED;
    }
    overlap_conn_ctx *ol;
    MALLOC(ol, sizeof(overlap_conn_ctx));
    ol->overlap.sock = sock;
    ol->overlap.ev_cb = _on_connect_cb;
    ol->conn_cb = conn_cb;
    ol->ud = ud;
    if (ERR_OK != _connectex(ctx, ol, &addr))
    {
        CLOSE_SOCK(sock);
        FREE(ol);
        return ERR_FAILED;
    }
    return ERR_OK;
}

#endif//EV_IOCP
