#include "event/uev.h"
#include "event/evworker.h"

#ifndef EV_IOCP

#define MAX_CMD_CNT   512

typedef struct lsnsock_ctx
{
    sock_ctx sock;
    struct listener_ctx *lsn;
}lsnsock_ctx;
typedef struct listener_ctx
{
    int32_t family;
    int32_t nlsn;
    accept_cb acp_cb;
    free_ud f_cb;
    ud_cxt ud;
    lsnsock_ctx *lsnsock;
}listener_ctx;
typedef struct conn_ctx
{
    sock_ctx sock;
    connect_cb conn_cb;
    ud_cxt ud;
}conn_ctx;
typedef struct tcp_ctx
{
    sock_ctx sock;
    buffer_ctx buf;
    qu_bufs qubuf;
}tcp_ctx;

static void _close_lsnsock(listener_ctx *lsn, int32_t cnt)
{
    for (int32_t i = 0; i < cnt; i++)
    {
        CLOSE_SOCK(lsn->lsnsock[i].sock.sock);
    }
}
void _on_accept_cb(watcher_ctx *watcher, sock_ctx *skctx, int32_t ev)
{
    SOCKET fd = accept(skctx->sock, NULL, NULL);
    if (INVALID_SOCK != fd)
    {
        lsnsock_ctx *acpt = UPCAST(skctx, lsnsock_ctx, sock);
        ewcmd_accept(watcher->ev->worker, fd, acpt->lsn->acp_cb, &acpt->lsn->ud);
    }
#ifdef EV_EVPORT
    _add_event(watcher, skctx->sock, &skctx->events, ev, skctx);
#endif
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
    listener_ctx *lsn;
    MALLOC(lsn, sizeof(listener_ctx));
    lsn->family = netaddr_family(&addr);
    lsn->nlsn = (ERR_OK == sock_checkrport() ? ctx->nthreads : 1);
    lsn->acp_cb = cb;
    lsn->f_cb = f_cb;
    COPY_UD(lsn->ud, ud);
    MALLOC(lsn->lsnsock, sizeof(lsnsock_ctx) * lsn->nlsn);
    lsnsock_ctx *lsnsock;
    for (int32_t i = 0; i < lsn->nlsn; i++)
    {
        lsnsock = &lsn->lsnsock[i];
        lsnsock->lsn = lsn;
        lsnsock->sock.events = 0;
        lsnsock->sock.flag = FLAG_LSN;
        lsnsock->sock.sock = _ev_listen(&addr);
        if (INVALID_SOCK == lsnsock->sock.sock)
        {
            _close_lsnsock(lsn, i);
            FREE(lsn->lsnsock);
            FREE(lsn);
            return ERR_FAILED;
        }
    }
    mutex_lock(&ctx->mulsn);
    qu_lsn_push(&ctx->qulsn, &lsn);
    mutex_unlock(&ctx->mulsn);
    if (1 == lsn->nlsn)
    {
        _post_listen_rand(ctx, &lsn->lsnsock->sock);
    }
    else
    {
        for (int32_t i = 0; i < lsn->nlsn; i++)
        {
            _post_listen(&ctx->watcher[i], &lsn->lsnsock[i].sock);
        }
    }
    return ERR_OK;
}
void _freelsn(listener_ctx *lsn)
{
    _close_lsnsock(lsn, lsn->nlsn);
    FREE(lsn->lsnsock);
    FREE(lsn);
}
void _on_connect_cb(watcher_ctx *watcher, sock_ctx *skctx, int32_t ev)
{
    _del_event(watcher, skctx->sock, &skctx->events, ev, skctx);
    ASSERTAB(0 == skctx->events, "logic error.");
    conn_ctx *conn = UPCAST(skctx, conn_ctx, sock);
    ewcmd_connect(watcher->ev->worker, skctx->sock, conn->conn_cb, &conn->ud);
    FREE(conn);
}
int32_t ev_connecter(ev_ctx *ctx, const char *host, const uint16_t port,
    connect_cb conn_cb, ud_cxt *ud)
{
    ASSERTAB(NULL != conn_cb, ERRSTR_NULLP);
    netaddr_ctx addr;
    if (ERR_OK != netaddr_sethost(&addr, host, port))
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    SOCKET fd = _ev_sock(netaddr_family(&addr));
    if (INVALID_SOCK == fd)
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    sock_raddr(fd);
    _set_sockops(fd);
    if (ERR_OK == connect(fd, netaddr_addr(&addr), netaddr_size(&addr)))
    {
        ewcmd_connect(ctx->worker, fd, conn_cb, ud);
        return ERR_OK;
    }
    int32_t rtn = ERRNO;
    if (!ERR_CONNECT_RETRIABLE(rtn))
    {
        CLOSE_SOCK(fd);
        LOG_ERROR("%s", ERRORSTR(rtn));
        return ERR_FAILED;
    }

    conn_ctx *conn;
    MALLOC(conn, sizeof(conn_ctx));
    conn->conn_cb = conn_cb;
    COPY_UD(conn->ud, ud);
    conn->sock.events = 0;
    conn->sock.flag = FLAG_CONN;
    conn->sock.sock = fd;
    _post_connect(ctx, &conn->sock);
    return ERR_OK;
}
void _on_cmd_cb(watcher_ctx *watcher, sock_ctx *skctx, int32_t ev)
{
    char buf[MAX_CMD_CNT];
    watcher->ncmd = read(skctx->sock, buf, sizeof(buf));
#ifdef EV_EVPORT
    _add_event(watcher, skctx->sock, &skctx->events, ev, skctx);
#endif
}
void _on_cmd_rw(watcher_ctx *watcher, sock_ctx *skctx, int32_t ev)
{
    _del_event(watcher, skctx->sock, &skctx->events, ev, skctx);
    if (ev & EVENT_READ)
    {
        ewcmd_canread(watcher->ev->worker, skctx->sock);
    }
    if (ev & EVENT_WRITE)
    {
        ewcmd_canwrite(watcher->ev->worker, skctx->sock);
    }
}
connect_cb _get_connect_ud(sock_ctx *skctx, ud_cxt **ud)
{
    conn_ctx *conn = UPCAST(skctx, conn_ctx, sock);
    *ud = &conn->ud;
    return conn->conn_cb;
}
sock_ctx *_new_sockctx(ev_ctx *ctx, SOCKET sock)
{
    tcp_ctx *tcp;
    MALLOC(tcp, sizeof(tcp_ctx));
    tcp->sock.events = 0;
    tcp->sock.flag = FLAG_RW;
    tcp->sock.sock = sock;
    buffer_init(&tcp->buf);
    qu_bufs_init(&tcp->qubuf, INIT_SENDBUF_LEN);
    return &tcp->sock;
}
void _reset_sockctx(struct sock_ctx *skctx, SOCKET sock)
{
    tcp_ctx *tcp = UPCAST(skctx, tcp_ctx, sock);
    tcp->sock.events = 0;
    tcp->sock.sock = sock;
    buffer_drain(&tcp->buf, buffer_size(&tcp->buf));
    _qubufs_clear(&tcp->qubuf);
}
void _close_sockctx(struct sock_ctx *skctx)
{
    SOCK_CLOSE(skctx->sock);
}
void _free_sockctx(sock_ctx *skctx)
{
    tcp_ctx *tcp = UPCAST(skctx, tcp_ctx, sock);
    CLOSE_SOCK(tcp->sock.sock);
    buffer_free(&tcp->buf);    
    _qubufs_clear(&tcp->qubuf);
    qu_bufs_free(&tcp->qubuf);
    FREE(tcp);
}
buffer_ctx *_get_recv_buf(sock_ctx *skctx)
{
    tcp_ctx *tcp = UPCAST(skctx, tcp_ctx, sock);
    return &tcp->buf;
}
qu_bufs *_get_send_buf(struct sock_ctx *skctx)
{
    tcp_ctx *tcp = UPCAST(skctx, tcp_ctx, sock);
    return &tcp->qubuf;
}

#endif
