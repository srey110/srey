#include "event/uev.h"
#include "buffer.h"
#include "netutils.h"
#include "loger.h"
#include "hashmap.h"

#ifndef EV_IOCP

typedef struct lsnsock_ctx
{
    sock_ctx sock;
    struct listener_ctx *lsn;
}lsnsock_ctx;
typedef struct listener_ctx
{
    int32_t family;
    int32_t nlsn;
    lsnsock_ctx *lsnsock;
#if WITH_SSL
    evssl_ctx *evssl;
#endif
    cbs_ctx cbs;
    ud_cxt ud;
#ifndef SO_REUSEPORT
    mutex_ctx lsnlck;
#endif
}listener_ctx;
typedef struct skrw_ctx
{
    sock_ctx sock;
#if WITH_SSL
    int32_t server;
    int32_t handshake;
    SSL *ssl;
#endif
    buffer_ctx buf_r;
    qu_bufs buf_s;
    cbs_ctx cbs;
    ud_cxt ud;
}skrw_ctx;
typedef struct udp_ctx
{
    sock_ctx sock;
    recvfrom_cb rf_cb;
    buffer_ctx buf_r;
    qu_bufs buf_s;
    netaddr_ctx addr;
    ud_cxt ud;
}udp_ctx;

static void _on_rw_cb(watcher_ctx *watcher, sock_ctx *skctx, int32_t ev, int32_t *stop);

void _sk_shutdown(sock_ctx *skctx)
{
#if WITH_SSL
    skrw_ctx *skrw = UPCAST(skctx, skrw_ctx, sock);
    evssl_shutdown(skrw->ssl, skrw->sock.fd);
#else
    shutdown(skctx->fd, SHUT_RD);
#endif
}
sock_ctx *_new_sk(SOCKET fd, cbs_ctx *cbs, ud_cxt *ud)
{
    skrw_ctx *skrw;
    MALLOC(skrw, sizeof(skrw_ctx));
    skrw->sock.ev_cb = _on_rw_cb;
    skrw->sock.type = SOCK_STREAM;
    skrw->sock.fd = fd;
    skrw->sock.events = 0;
#if WITH_SSL
    skrw->ssl = NULL;
#endif
    skrw->cbs = *cbs;
    COPY_UD(skrw->ud, ud);
    buffer_init(&skrw->buf_r);
    qu_bufs_init(&skrw->buf_s, INIT_SENDBUF_LEN);
    return &skrw->sock;
}
void _free_sk(sock_ctx *skctx)
{
    skrw_ctx *skrw = UPCAST(skctx, skrw_ctx, sock);
#if WITH_SSL
    FREE_SSL(skrw->ssl);
#endif
    CLOSE_SOCK(skrw->sock.fd);
    buffer_free(&skrw->buf_r);
    _bufs_clear(&skrw->buf_s);
    qu_bufs_free(&skrw->buf_s);
    FREE(skrw);
}
void _clear_sk(sock_ctx *skctx)
{
    skrw_ctx *skrw = UPCAST(skctx, skrw_ctx, sock);
    skrw->sock.events = 0;
#if WITH_SSL
    FREE_SSL(skrw->ssl);
#endif
    CLOSE_SOCK(skrw->sock.fd);
    _bufs_clear(&skrw->buf_s);
    buffer_drain(&skrw->buf_r, buffer_size(&skrw->buf_r));
}
void _reset_sk(sock_ctx *skctx, SOCKET fd, cbs_ctx *cbs, ud_cxt *ud)
{
    skrw_ctx *skrw = UPCAST(skctx, skrw_ctx, sock);
    skrw->sock.fd = fd;
    skrw->cbs = *cbs;
    COPY_UD(skrw->ud, ud);
}
void _add_fd(watcher_ctx *watcher, sock_ctx *skctx)
{
    map_element el;
    el.fd =skctx->fd;
    el.sock = skctx;
    sock_ctx *old = hashmap_set(watcher->element, &el);
    if (NULL != old)
    {
        if (SOCK_STREAM == old->type)
        {
            skrw_ctx *skrw = UPCAST(old, skrw_ctx, sock);
            if (NULL != skrw->cbs.c_cb)
            {
                int32_t handshake = 1;
#if WITH_SSL
                if (NULL != skrw->ssl)
                {
                    handshake = skrw->handshake;
                }
#endif
                if (handshake)
                {
                    skrw->cbs.c_cb(watcher->ev, skrw->sock.fd, &skrw->ud);
                }
            }
            skrw->sock.fd = INVALID_SOCK;
            pool_push(&watcher->pool, old);
        }
        else if (SOCK_DGRAM == old->type)
        {
            udp_ctx *udp = UPCAST(old, udp_ctx, sock);
            udp->sock.fd = INVALID_SOCK;
            _free_udp(old);
        }
        LOG_WARN("socket %d repeat.", (int32_t)skctx->fd);
    }
}
static inline void _remove_fd(watcher_ctx *watcher, SOCKET fd)
{
    map_element key;
    key.fd = fd;
    hashmap_delete(watcher->element, &key);
}
//rw
#if WITH_SSL
static inline int32_t _ssl_handshake_acpt(watcher_ctx *watcher, skrw_ctx *skrw)
{
    int32_t rtn = evssl_tryacpt(skrw->ssl);
    if (ERR_FAILED == rtn)
    {
#ifdef EV_POLLSET
        _del_event(watcher, skrw->sock.fd, &skrw->sock.events, skrw->sock.events, &skrw->sock);
#endif
        _remove_fd(watcher, skrw->sock.fd);
        pool_push(&watcher->pool, &skrw->sock);
        return ERR_FAILED;
    }
    if (1 == rtn)
    {
        if (ERR_OK != skrw->cbs.acp_cb(watcher->ev, skrw->sock.fd, &skrw->ud))
        {
#ifdef EV_POLLSET
            _del_event(watcher, skrw->sock.fd, &skrw->sock.events, skrw->sock.events, &skrw->sock);
#endif
            _remove_fd(watcher, skrw->sock.fd);
            pool_push(&watcher->pool, &skrw->sock);
            return ERR_FAILED;
        }
        skrw->handshake = 1;
        return ERR_OK;
    }
    //0
#ifdef EV_EVPORT
    if (ERR_OK != _add_event(watcher, skrw->sock.fd, &skrw->sock.events, EVENT_READ, &skrw->sock))
    {
        _remove_fd(watcher, skrw->sock.fd);
        pool_push(&watcher->pool, &skrw->sock);
    }
#endif
    return ERR_FAILED;
}
static inline int32_t _ssl_handshake_conn(watcher_ctx *watcher, skrw_ctx *skrw)
{
    int32_t rtn = evssl_tryconn(skrw->ssl);
    if (ERR_FAILED == rtn)
    {
        skrw->cbs.conn_cb(watcher->ev, INVALID_SOCK, &skrw->ud);
#ifdef EV_POLLSET
        _del_event(watcher, skrw->sock.fd, &skrw->sock.events, skrw->sock.events, &skrw->sock);
#endif
        _remove_fd(watcher, skrw->sock.fd);
        pool_push(&watcher->pool, &skrw->sock);
        return ERR_FAILED;
    }
    if (1 == rtn)
    {
        if (ERR_OK != skrw->cbs.conn_cb(watcher->ev, skrw->sock.fd, &skrw->ud))
        {
#ifdef EV_POLLSET
            _del_event(watcher, skrw->sock.fd, &skrw->sock.events, skrw->sock.events, &skrw->sock);
#endif
            _remove_fd(watcher, skrw->sock.fd);
            pool_push(&watcher->pool, &skrw->sock);
            return ERR_FAILED;
        }
        skrw->handshake = 1;
        return ERR_OK;
    }
    //0
#ifdef EV_EVPORT
    if (ERR_OK != _add_event(watcher, skrw->sock.fd, &skrw->sock.events, EVENT_READ, &skrw->sock))
    {
        skrw->cbs.conn_cb(watcher->ev, INVALID_SOCK, &skrw->ud);
        _remove_fd(watcher, skrw->sock.fd);
        pool_push(&watcher->pool, &skrw->sock);
    }
#endif
    return ERR_FAILED;
}
static inline int32_t _ssl_handshake(watcher_ctx *watcher, skrw_ctx *skrw)
{
    if (skrw->server)
    {
        return _ssl_handshake_acpt(watcher, skrw);
    }
    return _ssl_handshake_conn(watcher, skrw);
}
#endif
static inline int32_t _tcp_recv(watcher_ctx *watcher, skrw_ctx *skrw)
{
    size_t nread;
#if WITH_SSL
    int32_t rtn = buffer_from_sock(&skrw->buf_r, skrw->sock.fd, &nread, _sock_read, skrw->ssl);
#else
    int32_t rtn = buffer_from_sock(&skrw->buf_r, skrw->sock.fd, &nread, _sock_read, NULL);
#endif
    if (0 != nread)
    {
        skrw->cbs.r_cb(watcher->ev, skrw->sock.fd, &skrw->buf_r, nread, &skrw->ud);
    }
#ifdef EV_EVPORT
    if (ERR_OK == rtn)
    {
        rtn = _add_event(watcher, skrw->sock.fd, &skrw->sock.events, EVENT_READ, &skrw->sock);
    }
#endif
    if (ERR_OK != rtn)
    {
        if (NULL != skrw->cbs.c_cb)
        {
            skrw->cbs.c_cb(watcher->ev, skrw->sock.fd, &skrw->ud);
        }
#ifdef EV_POLLSET
        _del_event(watcher, skrw->sock.fd, &skrw->sock.events, skrw->sock.events, &skrw->sock);
#endif
        _remove_fd(watcher, skrw->sock.fd);
        pool_push(&watcher->pool, &skrw->sock);
    }
    return rtn;
}
static inline int32_t _on_r_cb(watcher_ctx *watcher, skrw_ctx *skrw)
{
#if WITH_SSL
    if (NULL != skrw->ssl
        && !skrw->handshake)
    {
        if (ERR_OK != _ssl_handshake(watcher, skrw))
        {
            return ERR_FAILED;
        }
    }
#endif
    return _tcp_recv(watcher, skrw);
}
static inline void _on_w_cb(watcher_ctx *watcher, skrw_ctx *skrw)
{
    size_t nsend;
#if WITH_SSL
    _sock_send(skrw->sock.fd, &skrw->buf_s, NULL, &nsend, skrw->ssl);
#else
    _sock_send(skrw->sock.fd, &skrw->buf_s, NULL, &nsend, NULL);
#endif
    if (NULL != skrw->cbs.s_cb
        && 0 != nsend)
    {
        skrw->cbs.s_cb(watcher->ev, skrw->sock.fd, nsend, &skrw->ud);
    }
    if (0 == qu_bufs_size(&skrw->buf_s))
    {
        _del_event(watcher, skrw->sock.fd, &skrw->sock.events, EVENT_WRITE, &skrw->sock);
        return;
    }
#ifdef EV_EVPORT
    _add_event(watcher, skrw->sock.fd, &skrw->sock.events, EVENT_WRITE, &skrw->sock);
#endif
}
static void _on_rw_cb(watcher_ctx *watcher, sock_ctx *skctx, int32_t ev, int32_t *stop)
{
    skrw_ctx *skrw = UPCAST(skctx, skrw_ctx, sock);
    if (ev & EVENT_READ)
    {
        if (ERR_OK != _on_r_cb(watcher, skrw))
        {
            return;
        }
    }
    if (ev & EVENT_WRITE)
    {
        _on_w_cb(watcher, skrw);
    }
}
void _add_write_inloop(watcher_ctx *watcher, sock_ctx *skctx, bufs_ctx *buf)
{
    qu_bufs_push((SOCK_STREAM == skctx->type) ?
                  &UPCAST(skctx, skrw_ctx, sock)->buf_s :
                  &UPCAST(skctx, udp_ctx, sock)->buf_s, 
                  buf);
    if (!(skctx->events & EVENT_WRITE))
    {
        _add_event(watcher, skctx->fd, &skctx->events, EVENT_WRITE, skctx);
    }
}
//connect
static inline void _on_connect_cb(watcher_ctx *watcher, sock_ctx *skctx, int32_t ev, int32_t *stop)
{
    skrw_ctx *connsk = UPCAST(skctx, skrw_ctx, sock);
    connsk->sock.ev_cb = _on_rw_cb;
    _del_event(watcher, connsk->sock.fd, &connsk->sock.events, connsk->sock.events, NULL);
    if (ERR_OK != sock_checkconn(connsk->sock.fd))
    {
        connsk->cbs.conn_cb(watcher->ev, INVALID_SOCK, &connsk->ud);
        _remove_fd(watcher, connsk->sock.fd);
        pool_push(&watcher->pool, &connsk->sock);
        return;
    }
    int32_t handshake = 1;
#if WITH_SSL
    if (NULL != connsk->ssl)
    {
        connsk->server = 0;
        int32_t rtn = evssl_tryconn(connsk->ssl);
        if (ERR_OK == rtn)
        {
            handshake = 0;
            connsk->handshake = 0;
        }
        else if (1 == rtn)
        {
            connsk->handshake = 1;
        }
        else//-1
        {
            connsk->cbs.conn_cb(watcher->ev, INVALID_SOCK, &connsk->ud);
            _remove_fd(watcher, connsk->sock.fd);
            pool_push(&watcher->pool, &connsk->sock);
            return;
        }
    }
#endif
    if (handshake)
    {
        if (ERR_OK != connsk->cbs.conn_cb(watcher->ev, connsk->sock.fd, &connsk->ud))
        {
            _remove_fd(watcher, connsk->sock.fd);
            pool_push(&watcher->pool, &connsk->sock);
            return;
        }
    }
    if (ERR_OK != _add_event(watcher, connsk->sock.fd, &connsk->sock.events, EVENT_READ, &connsk->sock))
    {
        if (handshake)
        {
            if (NULL != connsk->cbs.c_cb)
            {
                connsk->cbs.c_cb(watcher->ev, connsk->sock.fd, &connsk->ud);
            }
        }
        else
        {
            connsk->cbs.conn_cb(watcher->ev, INVALID_SOCK, &connsk->ud);
        }
        _remove_fd(watcher, connsk->sock.fd);
        pool_push(&watcher->pool, &connsk->sock);
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
    int32_t rtn = connect(fd, netaddr_addr(&addr), netaddr_size(&addr));
    if (ERR_OK != rtn)
    {
        rtn = ERRNO;
        if (!ERR_CONNECT_RETRIABLE(rtn))
        {
            CLOSE_SOCK(fd);
            return INVALID_SOCK;
        }
    }
    sock_ctx *skctx = _new_sk(fd, cbs, ud);
    skctx->ev_cb = _on_connect_cb;
#if WITH_SSL
    if (NULL != evssl)
    {
        skrw_ctx *skrw = UPCAST(skctx, skrw_ctx, sock);
        skrw->ssl = evssl_setfd(evssl, fd);
        if (NULL == skrw->ssl)
        {
            _free_sk(skctx);
            return INVALID_SOCK;
        }
    }
#endif
    _cmd_connect(ctx, fd, skctx);
    return fd;
}
void _add_conn_inloop(watcher_ctx *watcher, SOCKET fd, sock_ctx *skctx)
{
    _add_fd(watcher, skctx);
    if(ERR_OK != _add_event(watcher, fd, &skctx->events, EVENT_WRITE, skctx))
    {
        skrw_ctx *skrw = UPCAST(skctx, skrw_ctx, sock);
        skrw->cbs.conn_cb(watcher->ev, INVALID_SOCK, &skrw->ud);
        skctx->ev_cb = _on_rw_cb;
        _remove_fd(watcher, fd);
        pool_push(&watcher->pool, skctx);
    }
}
//listen
static inline void _on_accept_cb(watcher_ctx *watcher, sock_ctx *skctx, int32_t ev, int32_t *stop)
{
    lsnsock_ctx *acpt = UPCAST(skctx, lsnsock_ctx, sock);
#ifndef SO_REUSEPORT
    if (ERR_OK != mutex_trylock(&acpt->lsn->lsnlck))
    {
        return;
    }
#endif
    SOCKET fd;
    uint64_t hs;
    watcher_ctx *to;
    while (INVALID_SOCK != (fd = accept(acpt->sock.fd, NULL, NULL)))
    {
        if (ERR_OK != _set_sockops(fd))
        {
            CLOSE_SOCK(fd);
            continue;
        }
        hs = FD_HASH(fd);
        to = GET_PTR(watcher->ev->watcher, watcher->ev->nthreads, hs);
        if (to->index == watcher->index)
        {
            _add_acpfd_inloop(to, fd, acpt->lsn);
        }
        else
        {
            _cmd_add_acpfd(to, hs, fd, acpt->lsn);
        }
    }
#ifndef SO_REUSEPORT
    mutex_unlock(&acpt->lsn->lsnlck);
#endif
#ifdef EV_EVPORT
    _add_event(watcher, acpt->sock.fd, &acpt->sock.events, ev, &acpt->sock);
#endif
}
void _add_acpfd_inloop(watcher_ctx *watcher, SOCKET fd, listener_ctx *lsn)
{
    int32_t handshake = 1;
    sock_ctx *skctx = pool_pop(&watcher->pool, fd, &lsn->cbs, &lsn->ud);
#if WITH_SSL
    if (NULL != lsn->evssl)
    {
        skrw_ctx *acpsk = UPCAST(skctx, skrw_ctx, sock);
        acpsk->ssl = evssl_setfd(lsn->evssl, fd);
        if (NULL == acpsk->ssl)
        {
            pool_push(&watcher->pool, skctx);
            return;
        }
        handshake = 0;
        acpsk->handshake = 0;
        acpsk->server = 1;
    }
#endif
    _add_fd(watcher, skctx);
    if (handshake)
    {
        if (ERR_OK != lsn->cbs.acp_cb(watcher->ev, fd, &lsn->ud))
        {
            _remove_fd(watcher, fd);
            pool_push(&watcher->pool, skctx);
            return;
        }
    }
    if (ERR_OK != _add_event(watcher, fd, &skctx->events, EVENT_READ, skctx))
    {
        if (NULL != lsn->cbs.c_cb
            && handshake)
        {
            lsn->cbs.c_cb(watcher->ev, fd, &lsn->ud);
        }
        _remove_fd(watcher, fd);
        pool_push(&watcher->pool, skctx);
    }
}
static inline void _close_lsnsock(listener_ctx *lsn, int32_t cnt)
{
#ifndef SO_REUSEPORT
    CLOSE_SOCK(lsn->lsnsock[0].sock.fd);
#else
    for (int32_t i = 0; i < cnt; i++)
    {
        CLOSE_SOCK(lsn->lsnsock[i].sock.fd);
    }
#endif
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
#ifndef SO_REUSEPORT
    SOCKET fd = _listen(&addr);
    if (INVALID_SOCK == fd)
    {
        return ERR_FAILED;
    }
#endif
    listener_ctx *lsn;
    MALLOC(lsn, sizeof(listener_ctx));
    lsn->family = netaddr_family(&addr);
    lsn->nlsn = ctx->nthreads;
    lsn->cbs = *cbs;
    COPY_UD(lsn->ud, ud);
#if WITH_SSL
    lsn->evssl = evssl;
#endif
#ifndef SO_REUSEPORT
    mutex_init(&lsn->lsnlck);
#endif
    MALLOC(lsn->lsnsock, sizeof(lsnsock_ctx) * lsn->nlsn);
    int32_t i;
    lsnsock_ctx *lsnsock;
    for (i = 0; i < lsn->nlsn; i++)
    {
        lsnsock = &lsn->lsnsock[i];
        lsnsock->lsn = lsn;
        lsnsock->sock.type = 0;
        lsnsock->sock.events = 0;
        lsnsock->sock.ev_cb = _on_accept_cb;
#ifndef SO_REUSEPORT
        lsnsock->sock.fd = fd;
#else
        lsnsock->sock.fd = _listen(&addr);
        if (INVALID_SOCK == lsnsock->sock.fd)
        {
            _close_lsnsock(lsn, i);
            FREE(lsn->lsnsock);
            FREE(lsn);
            return ERR_FAILED;
        }
#endif
    }
    mutex_lock(&ctx->qulsnlck);
    qu_lsn_push(&ctx->qulsn, &lsn);
    mutex_unlock(&ctx->qulsnlck);
    for (i = 0; i < lsn->nlsn; i++)
    {
        _cmd_listen(&ctx->watcher[i], lsn->lsnsock[i].sock.fd, &lsn->lsnsock[i].sock);
    }
    return ERR_OK;
}
void _add_lsn_inloop(watcher_ctx *watcher, SOCKET fd, sock_ctx *skctx)
{
    _add_fd(watcher, skctx);
    if (ERR_OK != _add_event(watcher, fd, &skctx->events, EVENT_READ, skctx))
    {
        _remove_fd(watcher, fd);
        LOG_WARN("%s", "add listen socket in loop error.");
    }
}
void _freelsn(listener_ctx *lsn)
{
    _close_lsnsock(lsn, lsn->nlsn);
#ifndef SO_REUSEPORT
    mutex_free(&lsn->lsnlck);
#endif
    FREE(lsn->lsnsock);
    FREE(lsn);
}
//UDP
void _udp_close(watcher_ctx *watcher, sock_ctx *skctx)
{
    map_element key;
    key.fd = skctx->fd;
    if (NULL != hashmap_delete(watcher->element, &key))
    {
#ifdef EV_POLLSET
        _del_event(watcher, skctx->fd, &skctx->events, skctx->events, skctx);
#endif
        qu_sock_push(&watcher->qu_udpfree, &skctx);
    }
}
static inline void _init_msghdr(struct msghdr *msg, netaddr_ctx *addr, IOV_TYPE *iov, uint32_t niov)
{
    ZERO(msg, sizeof(struct msghdr));
    msg->msg_name = netaddr_addr(addr);
    msg->msg_namelen = netaddr_size(addr);
    msg->msg_iov = iov;
    msg->msg_iovlen = niov;
}
static inline int32_t _on_udp_rcb(watcher_ctx *watcher, udp_ctx *udp)
{
    struct msghdr msg;
    IOV_TYPE iov[MAX_EXPAND_NIOV];
    uint32_t niov = buffer_expand(&udp->buf_r, MAX_RECVFROM_SIZE, iov, MAX_EXPAND_NIOV);
    _init_msghdr(&msg, &udp->addr, iov, niov);
    int32_t rtn = (int32_t)recvmsg(udp->sock.fd, &msg, 0);
    if (rtn > 0)
    {
        buffer_commit_expand(&udp->buf_r, (size_t)rtn, iov, niov);
        udp->rf_cb(watcher->ev, udp->sock.fd, &udp->buf_r, (size_t)rtn, &udp->addr, &udp->ud);
        rtn = ERR_OK;
    }
    else
    {
        if (0 == rtn)
        {
            rtn = ERR_FAILED;
        }
        else//< 0
        {
            if (ERR_RW_RETRIABLE(ERRNO))
            {
                rtn = ERR_OK;
                buffer_commit_expand(&udp->buf_r, 0, iov, niov);
            }
        }
    }
#ifdef EV_EVPORT
    if (ERR_OK == rtn)
    {
        rtn = _add_event(watcher, udp->sock.fd, &udp->sock.events, EVENT_READ, &udp->sock);
    }
#endif
    if (ERR_OK != rtn)
    {
        _udp_close(watcher, &udp->sock);
    }
    return rtn;
}
static inline void _on_udp_wcb(watcher_ctx *watcher, udp_ctx *udp)
{
    int32_t rtn;
    bufs_ctx *buf;
    netaddr_ctx *addr;
    IOV_TYPE iov;
    struct msghdr msg;
    while (NULL != (buf = qu_bufs_pop(&udp->buf_s)))
    {
        addr = (netaddr_ctx *)buf->data;
        iov.IOV_PTR_FIELD = (char *)buf->data + sizeof(netaddr_ctx);
        iov.IOV_LEN_FIELD = (IOV_LEN_TYPE)buf->len;
        _init_msghdr(&msg, addr, &iov, 1);
        rtn = sendmsg(udp->sock.fd, &msg, 0);
        FREE(buf->data);
        if (rtn <= 0)
        {
            break;
        }
    }
    if (0 == qu_bufs_size(&udp->buf_s))
    {
        _del_event(watcher, udp->sock.fd, &udp->sock.events, EVENT_WRITE, &udp->sock);
        return;
    }
#ifdef EV_EVPORT
    _add_event(watcher, udp->sock.fd, &udp->sock.events, EVENT_WRITE, &udp->sock);
#endif
}
static void _on_udp_rw(watcher_ctx *watcher, sock_ctx *skctx, int32_t ev, int32_t *stop)
{
    udp_ctx *udp = UPCAST(skctx, udp_ctx, sock);
    if (ev & EVENT_READ)
    {
        if (ERR_OK != _on_udp_rcb(watcher, udp))
        {
            return;
        }
    }
    if (ev & EVENT_WRITE)
    {
        _on_udp_wcb(watcher, udp);
    }
}
static inline sock_ctx *_new_udp(SOCKET fd, int32_t family, recvfrom_cb rf_cb, ud_cxt *ud)
{
    udp_ctx *udp;
    MALLOC(udp, sizeof(udp_ctx));
    udp->sock.ev_cb = _on_udp_rw;
    udp->sock.type = SOCK_DGRAM;
    udp->sock.fd = fd;
    udp->sock.events = 0;
    udp->rf_cb = rf_cb;
    COPY_UD(udp->ud, ud);
    netaddr_empty_addr(&udp->addr, family);
    buffer_init(&udp->buf_r);
    qu_bufs_init(&udp->buf_s, INIT_SENDBUF_LEN);
    return &udp->sock;
}
void _free_udp(sock_ctx *skctx)
{
    udp_ctx *udp = UPCAST(skctx, udp_ctx, sock);
    CLOSE_SOCK(udp->sock.fd);
    buffer_free(&udp->buf_r);
    _bufs_clear(&udp->buf_s);
    qu_bufs_free(&udp->buf_s);
    FREE(udp);
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
    sock_ctx *skctx = _new_udp(fd, netaddr_family(&addr), rf_cb, ud);
    _cmd_add_udp(ctx, fd, skctx);
    return fd;
}
void _add_udp_inloop(watcher_ctx *watcher, SOCKET fd, sock_ctx *skctx)
{
    _add_fd(watcher, skctx);
    if (ERR_OK != _add_event(watcher, fd, &skctx->events, EVENT_READ, skctx))
    {
        _remove_fd(watcher, fd);
        _free_udp(skctx);
        return;
    }
}

#endif//EV_IOCP
