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
    freeud_cb fud_cb;
    lsnsock_ctx *lsnsock;
    lsn_cb_ctx lsn_cb;    
    ud_cxt ud;
#ifndef SO_REUSEPORT
    mutex_ctx lsnlck;
#endif
}listener_ctx;
typedef struct conn_ctx
{
    sock_ctx sock;
    conn_cb_ctx conn_cb;
    ud_cxt ud;
}conn_ctx;
typedef struct skrw_ctx
{
    sock_ctx sock;
    buffer_ctx buf;
    qu_bufs qubuf;
    rw_cb_ctx rw_cb;
    ud_cxt ud;
}skrw_ctx;

qu_bufs *_get_send_buf(sock_ctx *skctx)
{
    skrw_ctx *skrw = UPCAST(skctx, skrw_ctx, sock);
    return &skrw->qubuf;
}
connect_cb _get_conn_cb(sock_ctx *skctx, ud_cxt *ud)
{
    conn_ctx *conn = UPCAST(skctx, conn_ctx, sock);
    *ud = conn->ud;
    return conn->conn_cb.conn_cb;
}
void _update_ud(sock_ctx *skctx, ud_cxt *ud)
{
    skrw_ctx *skrw = UPCAST(skctx, skrw_ctx, sock);
    skrw->ud = *ud;
}
void _free_sockctx(sock_ctx *skctx)
{
    skrw_ctx *skrw = UPCAST(skctx, skrw_ctx, sock);
    CLOSE_SOCK(skrw->sock.fd);
    buffer_free(&skrw->buf);
    _qu_bufs_clear(&skrw->qubuf);
    qu_bufs_free(&skrw->qubuf);
    FREE(skrw);
}
void _on_close(watcher_ctx *watcher, sock_ctx *skctx, int32_t remove)
{
    skrw_ctx *skrw = UPCAST(skctx, skrw_ctx, sock);
    if (NULL != skrw->rw_cb.c_cb)
    {
        skrw->rw_cb.c_cb(watcher->ev, skrw->sock.fd, &skrw->ud);
    }
    if (remove)
    {
        map_element key;
        key.fd = skrw->sock.fd;
        hashmap_delete(watcher->element, &key);
    }
    _free_sockctx(skctx);
}
static inline int32_t _on_r_cb(watcher_ctx *watcher, skrw_ctx *skrw)
{
    size_t nread;
    //������ǰ���߼�⡣����
    int32_t rtn = buffer_from_sock(&skrw->buf, skrw->sock.fd, &nread, _sock_read, NULL);
    if (0 != nread)
    {
        skrw->rw_cb.r_cb(watcher->ev, skrw->sock.fd, &skrw->buf, nread, &skrw->ud);
    }
#ifdef EV_EVPORT
    if (ERR_OK == rtn)
    {
        rtn = _add_event(watcher, skrw->sock.fd, &skrw->sock.events, EVENT_READ, &skrw->sock);
    }
#endif
    if (ERR_OK != rtn)
    {
        _on_close(watcher, &skrw->sock, 1);
    }
    return rtn;
}
static inline void _on_w_cb(watcher_ctx *watcher, skrw_ctx *skrw)
{
    size_t nsend;
    int32_t rtn = _sock_send(skrw->sock.fd, &skrw->qubuf, &nsend, NULL);
    if (NULL != skrw->rw_cb.s_cb
        && 0 != nsend)
    {
        skrw->rw_cb.s_cb(watcher->ev, skrw->sock.fd, nsend, &skrw->ud);
    }
    if (ERR_OK != rtn)
    {
        return;
    }
    if (0 == qu_bufs_size(&skrw->qubuf))
    {
        _del_event(watcher, skrw->sock.fd, &skrw->sock.events, EVENT_WRITE, &skrw->sock);
        return;
    }
#ifdef EV_EVPORT
    _add_event(watcher, skrw->sock.fd, &skrw->sock.events, EVENT_WRITE, &skrw->sock);
#endif
}
static inline void _on_rw_cb(watcher_ctx *watcher, sock_ctx *skctx, int32_t ev, int32_t *stop)
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
static inline sock_ctx *_new_sockctx(SOCKET fd, rw_cb_ctx *cbs, ud_cxt *ud)
{
    skrw_ctx *rw;
    MALLOC(rw, sizeof(skrw_ctx));
    rw->sock.ev_cb = _on_rw_cb;
    rw->sock.fd = fd;
    rw->sock.events = 0;
    rw->ud = *ud;
    rw->rw_cb = *cbs;
    buffer_init(&rw->buf);
    qu_bufs_init(&rw->qubuf, INIT_SENDBUF_LEN);
    return &rw->sock;
}
static inline void _on_connect_cb(watcher_ctx *watcher, sock_ctx *skctx, int32_t ev, int32_t *stop)
{
    conn_ctx *conn = UPCAST(skctx, conn_ctx, sock);
    if (ERR_OK != sock_checkconn(conn->sock.fd))
    {
        conn->conn_cb.conn_cb(watcher->ev, INVALID_SOCK, &conn->ud);
        CLOSE_SOCK(conn->sock.fd);
        FREE(conn);
        return;
    }
    _del_event(watcher, conn->sock.fd, &conn->sock.events, ev, NULL);
    ASSERTAB(0 == conn->sock.events, "logic error.");
    if (ERR_OK != conn->conn_cb.conn_cb(watcher->ev, conn->sock.fd, &conn->ud))
    {
        CLOSE_SOCK(conn->sock.fd);
        FREE(conn);
        return;
    }
    sock_ctx *rwck = _new_sockctx(conn->sock.fd, &conn->conn_cb.rw_cb, &conn->ud);
    if (ERR_OK != _add_event(watcher, rwck->fd, &rwck->events, EVENT_READ, rwck))
    {
        _on_close(watcher, rwck, 0);
    }
    else
    {
        map_element el;
        el.fd = rwck->fd;
        el.sock = rwck;
        ASSERTAB(NULL == hashmap_set(watcher->element, &el), "socket repeat.");
    }
    FREE(conn);
}
int32_t ev_connect(ev_ctx *ctx, const char *host, const uint16_t port,
    conn_cb_ctx *cbs, ud_cxt *ud)
{
    ASSERTAB(NULL != cbs && NULL != cbs->rw_cb.r_cb, ERRSTR_NULLP);
    netaddr_ctx addr;
    if (ERR_OK != netaddr_sethost(&addr, host, port))
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    SOCKET fd = _create_sock(netaddr_family(&addr));
    if (INVALID_SOCK == fd)
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
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
            LOG_ERROR("%s", ERRORSTR(rtn));
            return ERR_FAILED;
        }
    }
    conn_ctx *conn;
    MALLOC(conn, sizeof(conn_ctx));
    conn->sock.ev_cb = _on_connect_cb;
    conn->sock.events = 0;
    conn->sock.fd = fd;
    conn->conn_cb = *cbs;
    COPY_UD(conn->ud, ud);
    _cmd_connect(ctx, fd, &conn->sock);
    return ERR_OK;
}
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
    while (INVALID_SOCK != (fd = accept(acpt->sock.fd, NULL, NULL)))
    {
        if (ERR_OK != _set_sockops(fd)
            || ERR_OK != acpt->lsn->lsn_cb.acp_cb(watcher->ev, fd, &acpt->lsn->ud))
        {
            CLOSE_SOCK(fd);
            continue;
        }
        sock_ctx *rwck = _new_sockctx(fd, &acpt->lsn->lsn_cb.rw_cb, &acpt->lsn->ud);
        _cmd_add(watcher->ev, rwck->fd, rwck);
    }
#ifndef SO_REUSEPORT
    mutex_unlock(&acpt->lsn->lsnlck);
#endif
#ifdef EV_EVPORT
    _add_event(watcher, acpt->sock.fd, &acpt->sock.events, ev, &acpt->sock);
#endif
}
static void _close_lsnsock(listener_ctx *lsn, int32_t cnt)
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
int32_t ev_listen(ev_ctx *ctx, const char *host, const uint16_t port,
    lsn_cb_ctx *cbs, freeud_cb fud_cb, ud_cxt *ud)
{
    ASSERTAB(NULL != cbs && NULL != cbs->rw_cb.r_cb, ERRSTR_NULLP);
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
    lsn->fud_cb = fud_cb;
    lsn->lsn_cb = *cbs;
    COPY_UD(lsn->ud, ud);
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
void _freelsn(listener_ctx *lsn)
{
    _close_lsnsock(lsn, lsn->nlsn);
#ifndef SO_REUSEPORT
    mutex_free(&lsn->lsnlck);
#endif
    FREE(lsn->lsnsock);
    FREE(lsn);
}

#endif//EV_IOCP
