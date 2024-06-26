#include "event/event.h"
#include "utils/netutils.h"

typedef enum ud_type {
    UD_PKTYPE = 0x00,
    UD_STATUS,
    UD_NAME,
    UD_SESS,
    UD_EXTRA
}ud_type;

void _bufs_clear(qu_off_buf_ctx *bufs) {
    off_buf_ctx *buf;
    while (NULL != (buf = qu_off_buf_pop(bufs))) {
        FREE(buf->data);
    }
    qu_off_buf_clear(bufs);
}
int32_t _set_sockops(SOCKET fd) {
    if (ERR_OK != sock_nodelay(fd)
        || ERR_OK != sock_nonblock(fd)) {
        return ERR_FAILED;
    }
    return ERR_OK;
}
SOCKET _create_sock(int32_t type, int32_t family) {
#ifdef EV_IOCP
    return WSASocket(family, type, SOCK_STREAM == type ? IPPROTO_TCP : IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED);
#else
    return socket(family, type, 0);
#endif
}
SOCKET _listen(netaddr_ctx *addr) {
    SOCKET fd = _create_sock(SOCK_STREAM, netaddr_family(addr));
    if (INVALID_SOCK == fd) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return INVALID_SOCK;
    }
    sock_reuseaddr(fd);
    sock_reuseport(fd);
    sock_nonblock(fd);
    if (ERR_OK != bind(fd, netaddr_addr(addr), netaddr_size(addr))) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        CLOSE_SOCK(fd);
        return INVALID_SOCK;
    }
    if (ERR_OK != listen(fd, SOMAXCONN)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        CLOSE_SOCK(fd);
        return INVALID_SOCK;
    }
    return fd;
}
SOCKET _udp(netaddr_ctx *addr) {
    SOCKET fd = _create_sock(SOCK_DGRAM, netaddr_family(addr));
    if (INVALID_SOCK == fd) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return INVALID_SOCK;
    }
#ifdef EV_IOCP
    DWORD bytes = 0;
    BOOL behavior = FALSE;
    if (WSAIoctl(fd, 
                 SIO_UDP_CONNRESET,
                 &behavior, 
                 sizeof(behavior),
                 NULL,
                 0, 
                 &bytes,
                 NULL, 
                 NULL) < ERR_OK) {
        CLOSE_SOCK(fd);
        LOG_ERROR("WSAIoctl(%d, SIO_UDP_CONNRESET...) failed. %s", (int32_t)fd, ERRORSTR(ERRNO));
        return INVALID_SOCK;
    }
#endif
    sock_reuseaddr(fd);
    sock_nonblock(fd);
    if (ERR_OK != bind(fd, netaddr_addr(addr), netaddr_size(addr))) {
        CLOSE_SOCK(fd);
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return INVALID_SOCK;
    }
    return fd;
}
#if WITH_SSL
static int32_t _sock_read_ssl(SSL *ssl, IOV_TYPE *iov, uint32_t niov, size_t *nread) {
    size_t readed;
    int32_t rtn;
    for (uint32_t i = 0; i < niov; i++) {
        rtn = evssl_read(ssl, iov[i].IOV_PTR_FIELD, (size_t)iov[i].IOV_LEN_FIELD, &readed);
        *nread += readed;
        if (ERR_OK == rtn) {
            if (readed < (int32_t)iov[i].IOV_LEN_FIELD) {
                return ERR_OK;
            }
            continue;
        }
        return rtn;
    }
    return ERR_OK;
}
#endif
static int32_t _sock_read_normal(SOCKET fd, IOV_TYPE *iov, uint32_t niov, size_t *readed) {
#ifdef EV_IOCP 
    DWORD bytes, flags = 0;
    if (SOCKET_ERROR != WSARecv(fd,
                                iov,
                                niov,
                                &bytes,
                                &flags,
                                NULL,
                                NULL)) {
        if (bytes > 0) {
            *readed = bytes;
            return ERR_OK;
        }
        return ERR_FAILED;
    }
    if (!IS_EAGAIN(ERRNO)) {
        return ERR_FAILED;
    }
    return ERR_OK;
#else
    int32_t rtn = (int32_t)readv(fd, iov, niov);
    if (rtn > 0) {
        *readed = rtn;
        return ERR_OK;
    }
    if (0 == rtn) {
        return ERR_FAILED;
    }
    if (!ERR_RW_RETRIABLE(ERRNO)) {
        return ERR_FAILED;
    }
    return ERR_OK;
#endif
}
int32_t _sock_read(SOCKET fd, IOV_TYPE *iov, uint32_t niov, void *arg, size_t *readed) {
    *readed = 0;
#if WITH_SSL
    if (NULL == arg) {
        return _sock_read_normal(fd, iov, niov, readed);
    }
    return _sock_read_ssl((SSL *)arg, iov, niov, readed);
#else
    return _sock_read_normal(fd, iov, niov, readed);
#endif
}
static uint32_t _bufs_fill_iov(qu_off_buf_ctx *buf_s, size_t bufsize, IOV_TYPE iov[MAX_SEND_NIOV]) {
    if (bufsize > MAX_SEND_NIOV) {
        bufsize = MAX_SEND_NIOV;
    }
    off_buf_ctx *buf;
    size_t total = 0;
    uint32_t cnt = 0;
    for (uint32_t i = 0; i < (uint32_t)bufsize; i++) {
        buf = qu_off_buf_at(buf_s, i);
        iov[i].IOV_PTR_FIELD = ((char *)buf->data) + buf->offset;
        iov[i].IOV_LEN_FIELD = (IOV_LEN_TYPE)(buf->lens - buf->offset);
        total += (size_t)iov[i].IOV_LEN_FIELD;
        cnt++;
        if (total >= MAX_SEND_SIZE) {
            break;
        }
    }
    return cnt;
}
static void _bufs_size_del(qu_off_buf_ctx *buf_s, size_t len) {
    if (0 == len) {
        return;
    }
    size_t buflen;
    off_buf_ctx *buf;
    while (len > 0) {
        buf = qu_off_buf_peek(buf_s);
        buflen = buf->lens - buf->offset;
        if (len >= buflen) {
            FREE(buf->data);
            len -= buflen;
            qu_off_buf_pop(buf_s);
        } else {
            buf->offset += len;
            len = 0;
        }
    }
}
static int32_t _sock_send_iov(SOCKET fd, IOV_TYPE *iov, uint32_t niov, size_t *sended) {
    *sended = 0;
#ifdef EV_IOCP
    DWORD bytes;
    if (SOCKET_ERROR != WSASend(fd,
                                iov, 
                                niov, 
                                &bytes,
                                0, 
                                NULL, 
                                NULL)) {
        *sended = bytes;
        return ERR_OK;
    }
    if (!IS_EAGAIN(ERRNO)) {
        return ERR_FAILED;
    }
    return ERR_OK;
#else
    int32_t rtn = (int32_t)writev(fd, iov, (int32_t)niov);
    if (rtn < 0) {
        if (!ERR_RW_RETRIABLE(ERRNO)) {
            return ERR_FAILED;
        }
        return ERR_OK;
    }
    *sended = rtn;
    return ERR_OK;
#endif
}
static int32_t _sock_send_normal(SOCKET fd, qu_off_buf_ctx *buf_s, size_t *nsend) {
    int32_t rtn = ERR_OK;
    size_t size;
    uint32_t niov;
    IOV_TYPE iov[MAX_SEND_NIOV];
    while (0 != (size = qu_off_buf_size(buf_s))) {
        niov = _bufs_fill_iov(buf_s, size, iov);
        rtn = _sock_send_iov(fd, iov, niov, &size);
        if (ERR_OK == rtn) {
            *nsend += size;
            _bufs_size_del(buf_s, size);
        } else {
            break;
        }
    }
    return rtn;
}
#if WITH_SSL
static int32_t _sock_send_ssl(SSL *ssl, qu_off_buf_ctx *buf_s, size_t *nsend) {
    int32_t rtn = ERR_OK;
    size_t sended;
    off_buf_ctx *buf;
    for (;;) {
        buf = qu_off_buf_peek(buf_s);
        if (NULL == buf) {
            break;
        }
        rtn = evssl_send(ssl, (char *)buf->data + buf->offset, buf->lens - buf->offset, &sended);
        if (ERR_OK == rtn) {
            (*nsend) += sended;
            buf->offset += sended;
            if (buf->offset == buf->lens) {
                qu_off_buf_pop(buf_s);
                FREE(buf->data);
                continue;
            } else {
                break;
            }
        } else {
            break;
        }
    }
    return rtn;
}
#endif
int32_t _sock_send(SOCKET fd, qu_off_buf_ctx *buf_s, size_t *nsend, void *arg) {
    *nsend = 0;
#if WITH_SSL
    if (NULL == arg) {
        return _sock_send_normal(fd, buf_s, nsend);
    }
    return _sock_send_ssl((SSL *)arg, buf_s, nsend);
#else
    return _sock_send_normal(fd, buf_s, nsend);
#endif
}
void ev_ud_pktype(ev_ctx *ctx, SOCKET fd, uint64_t skid, uint8_t pktype) {
    _ev_set_ud(ctx, fd, skid, UD_PKTYPE, pktype);
}
void ev_ud_status(ev_ctx *ctx, SOCKET fd, uint64_t skid, uint8_t status) {
    _ev_set_ud(ctx, fd, skid, UD_STATUS, status);
}
void ev_ud_sess(ev_ctx *ctx, SOCKET fd, uint64_t skid, uint64_t sess) {
    _ev_set_ud(ctx, fd, skid, UD_SESS, sess);
}
void ev_ud_name(ev_ctx *ctx, SOCKET fd, uint64_t skid, name_t name) {
    _ev_set_ud(ctx, fd, skid, UD_NAME, name);
}
void ev_ud_extra(ev_ctx *ctx, SOCKET fd, uint64_t skid, void *extra) {
    _ev_set_ud(ctx, fd, skid, UD_EXTRA, (uint64_t)extra);
}
void _set_ud(ud_cxt *ud, int32_t type, uint64_t val) {
    switch (type) {
    case UD_PKTYPE:
        ud->pktype = (uint8_t)val;
        break;
    case UD_STATUS:
        ud->status = (uint8_t)val;
        break;
    case UD_NAME:
        ud->name = (name_t)val;
        break;
    case UD_SESS:
        ud->sess = val;
        break;
    case UD_EXTRA:
        ud->extra = (void *)val;
        break;
    default:
        break;
    }
}
