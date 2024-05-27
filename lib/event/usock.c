#include "event/uev.h"
#include "buffer.h"
#include "netutils.h"
#include "ds/hashmap.h"

#ifndef EV_IOCP

typedef struct lsnsock_ctx {
    sock_ctx sock;
    struct listener_ctx *lsn;
}lsnsock_ctx;
typedef struct listener_ctx {
    int32_t nlsn;
    int32_t remove;
    atomic_t ref;
    lsnsock_ctx *lsnsock;
#if WITH_SSL
    evssl_ctx *evssl;
#endif
    cbs_ctx cbs;
    ud_cxt ud;
#ifndef SO_REUSEPORT
    spin_ctx spin;
#endif
    uint64_t id;
}listener_ctx;
typedef struct tcp_ctx {
    sock_ctx sock;
    int32_t status;
#if WITH_SSL
    SSL *ssl;
#endif
    uint64_t skid;
    buffer_ctx buf_r;
    qu_off_buf_ctx buf_s;
    cbs_ctx cbs;
    ud_cxt ud;
}tcp_ctx;
typedef struct udp_ctx {
    sock_ctx sock;
    int32_t status;
    uint64_t skid;
    cbs_ctx cbs;
    IOV_TYPE buf_r;
    struct msghdr msg;
    qu_off_buf_ctx buf_s;
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
    tcp->status = STATUS_NONE;
    tcp->skid = createid();
#if WITH_SSL
    tcp->ssl = NULL;
#endif
    tcp->cbs = *cbs;
    COPY_UD(tcp->ud, ud);
    buffer_init(&tcp->buf_r);
    qu_off_buf_init(&tcp->buf_s, INIT_SENDBUF_LEN);
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
    qu_off_buf_free(&tcp->buf_s);
    if (NULL != tcp->cbs.ud_free) {
        tcp->cbs.ud_free(&tcp->ud);
    }
    FREE(tcp);
}
void _clear_sk(sock_ctx *skctx) {
    tcp_ctx *tcp = UPCAST(skctx, tcp_ctx, sock);
    tcp->sock.events = 0;
    tcp->status = STATUS_NONE;
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
    tcp->skid = createid();
    COPY_UD(tcp->ud, ud);
}
ud_cxt *_get_ud(sock_ctx *skctx) {
    if (SOCK_STREAM == skctx->type) {
        return &UPCAST(skctx, tcp_ctx, sock)->ud;
    } else {
        return &UPCAST(skctx, udp_ctx, sock)->ud;
    }
}
int32_t _check_skid(sock_ctx *skctx, const uint64_t skid) {
    if (SOCK_STREAM == skctx->type) {
        if (skid == UPCAST(skctx, tcp_ctx, sock)->skid) {
            return ERR_OK;
        }
    } else {
        if (skid == UPCAST(skctx, udp_ctx, sock)->skid) {
            return ERR_OK;
        }
    }
    return ERR_FAILED;
}
void _disconnect(watcher_ctx *watcher, sock_ctx *skctx) {
    if (SOCK_STREAM == skctx->type) {
        tcp_ctx *tcp = UPCAST(skctx, tcp_ctx, sock);
        if (BIT_CHECK(tcp->status, STATUS_ERROR)) {
            return;
        }
        BIT_SET(tcp->status, STATUS_ERROR);
        _sk_shutdown(skctx);
    } else {
        udp_ctx *udp = UPCAST(skctx, udp_ctx, sock);
        if (BIT_CHECK(udp->status, STATUS_ERROR)) {
            return;
        }
        BIT_SET(udp->status, STATUS_ERROR);
        _add_event(watcher, skctx->fd, &skctx->events, EVENT_WRITE, skctx);
    }
}
void _add_fd(watcher_ctx *watcher, sock_ctx *skctx) {
    ASSERTAB(NULL == hashmap_set(watcher->element, &skctx), "socket repeat.");
}
static void *_remove_fd(watcher_ctx *watcher, SOCKET fd) {
    sock_ctx key;
    key.fd = fd;
    sock_ctx *pkey = &key;
    return (void *)hashmap_delete(watcher->element, &pkey);
}
static inline int32_t _call_acp_cb(ev_ctx *ev, tcp_ctx *tcp) {
    if (NULL != tcp->cbs.acp_cb) {
        return tcp->cbs.acp_cb(ev, tcp->sock.fd, tcp->skid, &tcp->ud);
    }
    return ERR_OK;
}
static inline int32_t _call_conn_cb(ev_ctx *ev, tcp_ctx *tcp, int32_t err) {
    if (NULL != tcp->cbs.conn_cb) {
        return tcp->cbs.conn_cb(ev, tcp->sock.fd, tcp->skid, err, &tcp->ud);
    }
    return ERR_OK;
}
static inline void _call_auth_cb(ev_ctx *ev, tcp_ctx *tcp) {
    if (NULL != tcp->cbs.auth_cb) {
        tcp->cbs.auth_cb(ev, tcp->sock.fd, tcp->skid, BIT_CHECK(tcp->status, STATUS_CLIENT), &tcp->ud);
    }
}
static inline void _call_recv_cb(ev_ctx *ev, tcp_ctx *tcp, size_t nread) {
    if (nread > 0) {
        tcp->cbs.r_cb(ev, tcp->sock.fd, tcp->skid, BIT_CHECK(tcp->status, STATUS_CLIENT), &tcp->buf_r, nread, &tcp->ud);
    }
}
static inline void _call_send_cb(ev_ctx *ev, tcp_ctx *tcp, size_t nsend) {
    if (NULL != tcp->cbs.s_cb
        && nsend > 0) {
        tcp->cbs.s_cb(ev, tcp->sock.fd, tcp->skid, BIT_CHECK(tcp->status, STATUS_CLIENT), nsend, &tcp->ud);
    }
}
static inline void _call_close_cb(ev_ctx *ev, tcp_ctx *tcp) {
    if (NULL != tcp->cbs.c_cb) {
        tcp->cbs.c_cb(ev, tcp->sock.fd, tcp->skid, BIT_CHECK(tcp->status, STATUS_CLIENT), &tcp->ud);
    }
}
static inline void _call_recvfrom_cb(ev_ctx *ev, udp_ctx *udp, size_t nread) {
    if (nread > 0) {
        udp->cbs.rf_cb(ev, udp->sock.fd, udp->skid, udp->buf_r.IOV_PTR_FIELD, nread, &udp->addr, &udp->ud);
    }
}
//rw
static int32_t _tcp_recv(watcher_ctx *watcher, tcp_ctx *tcp) {
    size_t nread;
#if WITH_SSL
    int32_t rtn = buffer_from_sock(&tcp->buf_r, tcp->sock.fd, &nread, _sock_read, tcp->ssl);
#else
    int32_t rtn = buffer_from_sock(&tcp->buf_r, tcp->sock.fd, &nread, _sock_read, NULL);
#endif
    _call_recv_cb(watcher->ev, tcp, nread);
#ifdef MANUAL_ADD
    if (ERR_OK == rtn) {
        rtn = _add_event(watcher, tcp->sock.fd, &tcp->sock.events, EVENT_READ, &tcp->sock);
    }
#endif
    return rtn;
}
static int32_t _tcp_send(watcher_ctx *watcher, tcp_ctx *tcp) {
    size_t nsend;
#if WITH_SSL
    int32_t rtn = _sock_send(tcp->sock.fd, &tcp->buf_s, &nsend, tcp->ssl);
#else
    int32_t rtn = _sock_send(tcp->sock.fd, &tcp->buf_s, &nsend, NULL);
#endif
    _call_send_cb(watcher->ev, tcp, nsend);
    if (ERR_OK != rtn) {
        return rtn;
    }
    if (0 == qu_off_buf_size(&tcp->buf_s)) {
        _del_event(watcher, tcp->sock.fd, &tcp->sock.events, EVENT_WRITE, &tcp->sock);
        return ERR_OK;
    }
#ifdef MANUAL_ADD
    rtn = _add_event(watcher, tcp->sock.fd, &tcp->sock.events, EVENT_WRITE, &tcp->sock);
#endif
    return rtn;
}
static inline void _close_tcp(watcher_ctx *watcher, tcp_ctx *tcp) {
    _call_close_cb(watcher->ev, tcp);
#ifdef MANUAL_REMOVE
    _del_event(watcher, tcp->sock.fd, &tcp->sock.events, tcp->sock.events, &tcp->sock);
#endif
    _remove_fd(watcher, tcp->sock.fd);
    pool_push(&watcher->pool, &tcp->sock);
}
static void _on_rw_cb(watcher_ctx *watcher, sock_ctx *skctx, int32_t ev) {
    int32_t rtn;
    int32_t evread = BIT_CHECK(ev, EVENT_READ);
    tcp_ctx *tcp = UPCAST(skctx, tcp_ctx, sock);
#if WITH_SSL
    if (evread
        && NULL != tcp->ssl
        && BIT_CHECK(tcp->status, STATUS_AUTHSSL)
        && !BIT_CHECK(tcp->status, STATUS_ERROR)) {
        if (BIT_CHECK(tcp->status, STATUS_CLIENT)) {
            rtn = evssl_tryconn(tcp->ssl);
        } else {
            rtn = evssl_tryacpt(tcp->ssl);
        }
        switch (rtn) {
        case ERR_FAILED://错误
            BIT_SET(tcp->status, STATUS_ERROR);
            break;
        case 1://完成
            _call_auth_cb(watcher->ev, tcp);
            BIT_REMOVE(tcp->status, STATUS_AUTHSSL);
#ifdef READV_EINVAL
            return;
#else
            break;
#endif
        case ERR_OK://等待更多数据
#ifdef MANUAL_ADD
            if (ERR_OK != _add_event(watcher, tcp->sock.fd, &tcp->sock.events, EVENT_READ, &tcp->sock)) {
                BIT_SET(tcp->status, STATUS_ERROR);
                break;
            } else {
                return;
            }
#else
            return;
#endif
        }//switch
    }
#endif
    if (BIT_CHECK(tcp->status, STATUS_ERROR)) {
        _close_tcp(watcher, tcp);
        return;
    }
    rtn = ERR_OK;
    if (evread) {
        rtn = _tcp_recv(watcher, tcp);
    }
    if (ERR_OK == rtn
        && BIT_CHECK(ev, EVENT_WRITE)) {
        rtn = _tcp_send(watcher, tcp);
    }
    if (ERR_OK != rtn) {
        _close_tcp(watcher, tcp);
    }
}
int32_t _switch_ssl(watcher_ctx *watcher, sock_ctx *skctx, struct evssl_ctx *evssl, int32_t client) {
#if WITH_SSL
    tcp_ctx *tcp = UPCAST(skctx, tcp_ctx, sock);
    if (NULL != tcp->ssl) {
        LOG_WARN("repeat switch ssl.");
        return ERR_OK;
    }
    tcp->ssl = evssl_setfd(evssl, tcp->sock.fd);
    if (NULL == tcp->ssl) {
        return ERR_FAILED;
    }
    if (client) {
        BIT_SET(tcp->status, STATUS_CLIENT);
        switch (evssl_tryconn(tcp->ssl)) {
        case ERR_FAILED://错误
            return ERR_FAILED;
        case 1://完成
            _call_auth_cb(watcher->ev, tcp);
            break;
        case ERR_OK://等待更多数据
            BIT_SET(tcp->status, STATUS_AUTHSSL);
            break;
        }
    } else {
        BIT_REMOVE(tcp->status, STATUS_CLIENT);
        BIT_SET(tcp->status, STATUS_AUTHSSL);
    }
#endif
    return ERR_OK;
}
void _add_write_inloop(watcher_ctx *watcher, sock_ctx *skctx, off_buf_ctx *buf) {
    if (SOCK_STREAM == skctx->type) {
        tcp_ctx *tcp = UPCAST(skctx, tcp_ctx, sock);
        qu_off_buf_push(&tcp->buf_s, buf);
    } else {
        udp_ctx *udp = UPCAST(skctx, udp_ctx, sock);
        qu_off_buf_push(&udp->buf_s, buf);
    }
    if (!BIT_CHECK(skctx->events, EVENT_WRITE)) {
        _add_event(watcher, skctx->fd, &skctx->events, EVENT_WRITE, skctx);
    }
}
//connect
static void _on_connect_cb(watcher_ctx *watcher, sock_ctx *skctx, int32_t ev) {
    tcp_ctx *tcp = UPCAST(skctx, tcp_ctx, sock);
    tcp->sock.ev_cb = _on_rw_cb;
    _del_event(watcher, tcp->sock.fd, &tcp->sock.events, tcp->sock.events, skctx);
    if (ERR_OK != sock_checkconn(tcp->sock.fd)) {
        _call_conn_cb(watcher->ev, tcp, ERR_FAILED);
        _remove_fd(watcher, tcp->sock.fd);
        pool_push(&watcher->pool, &tcp->sock);
        return;
    }
    if (ERR_OK != _call_conn_cb(watcher->ev, tcp, ERR_OK)) {
        _remove_fd(watcher, tcp->sock.fd);
        pool_push(&watcher->pool, &tcp->sock);
        return;
    }
    if (ERR_OK != _add_event(watcher, tcp->sock.fd, &tcp->sock.events, EVENT_READ, &tcp->sock)) {
        _call_close_cb(watcher->ev, tcp);
        _remove_fd(watcher, tcp->sock.fd);
        pool_push(&watcher->pool, &tcp->sock);
    }
}
SOCKET ev_connect(ev_ctx *ctx, const char *ip, const uint16_t port, cbs_ctx *cbs, ud_cxt *ud, uint64_t *skid) {
    ASSERTAB(NULL != cbs && NULL != cbs->r_cb, ERRSTR_NULLP);
    netaddr_ctx addr;
    if (ERR_OK != netaddr_set(&addr, ip, port)) {
        LOG_ERROR("netaddr_set %s:%d, %s", ip, port, ERRORSTR(ERRNO));
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
            LOG_ERROR("connect %s:%d, %s", ip, port, ERRORSTR(ERRNO));
            CLOSE_SOCK(fd);
            return INVALID_SOCK;
        }
    }
    sock_ctx *skctx = _new_sk(fd, cbs, ud);
    skctx->ev_cb = _on_connect_cb;
    tcp_ctx *tcp = UPCAST(skctx, tcp_ctx, sock);
    BIT_SET(tcp->status, STATUS_CLIENT);
    *skid = tcp->skid;
    _cmd_connect(ctx, fd, skctx);
    return fd;
}
void _add_conn_inloop(watcher_ctx *watcher, SOCKET fd, sock_ctx *skctx) {
    _add_fd(watcher, skctx);
    if(ERR_OK != _add_event(watcher, fd, &skctx->events, EVENT_WRITE, skctx)) {
        tcp_ctx *tcp = UPCAST(skctx, tcp_ctx, sock);
        _call_conn_cb(watcher->ev, tcp, ERR_FAILED);
        skctx->ev_cb = _on_rw_cb;
        _remove_fd(watcher, fd);
        pool_push(&watcher->pool, skctx);
    }
}
//listen
static void _on_accept_cb(watcher_ctx *watcher, sock_ctx *skctx, int32_t ev) {
    lsnsock_ctx *acpt = UPCAST(skctx, lsnsock_ctx, sock);
#ifndef SO_REUSEPORT
    if (ERR_OK != spin_trylock(&acpt->lsn->spin)) {
        return;
    }
#endif
    SOCKET fd;
    watcher_ctx *to;
    while (INVALID_SOCK != (fd = accept(acpt->sock.fd, NULL, NULL))) {
        if (ERR_OK != _set_sockops(fd)
            || ERR_OK != sock_kpa(fd, KEEPALIVE_TIME, KEEPALIVE_INTERVAL)) {
            CLOSE_SOCK(fd);
            continue;
        }
        to = GET_PTR(watcher->ev->watcher, watcher->ev->nthreads, fd);
        if (to->index == watcher->index) {
            _add_acpfd_inloop(to, fd, acpt->lsn);
        } else {
            _cmd_add_acpfd(to, fd, acpt->lsn);
        }
    }
#ifndef SO_REUSEPORT
    spin_unlock(&acpt->lsn->spin);
#endif
#ifdef MANUAL_ADD
    if (ERR_OK != _add_event(watcher, acpt->sock.fd, &acpt->sock.events, ev, &acpt->sock)) {
        _remove_fd(watcher, acpt->sock.fd);
        LOG_ERROR("%s", ERRORSTR(ERRNO));
    }
#endif
}
void _add_acpfd_inloop(watcher_ctx *watcher, SOCKET fd, listener_ctx *lsn) {
    sock_ctx *skctx = pool_pop(&watcher->pool, fd, &lsn->cbs, &lsn->ud);
#if WITH_SSL
    if (NULL != lsn->evssl) {
        if (ERR_OK != _switch_ssl(watcher, skctx, lsn->evssl, 0)) {
            pool_push(&watcher->pool, skctx);
            return;
        }
    }
#endif
    tcp_ctx *tcp = UPCAST(skctx, tcp_ctx, sock);
    _add_fd(watcher, skctx);
    if (ERR_OK != _call_acp_cb(watcher->ev, tcp)) {
        _remove_fd(watcher, fd);
        pool_push(&watcher->pool, skctx);
        return;
    }
    if (ERR_OK != _add_event(watcher, fd, &skctx->events, EVENT_READ, skctx)) {
        _call_close_cb(watcher->ev, tcp);
        _remove_fd(watcher, fd);
        pool_push(&watcher->pool, skctx);
    }
}
static void _close_lsnsock(listener_ctx *lsn, int32_t cnt) {
#ifndef SO_REUSEPORT
    CLOSE_SOCK(lsn->lsnsock[0].sock.fd);
#else
    for (int32_t i = 0; i < cnt; i++) {
        CLOSE_SOCK(lsn->lsnsock[i].sock.fd);
    }
#endif
}
int32_t ev_listen(ev_ctx *ctx, struct evssl_ctx *evssl, const char *ip, const uint16_t port,
    cbs_ctx *cbs, ud_cxt *ud, uint64_t *id) {
    ASSERTAB(NULL != cbs && NULL != cbs->r_cb, ERRSTR_NULLP);
    netaddr_ctx addr;
    if (ERR_OK != netaddr_set(&addr, ip, port)) {
        LOG_ERROR("netaddr_set %s:%d, %s", ip, port, ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
#ifndef SO_REUSEPORT
    SOCKET fd = _listen(&addr);
    if (INVALID_SOCK == fd) {
        LOG_ERROR("listen %s:%d error.", ip, port);
        return ERR_FAILED;
    }
#endif
    listener_ctx *lsn;
    MALLOC(lsn, sizeof(listener_ctx));
    lsn->nlsn = ctx->nthreads;
    lsn->ref = 0;
    lsn->remove = 0;
    lsn->cbs = *cbs;
    COPY_UD(lsn->ud, ud);
#if WITH_SSL
    lsn->evssl = evssl;
#endif
#ifndef SO_REUSEPORT
    spin_init(&lsn->spin, SPIN_CNT_LSN);
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
    spin_lock(&ctx->spin);
    arr_ptr_push_back(&ctx->arrlsn, (void **)&lsn);
    spin_unlock(&ctx->spin);
    lsn->ref = lsn->nlsn;
    for (i = 0; i < lsn->nlsn; i++) {
        _cmd_listen(&ctx->watcher[i], lsn->lsnsock[i].sock.fd, &lsn->lsnsock[i].sock);
    }
    lsn->id = createid();
    if (NULL != id) {
        *id = lsn->id;
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
    if (0 == lsn->remove) {
        _close_lsnsock(lsn, lsn->nlsn);
    }
#ifndef SO_REUSEPORT
    spin_free(&lsn->spin);
#endif
    FREE(lsn->lsnsock);
    FREE(lsn);
}
static listener_ctx * _get_listener(ev_ctx *ctx, uint64_t id) {
    listener_ctx *lsn = NULL;
    listener_ctx **tmp;
    spin_lock(&ctx->spin);
    uint32_t n = arr_ptr_size(&ctx->arrlsn);
    for (uint32_t i = 0; i < n; i++) {
        tmp = (listener_ctx **)arr_ptr_at(&ctx->arrlsn, i);
        if ((*tmp)->id == id) {
            lsn = *tmp;
            arr_ptr_del_nomove(&ctx->arrlsn, i);
            break;
        }
    }
    spin_unlock(&ctx->spin);
    return lsn;
}
void ev_unlisten(ev_ctx *ctx, uint64_t id) {
    listener_ctx *lsn = _get_listener(ctx, id);
    if (NULL == lsn) {
        return;
    }
    if (0 == lsn->ref) {
        _freelsn(lsn);
        return;
    }
    lsn->remove = 1;
    for (int32_t i = 0; i < lsn->nlsn; i++) {
        _cmd_unlisten(&ctx->watcher[i], lsn->lsnsock[i].sock.fd, lsn);
    }
}
void _remove_lsn(watcher_ctx *watcher, SOCKET fd, listener_ctx *lsn) {
    sock_ctx **skctx = _remove_fd(watcher, fd);
    SOCK_CLOSE(fd);
    if (NULL != skctx) {
#ifdef MANUAL_REMOVE
        lsnsock_ctx *acpt = UPCAST(*skctx, lsnsock_ctx, sock);
        _del_event(watcher, fd, &acpt->sock.events, EVENT_READ, &acpt->sock);
#endif
    }
    if (1 == ATOMIC_ADD(&lsn->ref, -1)) {
        _freelsn(lsn);
    }
}
//UDP
static void _init_msghdr(struct msghdr *msg, netaddr_ctx *addr, IOV_TYPE *iov, uint32_t niov) {
    ZERO(msg, sizeof(struct msghdr));
    msg->msg_name = netaddr_addr(addr);
    msg->msg_namelen = netaddr_size(addr);
    msg->msg_iov = iov;
    msg->msg_iovlen = niov;
}
static int32_t _on_udp_rcb(watcher_ctx *watcher, udp_ctx *udp) { 
    int32_t rtn = (int32_t)recvmsg(udp->sock.fd, &udp->msg, 0);
    if (rtn > 0) {
        _call_recvfrom_cb(watcher->ev, udp, (size_t)rtn);
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
static int32_t _on_udp_wcb(watcher_ctx *watcher, udp_ctx *udp) {
    IOV_TYPE iov;
    off_buf_ctx *buf;
    netaddr_ctx *addr;
    struct msghdr msg;
    int32_t rtn = ERR_OK;
    while (NULL != (buf = qu_off_buf_pop(&udp->buf_s))) {
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
    if (0 == qu_off_buf_size(&udp->buf_s)) {
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
    if (BIT_CHECK(udp->status, STATUS_ERROR)) {
#ifdef MANUAL_REMOVE
        _del_event(watcher, skctx->fd, &skctx->events, skctx->events, skctx);
#endif
        _remove_fd(watcher, skctx->fd);
        _free_udp(skctx);
        return;
    }
    int32_t rtn = ERR_OK;
    if (BIT_CHECK(ev, EVENT_READ)) {
        rtn = _on_udp_rcb(watcher, udp);
    }
    if (ERR_OK == rtn
        && BIT_CHECK(ev, EVENT_WRITE)) {
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
static sock_ctx *_new_udp(SOCKET fd, cbs_ctx *cbs, ud_cxt *ud) {
    udp_ctx *udp;
    MALLOC(udp, sizeof(udp_ctx));
    udp->sock.ev_cb = _on_udp_rw;
    udp->sock.type = SOCK_DGRAM;
    udp->sock.fd = fd;
    udp->sock.events = 0;
    udp->status = STATUS_NONE;
    udp->skid = createid();
    udp->cbs = *cbs;
    udp->buf_r.IOV_PTR_FIELD = udp->buf;
    udp->buf_r.IOV_LEN_FIELD = sizeof(udp->buf);
    COPY_UD(udp->ud, ud);
    netaddr_empty(&udp->addr);
    _init_msghdr(&udp->msg, &udp->addr, &udp->buf_r, 1);
    qu_off_buf_init(&udp->buf_s, INIT_SENDBUF_LEN);
    return &udp->sock;
}
void _free_udp(sock_ctx *skctx) {
    udp_ctx *udp = UPCAST(skctx, udp_ctx, sock);
    CLOSE_SOCK(udp->sock.fd);
    _bufs_clear(&udp->buf_s);
    qu_off_buf_free(&udp->buf_s);
    if (NULL != udp->cbs.ud_free) {
        udp->cbs.ud_free(&udp->ud);
    }
    FREE(udp);
}
SOCKET ev_udp(ev_ctx *ctx, const char *ip, const uint16_t port,
    cbs_ctx *cbs, ud_cxt *ud, uint64_t *skid) {
    ASSERTAB(NULL != cbs->rf_cb, ERRSTR_NULLP);
    netaddr_ctx addr;
    if (ERR_OK != netaddr_set(&addr, ip, port)) {
        LOG_ERROR("netaddr_set %s:%d, %s", ip, port, ERRORSTR(ERRNO));
        return INVALID_SOCK;
    }
    SOCKET fd = _udp(&addr);
    if (INVALID_SOCK == fd) {
        LOG_ERROR("udp %s:%d error.", ip, port);
        return INVALID_SOCK;
    }
    sock_ctx *skctx = _new_udp(fd, cbs, ud);
    *skid = UPCAST(skctx, udp_ctx, sock)->skid;
    _cmd_add_udp(ctx, fd, skctx);
    return fd;
}
void _add_udp_inloop(watcher_ctx *watcher, SOCKET fd, sock_ctx *skctx) {
    _add_fd(watcher, skctx);
    if (ERR_OK != _add_event(watcher, fd, &skctx->events, EVENT_READ, skctx)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        _remove_fd(watcher, fd);
        _free_udp(skctx);
        return;
    }
}

#endif//EV_IOCP
