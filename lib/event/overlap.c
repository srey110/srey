#include "event/iocp.h"

#ifdef EV_IOCP

#define MAX_ACCEPTEX_CNT        128
#define MAX_RECV_IOV_SIZE       ONEK
#define MAX_RECV_IOV_COUNT      4

typedef struct overlap_acpt_ctx
{
    sock_ctx overlap;
    struct listener_ctx *listener;
    DWORD bytes;
    char addrbuf[sizeof(struct sockaddr_storage)];
}overlap_acpt_ctx;
typedef struct listener_ctx
{
    int32_t family;
    SOCKET  sock;
    accept_cb acp_cb;
    void *ud;
    overlap_acpt_ctx overlap_acpt[MAX_ACCEPTEX_CNT];
}listener_ctx;
typedef struct overlap_send_ctx
{
    sock_ctx overlap;
    DWORD bytes;
    void *ud;
    send_cb sendcb;
    WSABUF wsabuf[1];
}overlap_send_ctx;
typedef struct overlap_recv_ctx
{
    sock_ctx overlap;
    int32_t iovcnt;
    DWORD bytes;
    DWORD flag;
    recv_cb r_cb;
    close_cb c_cb;
    void *ud;
    struct buffer_ctx buf;
    IOV_TYPE wsabuf[MAX_RECV_IOV_COUNT];
}overlap_recv_ctx;

static void _on_send_cb(ev_ctx *ctx, int32_t err, DWORD bytes, sock_ctx *sock)
{
    overlap_send_ctx *ol = UPCAST(sock, overlap_send_ctx, overlap);
    if (NULL != ol->sendcb)
    {
        ol->sendcb(ol->overlap.sock, ol->wsabuf->buf, ol->wsabuf->len, ol->ud, err);
    }
    FREE(ol->wsabuf->buf);
    FREE(ol);
}
int32_t _iocp_send(ev_ctx *ctx, SOCKET sock, send_cb cb, void *data, size_t len, void *ud)
{
    overlap_send_ctx *ol;
    MALLOC(ol, sizeof(overlap_send_ctx));
    ZERO(ol, sizeof(overlap_send_ctx));
    ol->overlap.sock = sock;
    ol->overlap.ev_cb = _on_send_cb;
    ol->ud = ud;
    ol->sendcb = cb;
    ol->wsabuf->buf = data;
    ol->wsabuf->len = (ULONG)len;

    int32_t rtn = WSASend(sock,
        ol->wsabuf,
        1,
        &ol->bytes,
        0,
        &ol->overlap.overlapped,
        NULL);
    if (ERR_OK != rtn)
    {
        rtn = ERRNO;
        if (WSA_IO_PENDING != rtn)
        {
            FREE(ol);
            return rtn;
        }
    }
    return ERR_OK;
}
static int32_t _post_accept(ev_ctx *ctx, overlap_acpt_ctx *acpol)
{
    SOCKET sock = ev_sock(acpol->listener->family);
    if (INVALID_SOCK == sock)
    {
        return ERRNO;
    }
    ZERO(&acpol->overlap.overlapped, sizeof(acpol->overlap.overlapped));
    acpol->bytes = 0;
    acpol->overlap.sock = sock;
    if (!ctx->acceptex(acpol->listener->sock,//Listen Socket
            acpol->overlap.sock,              //Accept Socket
            &acpol->addrbuf,
            0,
            sizeof(acpol->addrbuf) / 2,
            sizeof(acpol->addrbuf) / 2,
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
        (char *)&acpol->listener->sock, sizeof(&acpol->listener->sock));
    if (rtn < ERR_OK)
    {
        rtn = ERRNO;
        CLOSE_SOCK(fd);
        return;
    }
    if (NULL == CreateIoCompletionPort((HANDLE)fd, ctx->iocp, 0, ctx->nthreads))
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        CLOSE_SOCK(fd);
        return;
    }
    acpol->listener->acp_cb(fd, acpol->listener->ud);
}
static int32_t _acceptex(ev_ctx *ctx, listener_ctx *lsn)
{
    if (NULL == CreateIoCompletionPort((HANDLE)lsn->sock, ctx->iocp, 0, ctx->nthreads))
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    for (int32_t i = 0; i < MAX_ACCEPTEX_CNT; i++)
    {
        overlap_acpt_ctx *acpol = &lsn->overlap_acpt[i];
        acpol->overlap.sock = INVALID_SOCK;
        acpol->overlap.ev_cb = _on_accept_cb;
        acpol->listener = lsn;
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
    int32_t rtn = netaddr_sethost(&addr, host, port);
    if (ERR_OK != rtn)
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    SOCKET sock = ev_listen(&addr);
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
    qu_lsn_push(&ctx->qulsn, &lsn);
    return ERR_OK;
}
void _iocp_freelsn(listener_ctx *lsn)
{
    for (int32_t i = 0; i < MAX_ACCEPTEX_CNT; i++)
    {
        overlap_acpt_ctx *acpol = &lsn->overlap_acpt[i];
        CLOSE_SOCK(acpol->overlap.sock);
    }
    CLOSE_SOCK(lsn->sock);
    FREE(lsn);
}
static int32_t _post_recv(overlap_recv_ctx *ol)
{
    ZERO(&ol->overlap.overlapped, sizeof(ol->overlap.overlapped));
    ol->flag = ol->bytes = 0;
    ol->iovcnt = buffer_write_iov_application(&ol->buf, MAX_RECV_IOV_SIZE, ol->wsabuf, MAX_RECV_IOV_COUNT);
    int32_t rtn = WSARecv(ol->overlap.sock,
        ol->wsabuf,
        (DWORD)ol->iovcnt,
        &ol->bytes,
        &ol->flag,
        &ol->overlap.overlapped,
        NULL);
    if (ERR_OK != rtn)
    {
        rtn = ERRNO;
        if (WSA_IO_PENDING != rtn)
        {
            buffer_write_iov_commit(&ol->buf, 0, ol->wsabuf, ol->iovcnt);
            return rtn;
        }
    }
    return ERR_OK;
}
static void _free_recvol(overlap_recv_ctx *ol)
{
    CLOSE_SOCK(ol->overlap.sock);
    buffer_free(&ol->buf);
    FREE(ol);
}
static void _on_close(ev_ctx *ctx, overlap_recv_ctx *ol)
{
    ol->c_cb(ol->overlap.sock, ol->ud);
    _sender_remove(ctx, ol->overlap.sock);
    _free_recvol(ol);
}
static void _on_recv_cb(ev_ctx *ctx, int32_t err, DWORD bytes, sock_ctx *sock)
{
    overlap_recv_ctx *ol = UPCAST(sock, overlap_recv_ctx, overlap);
    if (0 == bytes
        || ERR_OK != err)
    {
        buffer_write_iov_commit(&ol->buf, 0, ol->wsabuf, ol->iovcnt);
        _on_close(ctx, ol);
        return;
    }
    buffer_write_iov_commit(&ol->buf, (size_t)bytes, ol->wsabuf, ol->iovcnt);
    ol->r_cb(ol->overlap.sock, &ol->buf, ol->ud);
    if (ERR_OK != _post_recv(ol))
    {
        _on_close(ctx, ol);
        return;
    }
} 
int32_t ev_loop(ev_ctx *ctx, SOCKET sock, recv_cb r_cb, close_cb c_cb, send_cb s_cb, void *ud)
{
    ASSERTAB(NULL != r_cb && NULL != c_cb, ERRSTR_NULLP);
    overlap_recv_ctx *ol;
    MALLOC(ol, sizeof(overlap_recv_ctx));
    ol->overlap.sock = sock;
    ol->overlap.ev_cb = _on_recv_cb;
    ol->r_cb = r_cb;
    ol->c_cb = c_cb;
    ol->ud = ud;
    sock_linger(sock);
    sock_nodelay(sock);
    sock_kpa(sock, SOCKKPA_DELAY, SOCKKPA_INTVL);
    sock_nbio(sock);
    buffer_init(&ol->buf);
    if (ERR_OK != _post_recv(ol))
    {
        CLOSE_SOCK(sock);
        buffer_free(&ol->buf);
        FREE(ol);
        return ERR_FAILED;
    }
    _sender_add(ctx, sock, s_cb);
    return ERR_OK;
}

#endif//EV_IOCP
