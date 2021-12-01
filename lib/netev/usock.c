#include "netev/netev.h"

#ifndef NETEV_IOCP

struct lsn_ctx
{
    struct sock_ctx sock;
    struct listener_ctx *listener;
};
struct listener_ctx
{
    int32_t family;
    int32_t freecnt;
    int32_t lsncnt;
    volatile atomic_t acpcnt;
    accept_cb acp_cb;
    struct netev_ctx *netev;
    struct lsn_ctx *lsn;
    struct ud_ctx ud;
};
struct usock_ctx
{
    struct sock_ctx sock;
    int32_t freecnt;
    int32_t family;
    int32_t socktype;//SOCK_STREAM  SOCK_DGRAM
    volatile atomic_t sending;
    volatile atomic_t ref_r;
    volatile atomic_t ref_cmd;
    connect_cb conn_cb;
    recv_cb r_cb;
    send_cb w_cb;
    close_cb c_cb;
    struct watcher_ctx *watcher;
    struct ud_ctx ud;
    struct buffer_ctx buf_r;
    struct buffer_ctx buf_w;
    IOV_TYPE wsabuf_r[MAX_RECV_IOV_COUNT];
    IOV_TYPE wsabuf_w[MAX_SEND_IOV_COUNT];
};
static inline struct lsn_ctx * _get_lsn(struct sock_ctx *psock)
{
    return UPCAST(psock, struct lsn_ctx, sock);
}
static inline struct usock_ctx * _get_usock(struct sock_ctx *psock)
{
    return UPCAST(psock, struct usock_ctx, sock);
}
void _uev_sock_close(struct sock_ctx *psock)
{
    if (psock->flags & _FLAGS_LSN)
    {
        struct lsn_ctx *plsn = _get_lsn(psock);
        SAFE_CLOSE_SOCK(psock->sock);
        ATOMIC_ADD(&plsn->listener->acpcnt, -1);
    }
    else if (psock->flags & _FLAGS_CONN)
    {
        struct usock_ctx *pusock = _get_usock(psock);
        shutdown(psock->sock, SHUT_WR);
        SAFE_CLOSE_SOCK(psock->sock);
        pusock->conn_cb(psock, ERR_FAILED, &pusock->ud);
        ATOMIC_ADD(&pusock->ref_r, -1);
    }
    else if (psock->flags & _FLAGS_NORM)
    {
        struct usock_ctx *pusock = _get_usock(psock);
        shutdown(psock->sock, SHUT_WR);
        SAFE_CLOSE_SOCK(psock->sock);
        pusock->c_cb(psock, &pusock->ud);
        ATOMIC_ADD(&pusock->ref_r, -1);
    }
}
static inline void _listener_free(struct listener_ctx *plsn)
{
    for (int32_t i = 0; i < plsn->lsncnt; i++)
    {
        SAFE_CLOSE_SOCK(plsn->lsn[i].sock.sock);
    }
    FREE(plsn->lsn);
    FREE(plsn);
}
static inline void _listener_delay_free(struct ud_ctx *pud)
{
    struct listener_ctx *plsn = (struct listener_ctx *)pud->handle;
    atomic_t iacpcnt = ATOMIC_GET(&plsn->acpcnt);
    if (0 == iacpcnt)
    {
        _listener_free(plsn);
        return;
    }
    plsn->freecnt++;
    if (plsn->freecnt >= MAX_DELAYFREE_CNT)
    {
        LOG_WARN("free listener use long time. acpcnt %d", (int32_t)iacpcnt);
        plsn->freecnt = 0;
    }
    tw_add(plsn->netev->tw, DELAYFREE_TIME, _listener_delay_free, pud);
}
void listener_free(struct listener_ctx *plsn)
{
    //·¢Æð¹Ø±Õ
    if (1 == plsn->lsncnt)
    {
        struct watcher_ctx *pwatcher = _netev_get_watcher(plsn->netev, plsn->lsn[0].sock.sock);
        _uev_cmd_close(pwatcher, &plsn->lsn[0].sock);
    }
    else
    {
        for (int32_t i = 0; i < plsn->lsncnt; i++)
        {
            _uev_cmd_close(&plsn->netev->watcher[i], &plsn->lsn[i].sock);
        }
    }

    atomic_t iacpcnt = ATOMIC_GET(&plsn->acpcnt);
    if (0 == iacpcnt)
    {
        _listener_free(plsn);
    }
    else
    {
        struct ud_ctx ud;
        ud.handle = (uintptr_t)plsn;
        tw_add(plsn->netev->tw, DELAYFREE_TIME, _listener_delay_free, &ud);
    }
}
static inline void _sock_free(struct usock_ctx *pusock)
{
    buffer_free(&pusock->buf_r);
    buffer_free(&pusock->buf_w);
    FREE(pusock);
}
static inline void _sock_delay_free(struct ud_ctx *pud)
{
    struct usock_ctx *pusock = (struct usock_ctx *)pud->handle;
    atomic_t iref_r = ATOMIC_GET(&pusock->ref_r);
    atomic_t iref_cmd = ATOMIC_GET(&pusock->ref_cmd);
    if (0 == iref_r
        && 0 == iref_cmd)
    {
        _sock_free(pusock);
        return;
    }
    pusock->freecnt++;
    if (pusock->freecnt >= MAX_DELAYFREE_CNT)
    {
        LOG_WARN("free socket use long time id %"PRIu64" type %d . ref_r %d ref_cmd %d",
            pusock->sock.id, pusock->socktype, iref_r, iref_cmd);
        pusock->freecnt = 0;
    }
    tw_add(pusock->watcher->netev->tw, DELAYFREE_TIME, _sock_delay_free, pud);
}
void sock_free(struct sock_ctx *psock)
{
    struct usock_ctx *pusock = _get_usock(psock); 
    SAFE_CLOSE_SOCK(pusock->sock.sock);
    if (0 == ATOMIC_GET(&pusock->ref_r)
        && 0 == ATOMIC_GET(&pusock->ref_cmd))
    {
        _sock_free(pusock);
    }
    else
    {
        struct ud_ctx ud;
        ud.handle = (uintptr_t)pusock;
        tw_add(pusock->watcher->netev->tw, DELAYFREE_TIME, _sock_delay_free, &ud);
    }
}
void sock_close(struct sock_ctx *psock)
{
    if (psock->flags & _FLAGS_CLOSE)
    {
        return;
    }
    struct usock_ctx *pusock = _get_usock(psock);
    _uev_cmd_close(pusock->watcher, psock);
}
static inline void _evport_readd(struct watcher_ctx *pwatcher, struct sock_ctx *psock)
{
#ifdef NETEV_EVPORT
    if (ERR_OK != _uev_add(pwatcher, psock, psock->events))
    {
        _add_close_qu(pwatcher, psock);
    }
#endif
}
static inline void _on_accept_cb(struct watcher_ctx *pwatcher, struct sock_ctx *psock, uint32_t uiev, int32_t *pstop)
{
    struct lsn_ctx *plsn = _get_lsn(psock);
    SOCKET fd = accept(plsn->sock.sock, NULL, NULL);
    if (INVALID_SOCK == fd)
    {
        _evport_readd(pwatcher, psock);
        return;
    }
    struct sock_ctx *pacpsock = netev_add_sock(plsn->listener->netev, 
        fd, SOCK_STREAM, plsn->listener->family);
    if (NULL == pacpsock)
    {
        SOCK_CLOSE(fd);
        _evport_readd(pwatcher, psock);
        return;
    }

    plsn->listener->acp_cb(pacpsock, &plsn->listener->ud);
    _evport_readd(pwatcher, psock);
}
static inline int32_t _check_closed(struct usock_ctx *pusock, ssize_t ireaded, uint32_t uiovcnt)
{
    if (0 == ireaded)
    {
        buffer_write_iov_commit(&pusock->buf_r, 0, pusock->wsabuf_r, uiovcnt);
        return ERR_OK;
    }
    if (ERR_FAILED == ireaded)
    {
        buffer_write_iov_commit(&pusock->buf_r, 0, pusock->wsabuf_r, uiovcnt);
        int32_t irtn = ERRNO;
        if (!ERR_RW_RETRIABLE(irtn))
        {
            return ERR_OK;
        }
        return ERR_FAILED;
    }
    buffer_write_iov_commit(&pusock->buf_r, (size_t)ireaded, pusock->wsabuf_r, uiovcnt);

    return ERR_FAILED;
}
static inline int32_t _on_recv_cb(struct watcher_ctx *pwatcher, struct usock_ctx *pusock)
{
    int32_t icnt = socknread(pusock->sock.sock);
    if (ERR_FAILED == icnt)
    {
        return ERR_FAILED;
    }
    if (icnt > MAX_RECV_IOV_SIZE)
    {
        icnt = MAX_RECV_IOV_SIZE;
    }

    uint32_t uiovcnt = buffer_write_iov_application(&pusock->buf_r, icnt, pusock->wsabuf_r, MAX_RECV_IOV_COUNT);
    ssize_t ireaded = readv(pusock->sock.sock, pusock->wsabuf_r, uiovcnt);
    if (ERR_OK == _check_closed(pusock, ireaded, uiovcnt))
    {
        return ERR_FAILED;
    }
    if (ireaded < 0)
    {
        return ERR_OK;
    }
    pusock->r_cb(&pusock->sock, (size_t)ireaded, NULL, &pusock->ud);

    return ERR_OK;
}
static inline void _check_sending(struct watcher_ctx *pwatcher, struct usock_ctx *pusock)
{
    if (0 == buffer_size(&pusock->buf_w)
        && ATOMIC_CAS(&pusock->sending, 1, 0))
    {
        _uev_del(pwatcher, &pusock->sock, EV_WRITE);
    }
}
static inline int32_t _on_send_cb(struct watcher_ctx *pwatcher, struct usock_ctx *pusock)
{
    uint32_t uiovcnt = buffer_read_iov_application(&pusock->buf_w, MAX_SEND_IOV_SIZE, 
        pusock->wsabuf_w, MAX_SEND_IOV_COUNT);
    if (0 == uiovcnt)
    {
        _check_sending(pwatcher, pusock);
        return ERR_OK;
    }

    ssize_t isend = writev(pusock->sock.sock, pusock->wsabuf_w, uiovcnt);
    if (isend <= 0)
    {
        buffer_read_iov_commit(&pusock->buf_w, 0);
        int32_t irtn = ERRNO;
        if (!ERR_RW_RETRIABLE(irtn))
        {
            return ERR_FAILED;
        }
        return ERR_OK;
    }
    buffer_read_iov_commit(&pusock->buf_w, (size_t)isend);
    if (NULL != pusock->w_cb)
    {
        pusock->w_cb(&pusock->sock, (size_t)isend, &pusock->ud);
    }

    return ERR_OK;
}
static inline void _init_msghdr(struct msghdr *pmsg, union netaddr_ctx *paddr, IOV_TYPE *wsabuf, uint32_t iiovcnt)
{
    ZERO(pmsg, sizeof(struct msghdr));
    pmsg->msg_name = netaddr_addr(paddr);
    pmsg->msg_namelen = netaddr_size(paddr);
    pmsg->msg_iov = wsabuf;
    pmsg->msg_iovlen = iiovcnt;
}
static inline int32_t _on_recvfrom_cb(struct watcher_ctx *pwatcher, struct usock_ctx *pusock)
{
    int32_t icnt = socknread(pusock->sock.sock);
    if (ERR_FAILED == icnt)
    {
        return ERR_FAILED;
    }
    if (icnt > MAX_RECVFROM_IOV_SIZE)
    {
        icnt = MAX_RECVFROM_IOV_SIZE;
    }

    struct msghdr msg;
    union netaddr_ctx addr;
    netaddr_empty_addr(&addr, pusock->family);
    uint32_t uiovcnt = buffer_write_iov_application(&pusock->buf_r, icnt, pusock->wsabuf_r, MAX_RECV_IOV_COUNT);
    _init_msghdr(&msg, &addr, pusock->wsabuf_r, uiovcnt);
    ssize_t ireaded = recvmsg(pusock->sock.sock, &msg, 0);
    if (ERR_OK == _check_closed(pusock, ireaded, uiovcnt))
    {
        return ERR_FAILED;
    }
    if (ireaded < 0)
    {
        return ERR_OK;
    }
    pusock->r_cb(&pusock->sock, (size_t)ireaded, &addr, &pusock->ud);

    return ERR_OK;
}
static inline int32_t _on_sendto_cb(struct watcher_ctx *pwatcher, struct usock_ctx *pusock)
{
    struct udp_msg_ctx udpmsg;
    uint32_t uiovcnt = buffer_piece_read_application(&pusock->buf_w, &udpmsg, sizeof(struct udp_msg_ctx),
        pusock->wsabuf_w, MAX_SEND_IOV_COUNT, _udp_data_lens);
    if (0 == uiovcnt)
    {
        _check_sending(pwatcher, pusock);
        return ERR_OK;
    }

    struct msghdr msg;
    _init_msghdr(&msg, &udpmsg.addr, pusock->wsabuf_w, uiovcnt);
    ssize_t isend = sendmsg(pusock->sock.sock, &msg, 0);
    buffer_read_iov_commit(&pusock->buf_w, udpmsg.size);
    if (isend <= 0)
    {
        int32_t irtn = ERRNO;
        if (!ERR_RW_RETRIABLE(irtn))
        {
            return ERR_FAILED;
        }
        return ERR_OK;
    }
    if (NULL != pusock->w_cb)
    {
        pusock->w_cb(&pusock->sock, (size_t)isend, &pusock->ud);
    }

    return ERR_OK;
}
static inline void _on_rw_cb(struct watcher_ctx *pwatcher, struct sock_ctx *psock, uint32_t uiev, int32_t *pstop)
{
    if (psock->flags & _FLAGS_CLOSE)
    {
        return;
    }
    int32_t irtn;
    struct usock_ctx *pusock = _get_usock(psock);
    if (uiev & EV_READ)
    {
        if (SOCK_STREAM == pusock->socktype)
        {
            irtn = _on_recv_cb(pwatcher, pusock);
        }
        else
        {
            irtn = _on_recvfrom_cb(pwatcher, pusock);
        }
        if (ERR_OK != irtn)
        {
            _add_close_qu(pwatcher, psock);
            return;
        }
    }
    if (uiev & EV_WRITE)
    {
        if (SOCK_STREAM == pusock->socktype)
        {
            irtn = _on_send_cb(pwatcher, pusock);
        }
        else
        {
            irtn = _on_sendto_cb(pwatcher, pusock);
        }
        if (ERR_OK != irtn)
        {
            _add_close_qu(pwatcher, psock);
            return;
        }
    }

    _evport_readd(pwatcher, psock);
}
int32_t _uev_add_ref_cmd(struct sock_ctx *psock)
{
    int32_t irtn = ERR_OK;
    if (NULL != psock
        && !(psock->flags & _FLAGS_LSN))
    {
        struct usock_ctx *pusock = _get_usock(psock);
        if (!(psock->flags & _FLAGS_CLOSE))
        {
            ATOMIC_ADD(&pusock->ref_cmd, 1);
        }
        else
        {
            irtn = ERR_FAILED;
        }
    }
    return irtn;
}
void _uev_sub_ref_cmd(struct sock_ctx *psock)
{
    if (NULL != psock
        && !(psock->flags & _FLAGS_LSN))
    {
        struct usock_ctx *pusock = _get_usock(psock);
        ATOMIC_ADD(&pusock->ref_cmd, -1);
    }
}
void _uev_sub_sending(struct sock_ctx *psock)
{
    struct usock_ctx *pusock = _get_usock(psock);
    ATOMIC_ADD(&pusock->sending, -1);
}
static inline int32_t _post_send(struct usock_ctx *pusock)
{
    if (0 != buffer_size(&pusock->buf_w)
        && ATOMIC_CAS(&pusock->sending, 0, 1))
    {
        _netev_add(pusock->watcher, &pusock->sock, EV_WRITE);
    }
    return ERR_OK;
}
int32_t sock_send(struct sock_ctx *psock, void *pdata, const size_t uilens)
{
    if (psock->flags & _FLAGS_CLOSE)
    {
        return ERR_FAILED;
    }

    struct usock_ctx *pusock = _get_usock(psock);
    ASSERTAB(SOCK_STREAM == pusock->socktype, "only support tcp.");
    ASSERTAB(ERR_OK == buffer_append(&pusock->buf_w, pdata, uilens), "buffer_append error.");
    return _post_send(pusock);
}
int32_t sock_send_buffer(struct sock_ctx *psock)
{
    if (psock->flags & _FLAGS_CLOSE)
    {
        return ERR_FAILED;
    }

    struct usock_ctx *pusock = _get_usock(psock);
    ASSERTAB(SOCK_STREAM == pusock->socktype, "only support tcp.");
    return _post_send(pusock);
}
int32_t sock_sendto(struct sock_ctx *psock, const char *phost, uint16_t uport,
    void *pdata, const size_t uilens)
{
    if (psock->flags & _FLAGS_CLOSE)
    {
        return ERR_FAILED;
    }

    struct usock_ctx *pusock = _get_usock(psock);
    ASSERTAB(SOCK_DGRAM == pusock->socktype, "only support tcp.");
    struct udp_msg_ctx msg;
    if (ERR_OK != netaddr_sethost(&msg.addr, phost, uport))
    {
        LOG_ERROR("%s", "netaddr_sethost failed.");
        return ERR_FAILED;
    }
    msg.size = uilens;
    buffer_piece_append(&pusock->buf_w, &msg, sizeof(struct udp_msg_ctx), pdata, uilens);
    return _post_send(pusock);
}
int32_t netev_enable_rw(struct netev_ctx *pctx, struct sock_ctx *psock,
    recv_cb r_cb, send_cb w_cb, close_cb c_cb, struct ud_ctx *pud)
{
    ASSERTAB(NULL != r_cb && NULL != c_cb, ERRSTR_NULLP);
    if (0 != psock->events
        || (psock->flags & _FLAGS_CLOSE))
    {
        return ERR_FAILED;
    }

    struct usock_ctx *pusock = _get_usock(psock);
    pusock->sock.flags = _FLAGS_NORM;
    pusock->sock.ev_cb = _on_rw_cb;
    pusock->c_cb = c_cb;
    pusock->r_cb = r_cb;
    pusock->w_cb = w_cb;
    if (NULL != pud)
    {
        memcpy(&pusock->ud, pud, sizeof(struct ud_ctx));
    }
    pusock->ref_r = 1;
    _netev_add(pusock->watcher, &pusock->sock, EV_READ);

    return ERR_OK;
}
static inline int32_t _sock_err(SOCKET sock)
{
    socklen_t ierr = ERR_OK;
    socklen_t ilens = (socklen_t)sizeof(ierr);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &ierr, &ilens) < ERR_OK)
    {
        ierr = ERR_FAILED;
        LOG_WARN("getsockopt(%d,SOL_SOCKET, SO_ERROR...) failed. %s", sock, ERRORSTR(ERRNO));
    }
    return ierr;
}
static inline void _on_connect_cb(struct watcher_ctx *pwatcher, struct sock_ctx *psock, uint32_t uiev, int32_t *pstop)
{
    if (NULL == _conn_timeout_remove(pwatcher, psock->id))
    {
        return;
    }
    struct usock_ctx *pusock = _get_usock(psock);
    int32_t irtn = _sock_err(pusock->sock.sock);
    if (ERR_OK != irtn)
    {
        _add_close_qu(pwatcher, &pusock->sock);
    }
    else
    {
        pusock->sock.flags = 0;
        pusock->sock.flags |= _FLAGS_NORM;
#ifndef NETEV_EVPORT
        _uev_del(pwatcher, &pusock->sock, pusock->sock.events);
#else
        pusock->sock.events = 0;
#endif
        pusock->conn_cb(&pusock->sock, irtn, &pusock->ud);
        ATOMIC_ADD(&pusock->ref_r, -1);
    }
}
struct sock_ctx *netev_add_sock(struct netev_ctx *pctx, SOCKET sock, int32_t itype, int32_t ifamily)
{
    struct usock_ctx *pusock = MALLOC(sizeof(struct usock_ctx));
    if (NULL == pusock)
    {
        LOG_ERROR("%s", ERRSTR_MEMORY);
        return NULL;
    }

    ZERO(pusock, sizeof(struct usock_ctx));
    socknbio(sock);
    if (SOCK_STREAM == itype)
    {
        closereset(sock);
        socknodelay(sock);
        sockkpa(sock, SOCKKPA_DELAY, SOCKKPA_INTVL);
    }
    pusock->socktype = itype;
    pusock->family = ifamily;
    pusock->sock.id = pctx->id_creater(pctx->id_data);
    pusock->sock.sock = sock;
    pusock->sock.ev_cb = _on_rw_cb;
    pusock->watcher = _netev_get_watcher(pctx, sock);
    buffer_init(&pusock->buf_r);
    buffer_init(&pusock->buf_w);

    return &pusock->sock;
}
struct listener_ctx *netev_listener(struct netev_ctx *pctx,
    const char *phost, const uint16_t usport, accept_cb acp_cb, struct ud_ctx *pud)
{
    ASSERTAB(NULL != acp_cb, ERRSTR_NULLP);
    union netaddr_ctx addr;
    int32_t irn = netaddr_sethost(&addr, phost, usport);
    if (ERR_OK != irn)
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return NULL;
    }
    struct listener_ctx *plsn = MALLOC(sizeof(struct listener_ctx));
    if (NULL == plsn)
    {
        LOG_ERROR("%s", ERRSTR_MEMORY);
        return NULL;
    }
    ZERO(plsn, sizeof(struct listener_ctx));
    plsn->lsncnt = (ERR_OK == checkrport() ? pctx->thcnt : 1);
    plsn->acp_cb = acp_cb;
    if (NULL != pud)
    {
        memcpy(&plsn->ud, pud, sizeof(struct ud_ctx));
    }
    plsn->netev = pctx;
    plsn->family = netaddr_family(&addr);
    plsn->lsn = MALLOC(sizeof(struct lsn_ctx) * plsn->lsncnt);
    if (NULL == plsn->lsn)
    {
        LOG_ERROR("%s", ERRSTR_MEMORY);
        FREE(plsn);
        return NULL;
    }
    int32_t i;
    struct lsn_ctx *plsnctx;
    for (i = 0; i < plsn->lsncnt; i++)
    {
        plsnctx = &plsn->lsn[i];
        plsnctx->listener = plsn;
        plsnctx->sock.id = pctx->id_creater(pctx->id_data);
        plsnctx->sock.events = 0;
        plsnctx->sock.flags = _FLAGS_LSN;
        plsnctx->sock.ev_cb = _on_accept_cb;
        plsnctx->sock.sock = sock_listen(&addr);
        if (INVALID_SOCK == plsnctx->sock.sock)
        {
            _listener_free(plsn);
            return NULL;
        }
    }
    plsn->acpcnt = plsn->lsncnt;
    if (1 == plsn->lsncnt)
    {
        struct watcher_ctx *pwatcher = _netev_get_watcher(pctx, plsn->lsn[0].sock.sock);
        _netev_add(pwatcher, &plsn->lsn[0].sock, EV_READ);
    }
    else
    {
        for (i = 0; i < plsn->lsncnt; i++)
        {
            _netev_add(&pctx->watcher[i], &plsn->lsn[i].sock, EV_READ);
        }
    }
    
    return plsn;
}
static inline void _conn_timeout(struct ud_ctx *pud)
{
    _uev_cmd_timeout((struct watcher_ctx *)pud->handle, pud->id);
}
struct sock_ctx *netev_connecter(struct netev_ctx *pctx, uint32_t utimeout,
    const char *phost, const uint16_t usport, connect_cb conn_cb, struct ud_ctx *pud)
{
    ASSERTAB(NULL != conn_cb, ERRSTR_NULLP);
    union netaddr_ctx addr;
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
    struct usock_ctx *pusock = _get_usock(psock);
    pusock->sock.ev_cb = _on_connect_cb;
    pusock->sock.flags = _FLAGS_CONN;
    pusock->conn_cb = conn_cb;
    if (NULL != pud)
    {
        memcpy(&pusock->ud, pud, sizeof(struct ud_ctx));
    }
    if (ERR_OK == connect(sock, netaddr_addr(&addr), netaddr_size(&addr)))
    {
        pusock->sock.flags = _FLAGS_NORM;
        pusock->conn_cb(&pusock->sock, ERR_OK, &pusock->ud);
        return &pusock->sock;
    }

    irtn = ERRNO;
    if (!ERR_CONNECT_RETRIABLE(irtn))
    {
        LOG_ERROR("%s", ERRORSTR(irtn));
        sock_free(&pusock->sock);
        return NULL;
    }
    pusock->ref_r = 1;
    struct ud_ctx ud;
    ud.id = pusock->sock.id;
    ud.handle = (uintptr_t)pusock->watcher;
    tw_add(pusock->watcher->netev->tw, utimeout, _conn_timeout, &ud);
    _uev_cmd_conn(pusock->watcher, &pusock->sock);

    return &pusock->sock;
}
int32_t sock_type(struct sock_ctx *psock)
{
    return _get_usock(psock)->socktype;
}
void sock_change_uid(struct sock_ctx *psock, sid_t uid)
{
    struct usock_ctx *pusock = _get_usock(psock);
    ATOMIC64_SET(&pusock->ud.id, uid);
}
struct buffer_ctx *sock_buffer_r(struct sock_ctx *psock)
{
    struct usock_ctx *pusock = _get_usock(psock);
    return &pusock->buf_r;
}
struct buffer_ctx *sock_buffer_w(struct sock_ctx *psock)
{
    struct usock_ctx *pusock = _get_usock(psock);
    return &pusock->buf_w;
}

#endif
