#include "netev/netev.h"

#ifndef NETEV_IOCP

#define _FLAGS_LSN   0x01
#define _FLAGS_CONN  0x02
#define _FLAGS_NORM  0x04
//#define _FLAGS_CLOSE 0x08  netev.h中
#define MAX_DELAYFREE_CNT  20
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
    void *udata;
    struct netev_ctx *netev;
    struct lsn_ctx *lsn;
};
struct usock_ctx
{
    struct sock_ctx sock;
    int32_t freecnt;
    int32_t family;
    int32_t socktype;//SOCK_STREAM  SOCK_DGRAM
    volatile atomic_t sending;
    volatile atomic_t ref_tw;
    volatile atomic_t ref_r;
    connect_cb conn_cb;
    read_cb r_cb;
    write_cb w_cb;
    close_cb c_cb;
    void *udata;
    struct netev_ctx *netev;
    struct tw_node_ctx *twconn;
    struct watcher_ctx *watcher;
    struct buffer_ctx buf_r;
    struct buffer_ctx buf_w;
    mutex_ctx mu;
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
        pusock->conn_cb(psock, ERR_FAILED, pusock->udata);
        pusock->ref_r = 0;
    }
    else if (psock->flags & _FLAGS_NORM)
    {
        struct usock_ctx *pusock = _get_usock(psock);
        shutdown(psock->sock, SHUT_WR);
        pusock->c_cb(psock, pusock->udata);
        pusock->ref_r = 0;
    }
}
static inline void _listener_free(void *pdata)
{
    struct listener_ctx *plsn = pdata;
    for (int32_t i = 0; i < plsn->lsncnt; i++)
    {
        SAFE_CLOSE_SOCK(plsn->lsn[i].sock.sock);
    }
    FREE(plsn->lsn);
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
        LOG_WARN("free listener use long time. acpcnt %d", (int32_t)iacpcnt);
        plsn->freecnt = 0;
    }
}
void listener_free(struct listener_ctx *plsn)
{
    //发起关闭
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
        tw_add(plsn->netev->tw, 10, -1, _listener_delay_free, plsn);
    }
}
static inline void _sock_free(void *pdata)
{
    struct usock_ctx *pusock = pdata;
    SAFE_CLOSE_SOCK(pusock->sock.sock);
    buffer_free(&pusock->buf_r);
    buffer_free(&pusock->buf_w);
    mutex_free(&pusock->mu);
    FREE(pusock);
}
static inline void _sock_delay_free(struct tw_node_ctx *ptw, void *pdata)
{
    struct usock_ctx *pusock = pdata;
    pusock->freecnt++;
    if (0 == pusock->ref_r
        && 0 == pusock->ref_tw)
    {
        _sock_free(pusock);
        tw_remove(ptw);
        return;
    }
    if (pusock->freecnt >= MAX_DELAYFREE_CNT)
    {
        LOG_WARN("free socket use long time id %d type %d . ref_r %d  ref_tw %d",
            pusock->sock.id, pusock->socktype, pusock->ref_r, pusock->ref_tw);
        pusock->freecnt = 0;
    }
}
void sock_free(struct sock_ctx *psock)
{
    struct usock_ctx *pusock = _get_usock(psock); 
    SAFE_CLOSE_SOCK(pusock->sock.sock);
    if (0 == pusock->ref_r
        && 0 == pusock->ref_tw)
    {
        _sock_free(pusock);
    }
    else
    {
        tw_add(pusock->netev->tw, 10, -1, _sock_delay_free, pusock);
    }
}
void sock_close(struct sock_ctx *psock)
{
    if (0 == psock->events)
    {
        SAFE_CLOSE_SOCK(psock->sock);
        return;
    }
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
static inline void _on_accept_cb(struct watcher_ctx *pwatcher, struct sock_ctx *psock, int32_t iev, int32_t *pstop)
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

    plsn->listener->acp_cb(pacpsock, plsn->listener->udata);
    _evport_readd(pwatcher, psock);
}
static inline int32_t _check_read(struct usock_ctx *pusock, ssize_t ireaded, uint32_t iiovcnt)
{
    if (0 == ireaded)
    {
        buffer_write_iov_commit(&pusock->buf_r, 0, pusock->wsabuf_r, iiovcnt);
        return ERR_FAILED;
    }
    if (ERR_FAILED == ireaded)
    {
        buffer_write_iov_commit(&pusock->buf_r, 0, pusock->wsabuf_r, iiovcnt);
        int32_t irtn = ERRNO;
        if (!ERR_RW_RETRIABLE(irtn))
        {
            return ERR_FAILED;
        }
        return ERR_OK;
    }

    buffer_write_iov_commit(&pusock->buf_r, (size_t)ireaded, pusock->wsabuf_r, iiovcnt);
    return ERR_OK;
}
static inline int32_t _on_tcp_read(struct watcher_ctx *pwatcher, struct usock_ctx *pusock)
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

    uint32_t iiovcnt = buffer_write_iov_application(&pusock->buf_r, icnt, pusock->wsabuf_r, MAX_RECV_IOV_COUNT);
    ssize_t ireaded = readv(pusock->sock.sock, pusock->wsabuf_r, iiovcnt);
    if (ERR_OK != _check_read(pusock, ireaded, iiovcnt))
    {
        return ERR_FAILED;
    }

    pusock->r_cb(&pusock->sock, &pusock->buf_r, (size_t)ireaded, NULL, 0, pusock->udata);
    return ERR_OK;
}
static inline int32_t _on_tcp_send(struct watcher_ctx *pwatcher, struct usock_ctx *pusock)
{
    uint32_t uicnt = buffer_read_iov_application(&pusock->buf_w, MAX_SEND_IOV_SIZE, 
        pusock->wsabuf_w, MAX_SEND_IOV_COUNT);
    if (0 == uicnt)
    {
        _uev_del(pwatcher, &pusock->sock, EV_WRITE);
        ATOMIC_SET(&pusock->sending, 0);
        return ERR_OK;
    }

    ssize_t isend = writev(pusock->sock.sock, pusock->wsabuf_w, uicnt);
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

    buffer_read_iov_commit(&pusock->buf_w, isend);
    if (NULL != pusock->w_cb)
    {
        pusock->w_cb(&pusock->sock, isend, pusock->udata);
    }
    return ERR_OK;
}
static inline void _init_msghdr(struct msghdr *pmsg, struct netaddr_ctx *paddr, IOV_TYPE *wsabuf, uint32_t iiovcnt)
{
    ZERO(pmsg, sizeof(struct msghdr));
    pmsg->msg_name = netaddr_addr(paddr);
    pmsg->msg_namelen = netaddr_size(paddr);
    pmsg->msg_iov = wsabuf;
    pmsg->msg_iovlen = iiovcnt;
}
static inline int32_t _on_udp_read(struct watcher_ctx *pwatcher, struct usock_ctx *pusock)
{
    int32_t icnt = socknread(pusock->sock.sock);
    if (ERR_FAILED == icnt)
    {
        return ERR_FAILED;
    }

    struct msghdr msg;
    struct netaddr_ctx addr;
    netaddr_empty_addr(&addr, pusock->family);
    uint32_t iiovcnt = buffer_write_iov_application(&pusock->buf_r, icnt, pusock->wsabuf_r, MAX_RECV_IOV_COUNT);
    _init_msghdr(&msg, &addr, pusock->wsabuf_r, iiovcnt);
    ssize_t ireaded = recvmsg(pusock->sock.sock, &msg, 0);
    if (ERR_OK != _check_read(pusock, ireaded, iiovcnt))
    {
        return ERR_FAILED;
    }

    char acip[IP_LENS];
    netaddr_ip(&addr, acip);
    pusock->r_cb(&pusock->sock, &pusock->buf_r, (size_t)ireaded, acip, netaddr_port(&addr), pusock->udata);

    return ERR_OK;
}
static inline int32_t _on_udp_send(struct watcher_ctx *pwatcher, struct usock_ctx *pusock)
{
    return ERR_OK;
}
static inline void _on_rw_cb(struct watcher_ctx *pwatcher, struct sock_ctx *psock, int32_t iev, int32_t *pstop)
{
    int32_t irtn;
    struct usock_ctx *pusock = _get_usock(psock);
    if (iev & EV_READ)
    {
        if (SOCK_STREAM == pusock->socktype)
        {
            irtn = _on_tcp_read(pwatcher, pusock);
        }
        else
        {
            irtn = _on_udp_read(pwatcher, pusock);
        }
        if (ERR_OK != irtn)
        {
            _add_close_qu(pwatcher, psock);
            return;
        }
    }
    if (iev & EV_WRITE)
    {
        if (SOCK_STREAM == pusock->socktype)
        {
            irtn = _on_tcp_send(pwatcher, pusock);
        }
        else
        {
            irtn = _on_udp_send(pwatcher, pusock);
        }
        if (ERR_OK != irtn)
        {
            _add_close_qu(pwatcher, psock);
            return;
        }
    }

    _evport_readd(pwatcher, psock);
}
static inline int32_t _tcp_trysend(struct usock_ctx *pusock)
{
    if (!ATOMIC_CAS(&pusock->sending, 0, 1))
    {
        return ERR_OK;
    }
    _netev_add(pusock->watcher, &pusock->sock, EV_WRITE);
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
    return _tcp_trysend(pusock);
}
int32_t sock_send_buffer(struct sock_ctx *psock)
{
    if (psock->flags & _FLAGS_CLOSE)
    {
        return ERR_FAILED;
    }

    struct usock_ctx *pusock = _get_usock(psock);
    ASSERTAB(SOCK_STREAM == pusock->socktype, "only support tcp.");
    return _tcp_trysend(pusock);
}
int32_t sock_sendto(struct sock_ctx *psock, const char *phost, uint16_t uport,
    void *pdata, const size_t uilens)
{
    return ERR_OK;
}
int32_t netev_enable_rw(struct netev_ctx *pctx, struct sock_ctx *psock,
    read_cb r_cb, write_cb w_cb, close_cb c_cb, void *pdata)
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
    pusock->udata = pdata;
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
static inline void _on_connect_cb(struct watcher_ctx *pwatcher, struct sock_ctx *psock, int32_t iev, int32_t *pstop)
{
    struct usock_ctx *pusock = _get_usock(psock);
    mutex_lock(&pusock->mu);
    if (0 == pusock->ref_tw)
    {
        mutex_unlock(&pusock->mu);
        return;
    }
    pusock->ref_tw = 0;
    tw_remove(pusock->twconn);
    mutex_unlock(&pusock->mu);

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
        ASSERTAB(0 == pusock->sock.events, "can't del all events.");
#else
        pusock->sock.events = 0;
#endif
        pusock->conn_cb(&pusock->sock, irtn, pusock->udata);
        pusock->ref_r = 0;
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
    uint32_t uid = (uint32_t)ATOMIC_ADD(&pctx->id, 1);
    pusock->sock.sock = sock;
    pusock->sock.id = uid;
    pusock->sock.ev_cb = _on_rw_cb;    
    pusock->netev = pctx;
    pusock->watcher = _netev_get_watcher(pctx, sock);
    buffer_init(&pusock->buf_r);
    buffer_init(&pusock->buf_w);
    mutex_init(&pusock->mu);

    return &pusock->sock;
}
struct listener_ctx *netev_listener(struct netev_ctx *pctx,
    const char *phost, const uint16_t usport, accept_cb acp_cb, void *pdata)
{
    ASSERTAB(NULL != acp_cb, ERRSTR_NULLP);
    struct netaddr_ctx addr;
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
    plsn->udata = pdata;
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
        plsnctx->sock.id = (uint32_t)ATOMIC_ADD(&pctx->id, 1);
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
static inline void _conn_timeout(struct tw_node_ctx *pnode, void *pdata)
{
    struct usock_ctx *pusock = pdata;
    //执行了connect
    mutex_lock(&pusock->mu);
    if (0 == pusock->ref_tw)
    {
        mutex_unlock(&pusock->mu);
        return;
    }
    pusock->ref_tw = 0;
    mutex_unlock(&pusock->mu);

    _uev_cmd_close(pusock->watcher, &pusock->sock);
    struct netaddr_ctx addr;
    if (ERR_OK == netaddr_remoteaddr(&addr, pusock->sock.sock, pusock->family))
    {
        char acip[IP_LENS];
        netaddr_ip(&addr, acip);
        LOG_WARN("sock %d connect %s %d time out.", pusock->sock.sock, acip, netaddr_port(&addr));
    }
    else
    {
        LOG_WARN("sock %d connect time out.", pusock->sock.sock);
    }
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
    struct usock_ctx *pusock = _get_usock(psock);
    pusock->sock.ev_cb = _on_connect_cb;
    pusock->sock.flags = _FLAGS_CONN;
    pusock->conn_cb = conn_cb;
    pusock->udata = pdata;
    if (ERR_OK == connect(sock, netaddr_addr(&addr), netaddr_size(&addr)))
    {
        pusock->sock.flags = _FLAGS_NORM;
        pusock->conn_cb(&pusock->sock, ERR_OK, pusock->udata);
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
    pusock->ref_tw = 1;
    pusock->twconn = tw_add(pusock->netev->tw, utimeout, 1, _conn_timeout, pusock);
    _netev_add(pusock->watcher, &pusock->sock, EV_WRITE);

    return &pusock->sock;
}
int32_t sock_type(struct sock_ctx *psock)
{
    return _get_usock(psock)->socktype;
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
