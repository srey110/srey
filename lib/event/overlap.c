#include "event/iocp.h"

#ifdef EV_IOCP

#define NOTIFY_MODE_SEND    1
#define NOTIFY_MODE_RECV    1
#define NOTIFY_MODE_DISCONN 1
#define NOTIFY_MODE_CONN    1
#define NOTIFY_MODE_LISTEN  0

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
typedef struct overlap_send_ctx
{
    sock_ctx overlap;
    DWORD bytes;
    void *ud;
    send_cb s_cb;
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
    IOV_TYPE wsabuf[MAX_RECV_IOV_COUNT];
}overlap_recv_ctx;

static inline int32_t _join_iocp(ev_ctx *ctx, SOCKET sock)
{
    if (NULL == CreateIoCompletionPort((HANDLE)sock, ctx->iocp, 0, ctx->nthreads))
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    if (!SetFileCompletionNotificationModes((HANDLE)sock, 
        FILE_SKIP_SET_EVENT_ON_HANDLE | FILE_SKIP_COMPLETION_PORT_ON_SUCCESS))
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    return ERR_OK;
}
static inline int32_t _init_sockctx(sock_ctx *ctx, SOCKET fd, ev_ctx *ev, event_cb cb, int32_t notifymode)
{
    ctx->ev = ev;
    ctx->sock = fd;
    ctx->ev_cb = cb;
    ctx->wait_handle = INVALID_HANDLE_VALUE;
    if (!notifymode)
    {
        ctx->ev_handle = NULL;
        return ERR_OK;
    }
    ctx->ev_handle = CreateEvent(NULL, 0, 0, NULL);
    if (NULL == ctx->ev_handle)
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    return ERR_OK;
}
static inline void _init_overlap(sock_ctx *sock, int32_t notifymode)
{
    ZERO(&sock->overlapped, sizeof(sock->overlapped));
    if (notifymode)
    {
        sock->overlapped.hEvent = (HANDLE)((ULONG_PTR)sock->ev_handle | 1);
    }    
}
static void CALLBACK _post_completion(void* arg, BOOLEAN timeout)
{
    sock_ctx *sock = (sock_ctx *)arg;
    if (!PostQueuedCompletionStatus(sock->ev->iocp,
        (DWORD)sock->overlapped.InternalHigh,
        0,
        &sock->overlapped))
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
    }
}
static inline int32_t _register_event(sock_ctx *sock)
{
    if (INVALID_HANDLE_VALUE != sock->wait_handle
        || NULL == sock->ev_handle)
    {
        return ERR_OK;
    }
    if (!RegisterWaitForSingleObject(&sock->wait_handle, sock->ev_handle,
        _post_completion, (void*)sock, INFINITE, WT_EXECUTEINWAITTHREAD))
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    return ERR_OK;
}
static inline void _unregister_waitevent(sock_ctx *sock)
{
    if (INVALID_HANDLE_VALUE != sock->wait_handle)
    {
        if (!UnregisterWaitEx(sock->wait_handle, INVALID_HANDLE_VALUE))
        {
            LOG_ERROR("%s", ERRORSTR(ERRNO));
        }
        sock->wait_handle = INVALID_HANDLE_VALUE;
    }
}
static inline void _unregister_event(sock_ctx *sock)
{
    _unregister_waitevent(sock);
    if (NULL != sock->ev_handle)
    {
        if (!CloseHandle(sock->ev_handle))
        {
            LOG_ERROR("%s", ERRORSTR(ERRNO));
        }
        sock->ev_handle = NULL;
    }
}
static inline void _set_sockop(SOCKET sock)
{
    sock_linger(sock);
    sock_nodelay(sock);
    sock_kpa(sock, SOCKKPA_DELAY, SOCKKPA_INTVL);
    sock_nbio(sock);
}
//send
static void _on_send_cb(ev_ctx *ctx, int32_t err, DWORD bytes, sock_ctx *sock)
{
    overlap_send_ctx *ol = UPCAST(sock, overlap_send_ctx, overlap);
    if (NULL != ol->s_cb)
    {
        ol->s_cb(ctx, ol->overlap.sock, (size_t)bytes, ol->ud, err);
    }
    _unregister_event(&ol->overlap);
    FREE(ol);
}
int32_t _post_send(ev_ctx *ctx, SOCKET sock, send_cb cb, void *data, size_t len, void *ud)
{
    overlap_send_ctx *ol = (overlap_send_ctx *)data;
    if (ERR_OK != _init_sockctx(&ol->overlap, sock, ctx, _on_send_cb, NOTIFY_MODE_SEND))
    {
        return ERR_FAILED;
    }
    ol->ud = ud;
    ol->s_cb = cb;
    ol->bytes = 0;
    ol->wsabuf.buf = (char *)data + sizeof(overlap_send_ctx);
    ol->wsabuf.len = (ULONG)len;
    _init_overlap(&ol->overlap, NOTIFY_MODE_SEND);
    if (ERR_OK != WSASend(sock, &ol->wsabuf, 1, &ol->bytes, 0, &ol->overlap.overlapped, NULL))
    {
        if (WSA_IO_PENDING != ERRNO)
        {
            _unregister_event(&ol->overlap);
            return ERR_FAILED;
        }
    }
    if (ERR_OK != _register_event(&ol->overlap))
    {
        _unregister_event(&ol->overlap);
        return ERR_FAILED;
    }
    return ERR_OK;
}
void ev_send(ev_ctx *ctx, SOCKET sock, void *data, size_t len)
{
    if (INVALID_SOCK == sock
        || NULL == data)
    {
        return;
    }
    cmd_ctx cmd;
    cmd.cmd = CMD_SEND;
    cmd.sock = sock;
    cmd.len = len;
    MALLOC(cmd.data, (sizeof(overlap_send_ctx) + len));
    memcpy(((char*)cmd.data + sizeof(overlap_send_ctx)), data, len);
    _send_cmd(_get_watcher(ctx, sock), &cmd);
}
//listen
static inline int32_t _post_accept(ev_ctx *ctx, overlap_acpt_ctx *acpol)
{
    SOCKET sock = _ev_sock(acpol->lsn->family);
    if (INVALID_SOCK == sock)
    {
        return ERRNO;
    }
    acpol->bytes = 0;
    acpol->overlap.sock = sock;
    _init_overlap(&acpol->overlap, NOTIFY_MODE_LISTEN);
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
    if (ERR_OK != _register_event(&acpol->overlap))
    {
        CLOSE_SOCK(acpol->overlap.sock);
        return ERR_FAILED;
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
        (char *)&acpol->lsn->sock, sizeof(&acpol->lsn->sock));
    if (rtn < ERR_OK)
    {
        rtn = ERRNO;
        CLOSE_SOCK(fd);
        return;
    }
    _set_sockop(fd);
    if (ERR_OK != _join_iocp(ctx, fd))
    {
        CLOSE_SOCK(fd);
        return;
    }
    acpol->lsn->acp_cb(ctx, fd, acpol->lsn->ud);
}
static void _free_acceptex(listener_ctx *lsn, int32_t cnt)
{
    for (int32_t j = 0; j < cnt; j++)
    {
        CLOSE_SOCK(lsn->overlap_acpt[j].overlap.sock);
        _unregister_event(&lsn->overlap_acpt[j].overlap);
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
        if (ERR_OK != _init_sockctx(&acpol->overlap, INVALID_SOCK, ctx, _on_accept_cb, NOTIFY_MODE_LISTEN))
        {
            _free_acceptex(lsn, i);
            return ERR_FAILED;
        }
        if (ERR_OK != _post_accept(ctx, acpol))
        {
            _unregister_event(&acpol->overlap);
            _free_acceptex(lsn, i);
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
    _free_acceptex(lsn, MAX_ACCEPTEX_CNT);
    CLOSE_SOCK(lsn->sock);
    FREE(lsn);
}
//recv
static inline void _free_recvol(overlap_recv_ctx *ol)
{
    _cmd_remove(ol->overlap.ev, ol->overlap.sock);
    _unregister_event(&ol->overlap);
    buffer_free(&ol->buf);
    FREE(ol);
}
static inline void _on_close(ev_ctx *ctx, overlap_recv_ctx *ol)
{
    if (NULL != ol->c_cb)
    {
        ol->c_cb(ctx, ol->overlap.sock, ol->ud);
    }
    _free_recvol(ol);
}
static inline int32_t _post_recv(overlap_recv_ctx *ol)
{
    ol->flag = ol->bytes = 0;
    ol->niov = buffer_write_iov_application(&ol->buf, MAX_RECV_IOV_SIZE, ol->wsabuf, MAX_RECV_IOV_COUNT);
    _unregister_waitevent(&ol->overlap);
    _init_overlap(&ol->overlap, NOTIFY_MODE_RECV);
    if (ERR_OK != WSARecv(ol->overlap.sock,
                          ol->wsabuf,
                          (DWORD)ol->niov,
                          &ol->bytes,
                          &ol->flag,
                          &ol->overlap.overlapped,
                          NULL))
    {
        if (WSA_IO_PENDING != ERRNO)
        {
            return ERR_FAILED;
        }
    }
    return _register_event(&ol->overlap);
}
static void _on_recv_cb(ev_ctx *ctx, int32_t err, DWORD bytes, sock_ctx *sock)
{
    overlap_recv_ctx *ol = UPCAST(sock, overlap_recv_ctx, overlap);
    int32_t rtn = ERR_FAILED;
    if (0 != bytes
        && ERR_OK == err)
    {
        buffer_write_iov_commit(&ol->buf, (size_t)bytes, ol->wsabuf, ol->niov);
        ol->r_cb(ctx, ol->overlap.sock, &ol->buf, ol->ud);
        rtn = _post_recv(ol);
    }
    if (ERR_OK != rtn)
    {
        _on_close(ctx, ol);
    }
}
int32_t ev_loop(ev_ctx *ctx, SOCKET sock, recv_cb r_cb, close_cb c_cb, send_cb s_cb, void *ud)
{
    ASSERTAB(NULL != r_cb, ERRSTR_NULLP);
    overlap_recv_ctx *ol;
    MALLOC(ol, sizeof(overlap_recv_ctx));
    if (ERR_OK != _init_sockctx(&ol->overlap, sock, ctx, _on_recv_cb, NOTIFY_MODE_RECV))
    {
        FREE(ol);
        return ERR_FAILED;
    }
    ol->r_cb = r_cb;
    ol->c_cb = c_cb;
    ol->ud = ud;
    buffer_init(&ol->buf);
    _cmd_add(ctx, sock, s_cb, ud);
    if (ERR_OK != _post_recv(ol))
    {
        _free_recvol(ol);
        return ERR_FAILED;
    }
    return ERR_OK;
}
//disconnect
static void _on_disconnex_cb(ev_ctx *ctx, int32_t err, DWORD bytes, sock_ctx *sock)
{
    _unregister_event(sock);
    FREE(sock);
}
void _post_disconn(ev_ctx *ctx, SOCKET sock)
{
    sock_ctx *ol;
    MALLOC(ol, sizeof(sock_ctx));
    if (ERR_OK != _init_sockctx(ol, sock, ctx, _on_disconnex_cb, NOTIFY_MODE_DISCONN))
    {
        CLOSE_SOCK(sock);
        FREE(ol);
        return;
    }
    shutdown(sock, SHUT_RD);
    _init_overlap(ol, NOTIFY_MODE_DISCONN);
    if (!_exfuncs.disconnectex(sock, &ol->overlapped, 0, 0))
    {
        if (ERROR_IO_PENDING != ERRNO)
        {
            CLOSE_SOCK(sock);
            _unregister_event(ol);
            FREE(ol);
            return;
        }
    }
    if (ERR_OK != _register_event(ol))
    {
        CLOSE_SOCK(sock);
        _unregister_event(ol);
        FREE(ol);
    }
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
static int32_t _post_connect(ev_ctx *ctx, overlap_conn_ctx *ol, netaddr_ctx *paddr)
{
    ol->bytes = 0;
    _init_overlap(&ol->overlap, NOTIFY_MODE_CONN);
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
    return _register_event(&ol->overlap);
}
static void _on_connect_cb(ev_ctx *ctx, int32_t err, DWORD bytes, sock_ctx *sock)
{
    overlap_conn_ctx *ol = UPCAST(sock, overlap_conn_ctx, overlap);
    ol->conn_cb(ctx, err, ol->overlap.sock, ol->ud);
    if (ERR_OK != err)
    {
        CLOSE_SOCK(ol->overlap.sock);
    }
    _unregister_event(sock);
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
    sock_raddr(sock);
    _set_sockop(sock);
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
    if (ERR_OK != _init_sockctx(&ol->overlap, sock, ctx, _on_connect_cb, NOTIFY_MODE_CONN))
    {
        CLOSE_SOCK(sock);
        FREE(ol);
        return ERR_FAILED;
    }
    ol->conn_cb = conn_cb;
    ol->ud = ud;
    if (ERR_OK != _post_connect(ctx, ol, &addr))
    {
        CLOSE_SOCK(sock);
        _unregister_event(&ol->overlap);
        FREE(ol);
        return ERR_FAILED;
    }
    return ERR_OK;
}

#endif//EV_IOCP
