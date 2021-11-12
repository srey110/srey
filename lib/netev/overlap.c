#include "netev/netev.h"

#ifdef NETEV_IOCP
#define _FLAGS_READY 0x01
#define _FLAGS_LOOP  0x02
#define MAX_DELAYFREE_CNT  20
#define MAX_ACCEPT_SOCKEX SOCKK_BACKLOG
#define NET_ERROR(err, format, ...) \
            if (ERR_FAILED != err\
            && WSAECONNRESET != err \
            && WSAENOTSOCK != err \
            && WSAENOTCONN != err \
            && WSAESHUTDOWN != err\
            && ERROR_OPERATION_ABORTED != err\
            && ERROR_NOT_FOUND != err\
            && WSAECONNABORTED != err)\
            LOG_ERROR(format, ##__VA_ARGS__)
struct overlap_acpt_ctx
{
    struct sock_ctx sock;
    struct listener_ctx *listener;
    DWORD bytes;
    char addrbuf[sizeof(struct sockaddr_storage)];
};
struct listener_ctx
{
    int32_t family;
    int32_t freecnt;
    volatile atomic_t acpcnt;
    SOCKET  sock;
    accept_cb acp_cb;
    void *udata;
    struct netev_ctx *netev;
    struct overlap_acpt_ctx overlap_acpt[MAX_ACCEPT_SOCKEX];
};
struct overlap_ctx
{
    struct sock_ctx overlap_r;
    struct sock_ctx overlap_w;
    int32_t flags;
    int32_t family;  //AF_INET  AF_INET6    
    int32_t socktype;//SOCK_STREAM  SOCK_DGRAM
    int32_t iovcnt_r;
    int32_t iovcnt_w;
    int32_t freecnt;
    int32_t addrlen;
    volatile atomic_t closed;
    volatile atomic_t ref_r;
    volatile atomic_t ref_w;    
    DWORD bytes_r;
    DWORD flag_r;
    DWORD bytes_w;
    connect_cb conn_cb;
    read_cb r_cb;
    write_cb w_cb;
    close_cb c_cb;
    void *udata;
    struct netev_ctx *netev;
    struct buffer_ctx buf_r;
    struct buffer_ctx buf_w;
    mutex_ctx lock_close;//CLOSE后不再触发write_cb
    IOV_TYPE wsabuf_r[MAX_RECV_IOV_COUNT];
    IOV_TYPE wsabuf_w[MAX_SEND_IOV_COUNT];
    struct sockaddr_storage rmtaddr;//存储数据来源IP地址
};
struct overlap_sendto_ctx
{
    struct sock_ctx overlap_sendto;
    struct overlap_ctx *udpctx;
    DWORD bytes;
    IOV_TYPE wsabuf;
    mutex_ctx lock_send;
};
static inline int32_t _post_accept(struct overlap_acpt_ctx *pacpol)
{
    SOCKET sock = sock_create(pacpol->listener->family, SOCK_STREAM);
    if (INVALID_SOCK == sock)
    {
        return ERRNO;
    }

    ZERO(&pacpol->sock.overlapped, sizeof(pacpol->sock.overlapped));
    pacpol->bytes = 0;
    pacpol->sock.sock = sock;
    if (!pacpol->listener->netev->watcher[0].
        acceptex(pacpol->listener->sock,//Listen Socket
        pacpol->sock.sock,              //Accept Socket
        &pacpol->addrbuf,
        0,
        sizeof(pacpol->addrbuf) / 2,
        sizeof(pacpol->addrbuf) / 2,
        &pacpol->bytes,
        &pacpol->sock.overlapped))
    {
        int32_t irtn = ERRNO;
        if (ERROR_IO_PENDING != irtn)
        {
            SAFE_CLOSE_SOCK(pacpol->sock.sock);
            return irtn;
        }
    }

    return ERR_OK;
}
static inline void _on_accept_cb(struct watcher_ctx *pwatcher, struct sock_ctx *psock, int32_t iev, int32_t *pstop)
{
    struct overlap_acpt_ctx *pacpol = UPCAST(psock, struct overlap_acpt_ctx, sock);
    SOCKET sock = pacpol->sock.sock;
    int32_t irtn = _post_accept(pacpol);
    if (ERR_OK != irtn)
    {
        NET_ERROR(irtn, "%s", ERRORSTR(irtn));
        SOCK_CLOSE(sock);
        ATOMIC_ADD(&pacpol->listener->acpcnt, -1);
        return;
    }
    if (ERR_OK != pwatcher->err)
    {
        NET_ERROR(pwatcher->err, "%s", ERRORSTR(pwatcher->err));
        SOCK_CLOSE(sock);
        return;
    }

    irtn = setsockopt(sock, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
        (char *)&pacpol->listener->sock, sizeof(&pacpol->listener->sock));
    if (irtn < ERR_OK)
    {
        irtn = ERRNO;
        NET_ERROR(irtn, "%s", ERRORSTR(irtn));
        SOCK_CLOSE(sock);
        return;
    }
    struct sock_ctx *pnew = netev_add_sock(pacpol->listener->netev, 
        sock, SOCK_STREAM, pacpol->listener->family);
    if (NULL == pnew)
    {
        SOCK_CLOSE(sock);
        return;
    }
    pacpol->listener->acp_cb(pnew, pacpol->listener->udata);
}
static inline struct overlap_ctx *_get_overlap(struct sock_ctx *psock)
{
    return UPCAST(psock, struct overlap_ctx, overlap_r);
}
static inline int32_t _post_recv(struct overlap_ctx *polctx)
{
    ZERO(&polctx->overlap_r.overlapped, sizeof(polctx->overlap_r.overlapped));
    polctx->flag_r = 0;
    polctx->bytes_r = 0;
    polctx->iovcnt_r = buffer_write_iov_application(&polctx->buf_r,
        MAX_RECV_IOV_SIZE, polctx->wsabuf_r, MAX_RECV_IOV_COUNT);

    ATOMIC_ADD(&polctx->ref_r, 1);
    int32_t irtn = WSARecv(polctx->overlap_r.sock,
        polctx->wsabuf_r,
        (DWORD)polctx->iovcnt_r,
        &polctx->bytes_r,
        &polctx->flag_r,
        &polctx->overlap_r.overlapped,
        NULL);
    if (ERR_OK != irtn)
    {
        irtn = ERRNO;
        if (WSA_IO_PENDING != irtn)
        {
            buffer_write_iov_commit(&polctx->buf_r, 0,
                polctx->wsabuf_r, polctx->iovcnt_r);
            ATOMIC_ADD(&polctx->ref_r, -1);
            return irtn;
        }
    }

    return ERR_OK;
}
static inline void _on_close(struct overlap_ctx *polctx)
{
    if (!ATOMIC_CAS(&polctx->closed, 0, 1))
    {
        return;
    }
    mutex_lock(&polctx->lock_close);
    polctx->c_cb(&polctx->overlap_r, polctx->udata);
    mutex_unlock(&polctx->lock_close);
}
static inline int32_t _commit_buf_r(struct watcher_ctx *pwatcher, struct overlap_ctx *polctx)
{
    if (0 == pwatcher->bytes
        || ERR_OK != pwatcher->err)
    {
        buffer_write_iov_commit(&polctx->buf_r, 0, polctx->wsabuf_r, polctx->iovcnt_r);
        _on_close(polctx);
        ATOMIC_ADD(&polctx->ref_r, -1);
        return ERR_FAILED;
    }
    buffer_write_iov_commit(&polctx->buf_r, (size_t)pwatcher->bytes, polctx->wsabuf_r, polctx->iovcnt_r);
    return ERR_OK;
}
static inline void _on_recv_cb(struct watcher_ctx *pwatcher, struct sock_ctx *psock, int32_t iev, int32_t *pstop)
{
    struct overlap_ctx *polctx = _get_overlap(psock);
    if (ERR_OK != _commit_buf_r(pwatcher, polctx))
    {
        return;
    }

    polctx->r_cb(psock, &polctx->buf_r, (size_t)pwatcher->bytes, NULL, 0, polctx->udata);
    int32_t irtn = _post_recv(polctx);
    if (ERR_OK != irtn)
    {
        NET_ERROR(irtn, "%s", ERRORSTR(irtn));
        _on_close(polctx);
    }
    ATOMIC_ADD(&polctx->ref_r, -1);
}
static inline int32_t _post_recv_from(struct overlap_ctx *polctx)
{
    ZERO(&polctx->overlap_r.overlapped, sizeof(polctx->overlap_r.overlapped));
    polctx->flag_r = 0;
    polctx->bytes_r = 0;
    polctx->addrlen = (int32_t)sizeof(polctx->rmtaddr);
    polctx->iovcnt_r = buffer_write_iov_application(&polctx->buf_r,
        MAX_RECV_IOV_SIZE, polctx->wsabuf_r, MAX_RECV_IOV_COUNT);

    ATOMIC_ADD(&polctx->ref_r, 1);
    int32_t irtn = WSARecvFrom(polctx->overlap_r.sock,
        polctx->wsabuf_r,
        (DWORD)polctx->iovcnt_r,
        &polctx->bytes_r,
        &polctx->flag_r,
        (struct sockaddr*)&polctx->rmtaddr,
        &polctx->addrlen,
        &polctx->overlap_r.overlapped,
        NULL);
    if (ERR_OK != irtn)
    {
        irtn = ERRNO;
        if (WSA_IO_PENDING != irtn)
        {
            buffer_write_iov_commit(&polctx->buf_r, 0,
                polctx->wsabuf_r, polctx->iovcnt_r);
            ATOMIC_ADD(&polctx->ref_r, -1);
            return irtn;
        }
    }

    return ERR_OK;
}
static inline void _on_recvfrom_cb(struct watcher_ctx *pwatcher, struct sock_ctx *psock, int32_t iev, int32_t *pstop)
{
    struct overlap_ctx *polctx = _get_overlap(psock);
    if (ERR_OK != _commit_buf_r(pwatcher, polctx))
    {
        return;
    }

    char achost[IP_LENS];
    char acport[PORT_LENS];
    uint16_t uport = 0;
    int32_t irtn = getnameinfo((struct sockaddr *)&polctx->rmtaddr, polctx->addrlen,
        achost, sizeof(achost), acport, sizeof(acport),
        NI_NUMERICHOST | NI_NUMERICSERV);
    if (ERR_OK == irtn)
    {
        uport = atoi(acport);
    }
    polctx->r_cb(psock, &polctx->buf_r, (size_t)pwatcher->bytes, achost, uport, polctx->udata);
    irtn = _post_recv_from(polctx);
    if (ERR_OK != irtn)
    {
        NET_ERROR(irtn, "%s", ERRORSTR(irtn));
        _on_close(polctx);
    }
    ATOMIC_ADD(&polctx->ref_r, -1);
}
static inline void _on_connect_cb(struct watcher_ctx *pwatcher, struct sock_ctx *psock, int32_t iev, int32_t *pstop)
{
    struct overlap_ctx *pol = _get_overlap(psock); 
    pol->overlap_r.ev_cb = _on_recv_cb;
    pol->conn_cb(psock, pwatcher->err, pol->udata);
    ATOMIC_ADD(&pol->ref_r, -1);
}
static inline int32_t _post_send(struct overlap_ctx *polctx)
{
    if (0 != ATOMIC_GET(&polctx->closed))
    {
        return ERR_FAILED;
    }
    uint32_t uicnt = buffer_read_iov_application(&polctx->buf_w,
        MAX_SEND_IOV_SIZE, polctx->wsabuf_w, MAX_SEND_IOV_COUNT);
    if (0 == uicnt)
    {
        return ERR_FAILED;
    }
    ZERO(&polctx->overlap_w.overlapped, sizeof(polctx->overlap_w.overlapped));
    polctx->iovcnt_w = uicnt;

    ATOMIC_ADD(&polctx->ref_w, 1);
    int32_t irtn = WSASend(polctx->overlap_w.sock,
        polctx->wsabuf_w,
        (DWORD)uicnt,
        &polctx->bytes_w,
        0,
        &polctx->overlap_w.overlapped,
        NULL);
    if (ERR_OK != irtn)
    {
        irtn = ERRNO;
        if (WSA_IO_PENDING != irtn)
        {
            buffer_read_iov_commit(&polctx->buf_w, 0);
            NET_ERROR(irtn, "%s", ERRORSTR(irtn));
            ATOMIC_ADD(&polctx->ref_w, -1);
            return irtn;
        }
    }

    return ERR_OK;
}
static inline void _on_send_cb(struct watcher_ctx *pwatcher, struct sock_ctx *psock, int32_t iev, int32_t *pstop)
{
    struct overlap_ctx *polctx = UPCAST(psock, struct overlap_ctx, overlap_w);
    if (0 == pwatcher->bytes
        || ERR_OK != pwatcher->err)
    {
        buffer_read_iov_commit(&polctx->buf_w, 0);
        ATOMIC_ADD(&polctx->ref_w, -1);
        return;
    }

    buffer_read_iov_commit(&polctx->buf_w, (size_t)pwatcher->bytes);
    if (NULL != polctx->w_cb)
    {
        mutex_lock(&polctx->lock_close);
        if (0 == ATOMIC_GET(&polctx->closed))
        {
            polctx->w_cb(&polctx->overlap_r, (size_t)pwatcher->bytes, polctx->udata);
        }
        mutex_unlock(&polctx->lock_close);
    }

    (void)_post_send(polctx);
    ATOMIC_ADD(&polctx->ref_w, -1);
}
static inline void _on_sendto_cb(struct watcher_ctx *pwatcher, struct sock_ctx *psock, int32_t iev, int32_t *pstop)
{
    struct overlap_sendto_ctx *psendtool = UPCAST(psock, struct overlap_sendto_ctx, overlap_sendto);
    struct overlap_ctx *polctx = psendtool->udpctx;
    if (0 == pwatcher->bytes
        || ERR_OK != pwatcher->err)
    {
        mutex_lock(&psendtool->lock_send);
        mutex_unlock(&psendtool->lock_send);
        mutex_free(&psendtool->lock_send);
        FREE(psendtool);
        ATOMIC_ADD(&polctx->ref_w, -1);
        return;
    }
    
    if (NULL != polctx->w_cb)
    {
        mutex_lock(&polctx->lock_close);
        if (0 == ATOMIC_GET(&polctx->closed))
        {
            polctx->w_cb(&polctx->overlap_r, (size_t)pwatcher->bytes, polctx->udata);
        }
        mutex_unlock(&polctx->lock_close);
    }

    mutex_lock(&psendtool->lock_send);
    mutex_unlock(&psendtool->lock_send);
    mutex_free(&psendtool->lock_send);
    FREE(psendtool);
    ATOMIC_ADD(&polctx->ref_w, -1);
}
static inline int32_t _trybind(SOCKET sock, const int32_t ifamily)
{
    int32_t irtn;
    struct netaddr_ctx addr;
    if (AF_INET == ifamily)
    {
        irtn = netaddr_sethost(&addr, "127.0.0.1", 0);
    }
    else
    {
        irtn = netaddr_sethost(&addr, "::1", 0);
    }
    if (ERR_OK != irtn)
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return irtn;
    }
    if (ERR_OK != bind(sock, netaddr_addr(&addr), netaddr_size(&addr)))
    {
        irtn = ERRNO;
        LOG_ERROR("%s", ERRORSTR(irtn));
        return irtn;
    }

    return ERR_OK;
}
struct sock_ctx *netev_add_sock(struct netev_ctx *pctx, SOCKET sock, int32_t itype, int32_t ifamily)
{
    struct netaddr_ctx addr;
    int32_t irtn = netaddr_localaddr(&addr, sock, ifamily);
    if (ERR_OK != irtn)
    {
        irtn = _trybind(sock, ifamily);
        if (ERR_OK != irtn)
        {
            return NULL;
        }
    }
    struct overlap_ctx *pol = MALLOC(sizeof(struct overlap_ctx));
    if (NULL == pol)
    {
        LOG_ERROR("%s", ERRSTR_MEMORY);
        return NULL;
    }

    ZERO(pol, sizeof(struct overlap_ctx));
    pol->socktype = itype;
    pol->family = ifamily;
    pol->overlap_r.sock = sock;
    pol->overlap_w.sock = sock;
    uint32_t uid = (uint32_t)ATOMIC_ADD(&pctx->id, 1);
    pol->overlap_r.id = uid;
    pol->overlap_w.id = uid;
    socknbio(sock);
    if (SOCK_DGRAM == itype)
    {
        DWORD dbytes = 0;
        BOOL bbehavior = FALSE;
        if (WSAIoctl(sock, SIO_UDP_CONNRESET,
            &bbehavior, sizeof(bbehavior),
            NULL, 0, &dbytes,
            NULL, NULL) < ERR_OK)
        {
            LOG_WARN("WSAIoctl(%d, SIO_UDP_CONNRESET...) failed. %s", (int32_t)sock, ERRORSTR(ERRNO));
        }
        pol->overlap_r.ev_cb = _on_recvfrom_cb;
    }
    else
    {
        closereset(sock);
        socknodelay(sock);
        sockkpa(sock, SOCKKPA_DELAY, SOCKKPA_INTVL);
        pol->overlap_r.ev_cb = _on_recv_cb;
    }
    pol->overlap_w.ev_cb = _on_send_cb;
    pol->netev = pctx;
    buffer_init(&pol->buf_r);
    buffer_init(&pol->buf_w);
    mutex_init(&pol->lock_close);

    return &pol->overlap_r;
}
static inline void _listener_free(void *pdata)
{
    struct listener_ctx *plsn = pdata;
    for (int32_t i = 0; i < MAX_ACCEPT_SOCKEX; i++)
    {
        SAFE_CLOSE_SOCK(plsn->overlap_acpt[i].sock.sock);
    }
    SAFE_CLOSE_SOCK(plsn->sock);
    FREE(plsn);
}
static inline void _listener_delay_free(struct tw_node_ctx *ptw, void *pdata)
{
    struct listener_ctx *plsn = pdata;
    plsn->freecnt++;
    atomic_t iacpcnt = ATOMIC_GET(&plsn->acpcnt);
    if (0 == iacpcnt)
    {
        _listener_free(plsn);
        tw_remove(ptw);
        return;
    }
    if (plsn->freecnt >= MAX_DELAYFREE_CNT)
    {
        LOG_WARN("free listener %d use long time. acpcnt %d", (int32_t)plsn->sock, (int32_t)iacpcnt);
        plsn->freecnt = 0;
    }
}
void listener_free(struct listener_ctx *plsn)
{
    SAFE_CLOSE_SOCK(plsn->sock);
    atomic_t iacpcnt = ATOMIC_GET(&plsn->acpcnt);
    if (0 == iacpcnt)
    {
        _listener_free(plsn);
    }
    else
    {
        tw_add(plsn->netev->tw, 10, -1, _listener_delay_free, plsn);
    }
}
static inline int32_t _acceptex(struct listener_ctx *plsn)
{
    if (NULL == CreateIoCompletionPort((HANDLE)plsn->sock,
        plsn->netev->watcher[0].iocp,
        0,
        plsn->netev->thcnt))
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    struct overlap_acpt_ctx *pacpol;
    for (int32_t i = 0; i < MAX_ACCEPT_SOCKEX; i++)
    {
        pacpol = &plsn->overlap_acpt[i];
        pacpol->sock.sock = INVALID_SOCK;
        pacpol->sock.ev_cb = _on_accept_cb;
        pacpol->listener = plsn;
    }
    int32_t irtn;
    for (int32_t i = 0; i < MAX_ACCEPT_SOCKEX; i++)
    {
        pacpol = &plsn->overlap_acpt[i];        
        irtn = _post_accept(pacpol);
        if (ERR_OK != irtn)
        {
            NET_ERROR(irtn, "%s", ERRORSTR(irtn));
            for (int32_t j = 0; j < i; j++)
            {
                SAFE_CLOSE_SOCK(plsn->overlap_acpt[j].sock.sock);
            }
            return irtn;
        }
        ATOMIC_ADD(&plsn->acpcnt, 1);
    }

    return ERR_OK;
}
struct listener_ctx *netev_listener(struct netev_ctx *pctx,
    const char *phost, const uint16_t usport, accept_cb acp_cb, void *pdata)
{
    ASSERTAB(NULL != acp_cb, ERRSTR_NULLP);
    struct netaddr_ctx addr;
    int32_t irtn = netaddr_sethost(&addr, phost, usport);
    if (ERR_OK != irtn)
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return NULL;
    }
    SOCKET sock = sock_listen(&addr);
    if (INVALID_SOCK == sock)
    {
        return NULL;
    }
    struct listener_ctx *plsn = MALLOC(sizeof(struct listener_ctx));
    if (NULL == plsn)
    {
        SOCK_CLOSE(sock);
        return NULL;
    }
    plsn->family = (uint32_t)netaddr_family(&addr);
    plsn->sock = sock;
    plsn->acp_cb = acp_cb;
    plsn->udata = pdata;
    plsn->netev = pctx;
    plsn->freecnt = 0;
    plsn->acpcnt = 0;
    if (ERR_OK != _acceptex(plsn))
    {
        listener_free(plsn);
        return NULL;
    }

    return plsn;
}
static inline void _sock_free(void *pdata)
{
    struct overlap_ctx *pol = pdata;
    SAFE_CLOSE_SOCK(pol->overlap_r.sock);
    buffer_free(&pol->buf_r);
    buffer_free(&pol->buf_w);
    mutex_free(&pol->lock_close);
    FREE(pol);
}
static inline void _sock_delay_free(struct tw_node_ctx *ptw, void *pdata)
{
    struct overlap_ctx *pol = pdata;
    pol->freecnt++;
    atomic_t iref_r = ATOMIC_GET(&pol->ref_r);
    atomic_t iref_w = ATOMIC_GET(&pol->ref_w);
    if (0 == iref_r 
        && 0 == iref_w)
    {
        _sock_free(pol);
        tw_remove(ptw);
        return;
    }
    if (pol->freecnt >= MAX_DELAYFREE_CNT)
    {
        LOG_WARN("free socket use long time id %d type %d . ref_r %d, ref_w %d",
            pol->overlap_r.id, pol->socktype, (int32_t)iref_r, (int32_t)iref_w);
        if (0 != iref_r)
        {
            pol->freecnt = 0;
            return;
        }
        _sock_free(pol);
        tw_remove(ptw);
    }
}
void sock_free(struct sock_ctx *psock)
{
    struct overlap_ctx *pol = _get_overlap(psock);
    atomic_t iref_r = ATOMIC_GET(&pol->ref_r);
    atomic_t iref_w = ATOMIC_GET(&pol->ref_w);
    SAFE_CLOSE_SOCK(pol->overlap_r.sock);
    if (0 == iref_r
        && 0 == iref_w)
    {
        _sock_free(pol);
    }
    else
    {
        tw_add(pol->netev->tw, 10, -1, _sock_delay_free, pol);
    }    
}
static inline int32_t _connectex(struct watcher_ctx *pwatcher, struct overlap_ctx *pol, struct netaddr_ctx *paddr)
{
    pol->overlap_r.ev_cb = _on_connect_cb;

    ATOMIC_ADD(&pol->ref_r, 1);
    if (!pwatcher->connectex(pol->overlap_r.sock,
        netaddr_addr(paddr),
        netaddr_size(paddr),
        NULL,
        0,
        &pol->bytes_r,
        &pol->overlap_r.overlapped))
    {
        int32_t irtn = ERRNO;
        if (ERROR_IO_PENDING != irtn)
        {
            NET_ERROR(irtn, "%s", ERRORSTR(irtn));
            ATOMIC_ADD(&pol->ref_r, -1);
            return irtn;
        }
    }

    return ERR_OK;
}
struct sock_ctx *netev_connecter(struct netev_ctx *pctx, uint32_t utimeout,
    const char *phost, const uint16_t usport, connect_cb conn_cb, void *pdata)
{
    ASSERTAB(NULL != conn_cb, ERRSTR_NULLP);
    struct netaddr_ctx addr;
    int32_t irtn = netaddr_sethost(&addr, phost, usport);
    if (ERR_OK != irtn)
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return NULL;
    }
    SOCKET sock = sock_create(netaddr_family(&addr), SOCK_STREAM);
    if (INVALID_SOCK == sock)
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return NULL;
    }
    struct sock_ctx *psock = netev_add_sock(pctx, sock, SOCK_STREAM, netaddr_family(&addr));
    if (NULL == psock)
    {
        SOCK_CLOSE(sock);
        return NULL;
    }
    struct watcher_ctx *pwatcher = _netev_get_watcher(pctx, sock);
    _netev_add(pwatcher, psock, EV_READ | EV_WRITE);
    struct overlap_ctx *pol = _get_overlap(psock);
    pol->conn_cb = conn_cb;
    pol->udata = pdata;
    pol->flags |= _FLAGS_READY;
    socknbio(sock);
    if (ERR_OK != _connectex(pwatcher, pol, &addr))
    {
        sock_free(&pol->overlap_r);
        return NULL;
    }

    return psock;
}
int32_t netev_enable_rw(struct netev_ctx *pctx, struct sock_ctx *psock,
    read_cb r_cb, write_cb w_cb, close_cb c_cb, void *pdata)
{
    ASSERTAB(NULL != r_cb && NULL != c_cb, ERRSTR_NULLP);
    struct overlap_ctx *pol = _get_overlap(psock);
    if (pol->flags & _FLAGS_LOOP)
    {
        return ERR_OK;
    }
    
    if (!(pol->flags & _FLAGS_READY))
    {
        _netev_add(&pctx->watcher[0], psock, EV_READ | EV_WRITE);
        pol->flags |= _FLAGS_READY;
    }
    int32_t irtn;
    if (SOCK_STREAM == pol->socktype)
    {
        irtn = _post_recv(pol);
    }
    else
    {
        irtn = _post_recv_from(pol);
    }
    if (ERR_OK != irtn)
    {
        return irtn;
    }
    pol->flags |= _FLAGS_LOOP;
    pol->udata = pdata;
    pol->c_cb = c_cb;
    pol->r_cb = r_cb;
    pol->w_cb = w_cb;

    return ERR_OK;
}
static inline void _on_disconnectex(struct watcher_ctx *pwatcher, 
    struct sock_ctx *psock, int32_t iev, int32_t *pstop)
{
    FREE(psock);
}
void sock_close(struct sock_ctx *psock)
{
    struct overlap_ctx *pol = _get_overlap(psock);
    if (!(pol->flags & _FLAGS_LOOP))
    {
        SAFE_CLOSE_SOCK(pol->overlap_r.sock);
        return;
    }
    if (0 != ATOMIC_GET(&pol->closed))
    {
        return;
    }
    if (SOCK_DGRAM == pol->socktype)
    {
        if (!CancelIoEx((HANDLE)pol->overlap_r.sock, NULL))
        {
            //防止free的时候关掉其他socket
            SAFE_CLOSE_SOCK(pol->overlap_r.sock);
        }
        return;
    }
    struct sock_ctx *pdisconol = MALLOC(sizeof(struct sock_ctx));
    if (NULL == pdisconol)
    {
        LOG_ERROR("%s", ERRSTR_MEMORY);
        SAFE_CLOSE_SOCK(pol->overlap_r.sock);
        return;
    }
    ZERO(&pdisconol->overlapped, sizeof(pdisconol->overlapped));
    pdisconol->sock = pol->overlap_r.sock;
    pdisconol->ev_cb = _on_disconnectex;
    //对端会收到0字节
    if (!pol->netev->watcher[0].disconnectex(pol->overlap_r.sock, &pdisconol->overlapped, 0, 0))
    {
        int32_t irtn = ERRNO;
        if (ERROR_IO_PENDING != irtn)
        {
            NET_ERROR(irtn, "%s", ERRORSTR(irtn));
            FREE(pdisconol);
            SAFE_CLOSE_SOCK(pol->overlap_r.sock);
            return;
        }
    }
}
static inline int32_t _tcp_trysend(struct overlap_ctx *polctx)
{
    if (!ATOMIC_CAS(&polctx->ref_w, 0, 1))
    {
        return ERR_OK;
    }
    int32_t irtn = _post_send(polctx);
    ATOMIC_ADD(&polctx->ref_w, -1);
    if (ERR_OK != irtn)
    {
        return irtn;
    }

    return ERR_OK;
}
int32_t sock_send(struct sock_ctx *psock, void *pdata, const size_t uilens)
{
    struct overlap_ctx *polctx = _get_overlap(psock);
    if (0 != ATOMIC_GET(&polctx->closed))
    {
        return ERR_FAILED;
    }
    ASSERTAB(SOCK_STREAM == polctx->socktype, "only support tcp.");
    ASSERTAB(ERR_OK == buffer_append(&polctx->buf_w, pdata, uilens), "buffer_append error.");
    return _tcp_trysend(polctx);
}
int32_t sock_send_buffer(struct sock_ctx *psock)
{
    struct overlap_ctx *polctx = _get_overlap(psock);
    if (0 != ATOMIC_GET(&polctx->closed))
    {
        return ERR_FAILED;
    }
    ASSERTAB(SOCK_STREAM == polctx->socktype, "only support tcp.");
    return _tcp_trysend(polctx);
}
int32_t sock_type(struct sock_ctx *psock)
{
    return _get_overlap(psock)->socktype;
}
struct buffer_ctx *sock_buffer_r(struct sock_ctx *psock)
{
    struct overlap_ctx *polctx = _get_overlap(psock);
    return &polctx->buf_r;
}
struct buffer_ctx *sock_buffer_w(struct sock_ctx *psock)
{
    struct overlap_ctx *polctx = _get_overlap(psock);
    return &polctx->buf_w;
}
static inline int32_t _post_sendto(struct overlap_sendto_ctx *psendtool, struct netaddr_ctx *paddr)
{
    ZERO(&psendtool->overlap_sendto.overlapped, sizeof(psendtool->overlap_sendto.overlapped));
    psendtool->bytes = 0;
    psendtool->overlap_sendto.ev_cb = _on_sendto_cb;
    mutex_init(&psendtool->lock_send);

    ATOMIC_ADD(&psendtool->udpctx->ref_w, 1);
    mutex_lock(&psendtool->lock_send);
    int32_t irtn = WSASendTo(psendtool->udpctx->overlap_r.sock,
        &psendtool->wsabuf,
        1,
        &psendtool->bytes,
        0,
        netaddr_addr(paddr),
        netaddr_size(paddr),
        &psendtool->overlap_sendto.overlapped,
        NULL);
    mutex_unlock(&psendtool->lock_send);
    if (ERR_OK != irtn)
    {
        irtn = ERRNO;
        if (ERROR_IO_PENDING != irtn)
        {
            NET_ERROR(irtn, "%s", ERRORSTR(irtn));
            ATOMIC_ADD(&psendtool->udpctx->ref_w, -1);
            return irtn;
        }
    }

    return ERR_OK;
}
int32_t sock_sendto(struct sock_ctx *psock, const char *phost, uint16_t uport, 
    void *pdata, const size_t uilens)
{
    struct overlap_ctx *polctx = _get_overlap(psock);
    if (0 != ATOMIC_GET(&polctx->closed))
    {
        return ERR_FAILED;
    }
    ASSERTAB(SOCK_DGRAM == polctx->socktype, "only support udp.");
    struct netaddr_ctx addr;
    int32_t irtn = netaddr_sethost(&addr, phost, uport);
    if (ERR_OK != irtn)
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return irtn;
    }
    struct overlap_sendto_ctx *psendtool = MALLOC(sizeof(struct overlap_sendto_ctx));
    if (NULL == psendtool)
    {
        LOG_ERROR("%s", ERRSTR_MEMORY);
        return ERR_FAILED;
    }
    psendtool->udpctx = polctx;
    psendtool->wsabuf.IOV_PTR_FIELD = pdata;
    psendtool->wsabuf.IOV_LEN_FIELD = (IOV_LEN_TYPE)uilens;
    irtn = _post_sendto(psendtool, &addr);
    if (ERR_OK != irtn)
    {
        FREE(psendtool);
        return irtn;
    }

    return ERR_OK;
}
#endif
