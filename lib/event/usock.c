#include "event/uev.h"
#include "buffer.h"
#include "netutils.h"
#include "loger.h"
#include "hashmap.h"

#ifndef EV_IOCP

#define STATUS_SERVER        0x01
#define STATUS_HANDSHAAKE    0x02
#define STATUS_ERROR         0x04

typedef struct lsnsock_ctx {
    sock_ctx sock;
    struct listener_ctx *lsn;
}lsnsock_ctx;
typedef struct listener_ctx {
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
typedef struct tcp_ctx {
    sock_ctx sock;
    int32_t status;
#if WITH_SSL
    SSL *ssl;
#endif
    buffer_ctx buf_r;
    qu_bufs buf_s;
    cbs_ctx cbs;
    ud_cxt ud;
}tcp_ctx;
typedef struct udp_ctx {
    sock_ctx sock;
    int32_t status;
    cbs_ctx cbs;
    IOV_TYPE buf_r;
    struct msghdr msg;
    qu_bufs buf_s;
    netaddr_ctx addr;
    ud_cxt ud;
    char buf[MAX_RECVFROM_SIZE];
}udp_ctx;

static void _on_rw_cb(watcher_ctx *watcher, sock_ctx *skctx, int32_t ev);

void _sk_shutdown(sock_ctx *skctx) {
#if WITH_SSL
    tcp_ctx *tcp = UPCAST(skctx, tcp_ctx, sock);
    evssl_shutdown(tcp->ssl, tcp->sock.fd);
#else
    shutdown(skctx->fd, SHUT_RD);
#endif
}
sock_ctx *_new_sk(SOCKET fd, cbs_ctx *cbs, ud_cxt *ud) {
    tcp_ctx *tcp;
    MALLOC(tcp, sizeof(tcp_ctx));
    tcp->sock.ev_cb = _on_rw_cb;
    tcp->sock.type = SOCK_STREAM;
    tcp->sock.fd = fd;
    tcp->sock.events = 0;
    tcp->status = 0;
#if WITH_SSL
    tcp->ssl = NULL;
#endif
    tcp->cbs = *cbs;
    COPY_UD(tcp->ud, ud);
    buffer_init(&tcp->buf_r);
    qu_bufs_init(&tcp->buf_s, INIT_SENDBUF_LEN);
    return &tcp->sock;
}
void _free_sk(sock_ctx *skctx) {
    tcp_ctx *tcp = UPCAST(skctx, tcp_ctx, sock);
#if WITH_SSL
    FREE_SSL(tcp->ssl);
#endif
    CLOSE_SOCK(tcp->sock.fd);
    buffer_free(&tcp->buf_r);
    _bufs_clear(&tcp->buf_s);
    qu_bufs_free(&tcp->buf_s);
    if (NULL != tcp->cbs.ud_free) {
        tcp->cbs.ud_free(&tcp->ud);
    }
    FREE(tcp);
}
void _clear_sk(sock_ctx *skctx) {
    tcp_ctx *tcp = UPCAST(skctx, tcp_ctx, sock);
    tcp->sock.events = 0;
    tcp->status = 0;
#if WITH_SSL
    FREE_SSL(tcp->ssl);
#endif
    CLOSE_SOCK(tcp->sock.fd);
    _bufs_clear(&tcp->buf_s);
    buffer_drain(&tcp->buf_r, buffer_size(&tcp->buf_r));
    if (NULL != tcp->cbs.ud_free) {
        tcp->cbs.ud_free(&tcp->ud);
    }
}
void _reset_sk(sock_ctx *skctx, SOCKET fd, cbs_ctx *cbs, ud_cxt *ud) {
    tcp_ctx *tcp = UPCAST(skctx, tcp_ctx, sock);
    tcp->sock.fd = fd;
    tcp->cbs = *cbs;
    COPY_UD(tcp->ud, ud);
}
void _set_tcp_error(sock_ctx *skctx) {
    UPCAST(skctx, tcp_ctx, sock)->status |= STATUS_ERROR;
}
void _add_fd(watcher_ctx *watcher, sock_ctx *skctx) {
    ASSERTAB(NULL == hashmap_set(watcher->element, &skctx), "socket repeat.");
}
static inline void _remove_fd(watcher_ctx *watcher, SOCKET fd) {
    sock_ctx key;
    key.fd = fd;
    sock_ctx *pkey = &key;
    hashmap_delete(watcher->element, &pkey);
}
//rw
#if WITH_SSL
static inline int32_t _ssl_handshake_acpt(watcher_ctx *watcher, tcp_ctx *tcp) {
    int32_t rtn = ERR_FAILED;
    switch (evssl_tryacpt(tcp->ssl)) {
    case ERR_FAILED://错误
#ifdef MANUAL_REMOVE
        _del_event(watcher, tcp->sock.fd, &tcp->sock.events, tcp->sock.events, &tcp->sock);
#endif
        _remove_fd(watcher, tcp->sock.fd);
        pool_push(&watcher->pool, &tcp->sock);
        break;
    case 1://完成
        if (ERR_OK != tcp->cbs.acp_cb(watcher->ev, tcp->sock.fd, &tcp->ud)) {
#ifdef MANUAL_REMOVE
            _del_event(watcher, tcp->sock.fd, &tcp->sock.events, tcp->sock.events, &tcp->sock);
#endif
            _remove_fd(watcher, tcp->sock.fd);
            pool_push(&watcher->pool, &tcp->sock);
            break;
        }
        tcp->status |= STATUS_HANDSHAAKE;
        rtn = ERR_OK;
        break;
    case ERR_OK://等待更多数据
#ifdef MANUAL_ADD
        if (ERR_OK != _add_event(watcher, tcp->sock.fd, &tcp->sock.events, EVENT_READ, &tcp->sock)) {
            _remove_fd(watcher, tcp->sock.fd);
            pool_push(&watcher->pool, &tcp->sock);
        }
#endif
        break;
    }
    return rtn;
}
static inline int32_t _ssl_handshake_conn(watcher_ctx *watcher, tcp_ctx *tcp) {
    int32_t rtn = ERR_FAILED;
    switch (evssl_tryconn(tcp->ssl)) {
    case ERR_FAILED://错误
        tcp->cbs.conn_cb(watcher->ev, tcp->sock.fd, ERR_FAILED, &tcp->ud);
#ifdef MANUAL_REMOVE
        _del_event(watcher, tcp->sock.fd, &tcp->sock.events, tcp->sock.events, &tcp->sock);
#endif
        _remove_fd(watcher, tcp->sock.fd);
        pool_push(&watcher->pool, &tcp->sock);
        break;
    case 1://完成
        if (ERR_OK != tcp->cbs.conn_cb(watcher->ev, tcp->sock.fd, ERR_OK, &tcp->ud)) {
#ifdef MANUAL_REMOVE
            _del_event(watcher, tcp->sock.fd, &tcp->sock.events, tcp->sock.events, &tcp->sock);
#endif
            _remove_fd(watcher, tcp->sock.fd);
            pool_push(&watcher->pool, &tcp->sock);
            break;
        }
        tcp->status |= STATUS_HANDSHAAKE;
        rtn = ERR_OK;
        break;
    case ERR_OK://等待更多数据
#ifdef MANUAL_ADD
        if (ERR_OK != _add_event(watcher, tcp->sock.fd, &tcp->sock.events, EVENT_READ, &tcp->sock)) {
            tcp->cbs.conn_cb(watcher->ev, tcp->sock.fd, ERR_FAILED, &tcp->ud);
            _remove_fd(watcher, tcp->sock.fd);
            pool_push(&watcher->pool, &tcp->sock);
        }
#endif
        break;
    }
    return rtn;
}
static inline int32_t _ssl_handshake(watcher_ctx *watcher, tcp_ctx *tcp) {
    if (tcp->status & STATUS_SERVER) {
        return _ssl_handshake_acpt(watcher, tcp);
    }
    return _ssl_handshake_conn(watcher, tcp);
}
#endif
static inline int32_t _tcp_recv(watcher_ctx *watcher, tcp_ctx *tcp) {
    size_t nread;
#if WITH_SSL
    int32_t rtn = buffer_from_sock(&tcp->buf_r, tcp->sock.fd, &nread, _sock_read, tcp->ssl);
#else
    int32_t rtn = buffer_from_sock(&tcp->buf_r, tcp->sock.fd, &nread, _sock_read, NULL);
#endif
    if (0 != nread) {
        tcp->cbs.r_cb(watcher->ev, tcp->sock.fd, &tcp->buf_r, nread, &tcp->ud);
    }
#ifdef MANUAL_ADD
    if (ERR_OK == rtn) {
        rtn = _add_event(watcher, tcp->sock.fd, &tcp->sock.events, EVENT_READ, &tcp->sock);
    }
#endif
    return rtn;
}
static inline int32_t _on_w_cb(watcher_ctx *watcher, tcp_ctx *tcp) {
    size_t nsend;
#if WITH_SSL
    int32_t rtn = _sock_send(tcp->sock.fd, &tcp->buf_s, &nsend, tcp->ssl);
#else
    int32_t rtn = _sock_send(tcp->sock.fd, &tcp->buf_s, &nsend, NULL);
#endif
    if (NULL != tcp->cbs.s_cb
        && 0 != nsend) {
        tcp->cbs.s_cb(watcher->ev, tcp->sock.fd, nsend, &tcp->ud);
    }
    if (ERR_OK != rtn) {
        return rtn;
    }
    if (0 == qu_bufs_size(&tcp->buf_s)) {
        _del_event(watcher, tcp->sock.fd, &tcp->sock.events, EVENT_WRITE, &tcp->sock);
        return ERR_OK;
    }
#ifdef MANUAL_ADD
    rtn = _add_event(watcher, tcp->sock.fd, &tcp->sock.events, EVENT_WRITE, &tcp->sock);
#endif
    return rtn;
}
static void _on_rw_cb(watcher_ctx *watcher, sock_ctx *skctx, int32_t ev) {
    tcp_ctx *tcp = UPCAST(skctx, tcp_ctx, sock);
    if (tcp->status & STATUS_ERROR) {
        int32_t handshake = 1;
#if WITH_SSL
        if (NULL != tcp->ssl
            && !(tcp->status & STATUS_HANDSHAAKE)) {
            handshake = 0;
        }
#endif
        if (handshake) {
            if (NULL != tcp->cbs.c_cb) {
                tcp->cbs.c_cb(watcher->ev, tcp->sock.fd, &tcp->ud);
            }
        } else {
            if (!(tcp->status & STATUS_SERVER)) {
                tcp->cbs.conn_cb(watcher->ev, tcp->sock.fd, ERR_FAILED, &tcp->ud);
            }
        }
#ifdef MANUAL_REMOVE
        _del_event(watcher, tcp->sock.fd, &tcp->sock.events, tcp->sock.events, &tcp->sock);
#endif
        _remove_fd(watcher, tcp->sock.fd);
        pool_push(&watcher->pool, &tcp->sock);
        return;
    }
    int32_t rtn = ERR_OK;
    if (ev & EVENT_READ) {
#if WITH_SSL
        if (NULL != tcp->ssl
            && !(tcp->status & STATUS_HANDSHAAKE)) {
            if (ERR_OK != _ssl_handshake(watcher, tcp)) {
                return;
            }
        }
#endif
        rtn = _tcp_recv(watcher, tcp);
    }
    if (ERR_OK == rtn
        && (ev & EVENT_WRITE)) {
        rtn = _on_w_cb(watcher, tcp);
    }
    if (ERR_OK != rtn) {
        if (NULL != tcp->cbs.c_cb) {
            tcp->cbs.c_cb(watcher->ev, tcp->sock.fd, &tcp->ud);
        }
#ifdef MANUAL_REMOVE
        _del_event(watcher, tcp->sock.fd, &tcp->sock.events, tcp->sock.events, &tcp->sock);
#endif
        _remove_fd(watcher, tcp->sock.fd);
        pool_push(&watcher->pool, &tcp->sock);
    }
}
void _add_write_inloop(watcher_ctx *watcher, sock_ctx *skctx, bufs_ctx *buf) {
    qu_bufs_push((SOCK_STREAM == skctx->type) ?
                  &UPCAST(skctx, tcp_ctx, sock)->buf_s :
                  &UPCAST(skctx, udp_ctx, sock)->buf_s, 
                  buf);
    if (!(skctx->events & EVENT_WRITE)) {
        _add_event(watcher, skctx->fd, &skctx->events, EVENT_WRITE, skctx);
    }
}
//connect
static inline void _on_connect_cb(watcher_ctx *watcher, sock_ctx *skctx, int32_t ev) {
    tcp_ctx *tcp = UPCAST(skctx, tcp_ctx, sock);
    tcp->sock.ev_cb = _on_rw_cb;
    _del_event(watcher, tcp->sock.fd, &tcp->sock.events, tcp->sock.events, NULL);
    if (ERR_OK != sock_checkconn(tcp->sock.fd)) {
        tcp->cbs.conn_cb(watcher->ev, tcp->sock.fd, ERR_FAILED, &tcp->ud);
        _remove_fd(watcher, tcp->sock.fd);
        pool_push(&watcher->pool, &tcp->sock);
        return;
    }
    int32_t handshake = 1;
#if WITH_SSL
    if (NULL != tcp->ssl) {
        switch (evssl_tryconn(tcp->ssl)) {
        case ERR_FAILED://错误
            tcp->cbs.conn_cb(watcher->ev, tcp->sock.fd, ERR_FAILED, &tcp->ud);
            _remove_fd(watcher, tcp->sock.fd);
            pool_push(&watcher->pool, &tcp->sock);
            return;
        case 1://完成
            tcp->status |= STATUS_HANDSHAAKE;
            break;
        case ERR_OK://等待更多数据
            handshake = 0;
            break;
        }
    }
#endif
    if (handshake) {
        if (ERR_OK != tcp->cbs.conn_cb(watcher->ev, tcp->sock.fd, ERR_OK, &tcp->ud)) {
            _remove_fd(watcher, tcp->sock.fd);
            pool_push(&watcher->pool, &tcp->sock);
            return;
        }
    }
    if (ERR_OK != _add_event(watcher, tcp->sock.fd, &tcp->sock.events, EVENT_READ, &tcp->sock)) {
        if (handshake) {
            if (NULL != tcp->cbs.c_cb) {
                tcp->cbs.c_cb(watcher->ev, tcp->sock.fd, &tcp->ud);
            }
        } else {
            tcp->cbs.conn_cb(watcher->ev, tcp->sock.fd, ERR_FAILED, &tcp->ud);
        }
        _remove_fd(watcher, tcp->sock.fd);
        pool_push(&watcher->pool, &tcp->sock);
    }
}
SOCKET ev_connect(ev_ctx *ctx, struct evssl_ctx *evssl, const char *host, const uint16_t port, cbs_ctx *cbs, ud_cxt *ud) {
    ASSERTAB(NULL != cbs && NULL != cbs->conn_cb && NULL != cbs->r_cb, ERRSTR_NULLP);
    netaddr_ctx addr;
    if (ERR_OK != netaddr_sethost(&addr, host, port)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return INVALID_SOCK;
    }
    SOCKET fd = _create_sock(SOCK_STREAM, netaddr_family(&addr));
    if (INVALID_SOCK == fd) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return INVALID_SOCK;
    }
    sock_raddr(fd);
    _set_sockops(fd);
    int32_t rtn = connect(fd, netaddr_addr(&addr), netaddr_size(&addr));
    if (ERR_OK != rtn) {
        rtn = ERRNO;
        if (!ERR_CONNECT_RETRIABLE(rtn)) {
            CLOSE_SOCK(fd);
            return INVALID_SOCK;
        }
    }
    sock_ctx *skctx = _new_sk(fd, cbs, ud);
    skctx->ev_cb = _on_connect_cb;
#if WITH_SSL
    if (NULL != evssl) {
        tcp_ctx *tcp = UPCAST(skctx, tcp_ctx, sock);
        tcp->ssl = evssl_setfd(evssl, fd);
        if (NULL == tcp->ssl) {
            _free_sk(skctx);
            return INVALID_SOCK;
        }
    }
#endif
    _cmd_connect(ctx, fd, skctx);
    return fd;
}
void _add_conn_inloop(watcher_ctx *watcher, SOCKET fd, sock_ctx *skctx) {
    _add_fd(watcher, skctx);
    if(ERR_OK != _add_event(watcher, fd, &skctx->events, EVENT_WRITE, skctx)) {
        tcp_ctx *tcp = UPCAST(skctx, tcp_ctx, sock);
        tcp->cbs.conn_cb(watcher->ev, fd, ERR_FAILED, &tcp->ud);
        skctx->ev_cb = _on_rw_cb;
        _remove_fd(watcher, fd);
        pool_push(&watcher->pool, skctx);
    }
}
//listen
static inline void _on_accept_cb(watcher_ctx *watcher, sock_ctx *skctx, int32_t ev) {
    lsnsock_ctx *acpt = UPCAST(skctx, lsnsock_ctx, sock);
#ifndef SO_REUSEPORT
    if (ERR_OK != mutex_trylock(&acpt->lsn->lsnlck)) {
        return;
    }
#endif
    SOCKET fd;
    uint64_t hs;
    watcher_ctx *to;
    while (INVALID_SOCK != (fd = accept(acpt->sock.fd, NULL, NULL))) {
        if (ERR_OK != _set_sockops(fd)) {
            CLOSE_SOCK(fd);
            continue;
        }
        hs = FD_HASH(fd);
        to = GET_PTR(watcher->ev->watcher, watcher->ev->nthreads, hs);
        if (to->index == watcher->index) {
            _add_acpfd_inloop(to, fd, acpt->lsn);
        } else {
            _cmd_add_acpfd(to, hs, fd, acpt->lsn);
        }
    }
#ifndef SO_REUSEPORT
    mutex_unlock(&acpt->lsn->lsnlck);
#endif
#ifdef MANUAL_ADD
    if (ERR_OK != _add_event(watcher, acpt->sock.fd, &acpt->sock.events, ev, &acpt->sock)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
    }
#endif
}
void _add_acpfd_inloop(watcher_ctx *watcher, SOCKET fd, listener_ctx *lsn) {
    int32_t handshake = 1;
    sock_ctx *skctx = pool_pop(&watcher->pool, fd, &lsn->cbs, &lsn->ud);
#if WITH_SSL
    if (NULL != lsn->evssl) {
        tcp_ctx *tcp = UPCAST(skctx, tcp_ctx, sock);
        tcp->ssl = evssl_setfd(lsn->evssl, fd);
        if (NULL == tcp->ssl) {
            pool_push(&watcher->pool, skctx);
            return;
        }
        handshake = 0;
        tcp->status |= STATUS_SERVER;
    }
#endif
    _add_fd(watcher, skctx);
    if (handshake) {
        if (ERR_OK != lsn->cbs.acp_cb(watcher->ev, fd, &lsn->ud)) {
            _remove_fd(watcher, fd);
            pool_push(&watcher->pool, skctx);
            return;
        }
    }
    if (ERR_OK != _add_event(watcher, fd, &skctx->events, EVENT_READ, skctx)) {
        if (NULL != lsn->cbs.c_cb
            && handshake) {
            lsn->cbs.c_cb(watcher->ev, fd, &lsn->ud);
        }
        _remove_fd(watcher, fd);
        pool_push(&watcher->pool, skctx);
    }
}
static inline void _close_lsnsock(listener_ctx *lsn, int32_t cnt) {
#ifndef SO_REUSEPORT
    CLOSE_SOCK(lsn->lsnsock[0].sock.fd);
#else
    for (int32_t i = 0; i < cnt; i++) {
        CLOSE_SOCK(lsn->lsnsock[i].sock.fd);
    }
#endif
}
int32_t ev_listen(ev_ctx *ctx, struct evssl_ctx *evssl, const char *host, const uint16_t port, cbs_ctx *cbs, ud_cxt *ud) {
    ASSERTAB(NULL != cbs && NULL != cbs->acp_cb && NULL != cbs->r_cb, ERRSTR_NULLP);
    netaddr_ctx addr;
    if (ERR_OK != netaddr_sethost(&addr, host, port)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
#ifndef SO_REUSEPORT
    SOCKET fd = _listen(&addr);
    if (INVALID_SOCK == fd) {
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
    for (i = 0; i < lsn->nlsn; i++) {
        lsnsock = &lsn->lsnsock[i];
        lsnsock->lsn = lsn;
        lsnsock->sock.type = 0;
        lsnsock->sock.events = 0;
        lsnsock->sock.ev_cb = _on_accept_cb;
#ifndef SO_REUSEPORT
        lsnsock->sock.fd = fd;
#else
        lsnsock->sock.fd = _listen(&addr);
        if (INVALID_SOCK == lsnsock->sock.fd) {
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
    for (i = 0; i < lsn->nlsn; i++) {
        _cmd_listen(&ctx->watcher[i], lsn->lsnsock[i].sock.fd, &lsn->lsnsock[i].sock);
    }
    return ERR_OK;
}
void _add_lsn_inloop(watcher_ctx *watcher, SOCKET fd, sock_ctx *skctx) {
    _add_fd(watcher, skctx);
    if (ERR_OK != _add_event(watcher, fd, &skctx->events, EVENT_READ, skctx)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        _remove_fd(watcher, fd);
    }
}
void _freelsn(listener_ctx *lsn) {
    _close_lsnsock(lsn, lsn->nlsn);
#ifndef SO_REUSEPORT
    mutex_free(&lsn->lsnlck);
#endif
    FREE(lsn->lsnsock);
    FREE(lsn);
}
//UDP
static inline void _init_msghdr(struct msghdr *msg, netaddr_ctx *addr, IOV_TYPE *iov, uint32_t niov) {
    ZERO(msg, sizeof(struct msghdr));
    msg->msg_name = netaddr_addr(addr);
    msg->msg_namelen = netaddr_size(addr);
    msg->msg_iov = iov;
    msg->msg_iovlen = niov;
}
static inline int32_t _on_udp_rcb(watcher_ctx *watcher, udp_ctx *udp) { 
    int32_t rtn = (int32_t)recvmsg(udp->sock.fd, &udp->msg, 0);
    if (rtn > 0) {
        udp->cbs.rf_cb(watcher->ev, udp->sock.fd, udp->buf_r.IOV_PTR_FIELD, (size_t)rtn, &udp->addr, &udp->ud);
        rtn = ERR_OK;
    } else {
        if (0 == rtn) {
            rtn = ERR_FAILED;
        } else {
            if (ERR_RW_RETRIABLE(ERRNO)) {
                rtn = ERR_OK;
            }
        }
    }
#ifdef MANUAL_ADD
    if (ERR_OK == rtn) {
        rtn = _add_event(watcher, udp->sock.fd, &udp->sock.events, EVENT_READ, &udp->sock);
    }
#endif
    return rtn;
}
static inline int32_t _on_udp_wcb(watcher_ctx *watcher, udp_ctx *udp) {
    IOV_TYPE iov;
    bufs_ctx *buf;
    netaddr_ctx *addr;
    struct msghdr msg;
    int32_t rtn = ERR_OK;
    while (NULL != (buf = qu_bufs_pop(&udp->buf_s))) {
        addr = (netaddr_ctx *)buf->data;
        iov.IOV_PTR_FIELD = (char *)buf->data + sizeof(netaddr_ctx);
        iov.IOV_LEN_FIELD = (IOV_LEN_TYPE)buf->len;
        _init_msghdr(&msg, addr, &iov, 1);
        rtn = sendmsg(udp->sock.fd, &msg, 0);
        FREE(buf->data);
        if (rtn > 0) {
            rtn = ERR_OK;
        } else {
            if (0 == rtn) {
                rtn = ERR_FAILED;
            } else {
                if (ERR_RW_RETRIABLE(ERRNO)) {
                    rtn = ERR_OK;
                }
            }
            break;
        }
    }
    if (ERR_OK != rtn) {
        return rtn;
    }
    if (0 == qu_bufs_size(&udp->buf_s)) {
        _del_event(watcher, udp->sock.fd, &udp->sock.events, EVENT_WRITE, &udp->sock);
        return ERR_OK;
    }
#ifdef MANUAL_ADD
    rtn = _add_event(watcher, udp->sock.fd, &udp->sock.events, EVENT_WRITE, &udp->sock);
#endif
    return rtn;
}
static void _on_udp_rw(watcher_ctx *watcher, sock_ctx *skctx, int32_t ev) {
    udp_ctx *udp = UPCAST(skctx, udp_ctx, sock);
    if (udp->status & STATUS_ERROR) {
#ifdef MANUAL_REMOVE
        _del_event(watcher, skctx->fd, &skctx->events, skctx->events, skctx);
#endif
        _remove_fd(watcher, skctx->fd);
        _free_udp(skctx);
        return;
    }
    int32_t rtn = ERR_OK;
    if (ev & EVENT_READ) {
        rtn = _on_udp_rcb(watcher, udp);
    }
    if (ERR_OK == rtn
        && (ev & EVENT_WRITE)) {
        rtn = _on_udp_wcb(watcher, udp);
    }
    if (ERR_OK != rtn) {
#ifdef MANUAL_REMOVE
        _del_event(watcher, skctx->fd, &skctx->events, skctx->events, skctx);
#endif
        _remove_fd(watcher, skctx->fd);
        _free_udp(skctx);
    }
}
static inline sock_ctx *_new_udp(SOCKET fd, int32_t family, cbs_ctx *cbs, ud_cxt *ud) {
    udp_ctx *udp;
    MALLOC(udp, sizeof(udp_ctx));
    udp->sock.ev_cb = _on_udp_rw;
    udp->sock.type = SOCK_DGRAM;
    udp->sock.fd = fd;
    udp->sock.events = 0;
    udp->status = 0;
    udp->cbs = *cbs;
    udp->buf_r.IOV_PTR_FIELD = udp->buf;
    udp->buf_r.IOV_LEN_FIELD = sizeof(udp->buf);
    COPY_UD(udp->ud, ud);
    netaddr_empty_addr(&udp->addr, family);
    _init_msghdr(&udp->msg, &udp->addr, &udp->buf_r, 1);
    qu_bufs_init(&udp->buf_s, INIT_SENDBUF_LEN);
    return &udp->sock;
}
void _free_udp(sock_ctx *skctx) {
    udp_ctx *udp = UPCAST(skctx, udp_ctx, sock);
    CLOSE_SOCK(udp->sock.fd);
    _bufs_clear(&udp->buf_s);
    qu_bufs_free(&udp->buf_s);
    if (NULL != udp->cbs.ud_free) {
        udp->cbs.ud_free(&udp->ud);
    }
    FREE(udp);
}
void _set_udp_error(watcher_ctx *watcher, sock_ctx *skctx) {
    UPCAST(skctx, udp_ctx, sock)->status |= STATUS_ERROR;
    _add_event(watcher, skctx->fd, &skctx->events, EVENT_WRITE, skctx);
}
SOCKET ev_udp(ev_ctx *ctx, const char *host, const uint16_t port, cbs_ctx *cbs, ud_cxt *ud) {
    ASSERTAB(NULL != cbs->rf_cb, ERRSTR_NULLP);
    netaddr_ctx addr;
    if (ERR_OK != netaddr_sethost(&addr, host, port)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return INVALID_SOCK;
    }
    SOCKET fd = _udp(&addr);
    if (INVALID_SOCK == fd) {
        return INVALID_SOCK;
    }
    sock_ctx *skctx = _new_udp(fd, netaddr_family(&addr), cbs, ud);
    _cmd_add_udp(ctx, fd, skctx);
    return fd;
}
void _add_udp_inloop(watcher_ctx *watcher, SOCKET fd, sock_ctx *skctx) {
    _add_fd(watcher, skctx);
    if (ERR_OK != _add_event(watcher, fd, &skctx->events, EVENT_READ, skctx)) {
        _remove_fd(watcher, fd);
        _free_udp(skctx);
        return;
    }
}

#endif//EV_IOCP
