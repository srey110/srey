#include "event/event.h"
#include "netutils.h"
#include "loger.h"

void _bufs_clear(qu_bufs *bufs) {
    bufs_ctx *buf;
    while (NULL != (buf = qu_bufs_pop(bufs))) {
        FREE(buf->data);
    }
    qu_bufs_clear(bufs);
}
int32_t _set_sockops(SOCKET fd) {
    if (ERR_OK != sock_nodelay(fd)
        || ERR_OK != sock_nbio(fd)) {
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
    sock_raddr(fd);
    sock_rport(fd);
    sock_nbio(fd);
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
    sock_raddr(fd);
    sock_nbio(fd);
    if (ERR_OK != bind(fd, netaddr_addr(addr), netaddr_size(addr))) {
        CLOSE_SOCK(fd);
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return INVALID_SOCK;
    }
    return fd;
}
#if WITH_SSL
static inline int32_t _sock_read_ssl(SSL *ssl, IOV_TYPE *iov, uint32_t niov, size_t *nread) {
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
static inline int32_t _sock_read_normal(SOCKET fd, IOV_TYPE *iov, uint32_t niov, size_t *readed) {
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
static inline uint32_t _bufs_fill_iov(qu_bufs *buf_s, size_t bufsize, IOV_TYPE iov[MAX_SEND_NIOV]) {
    if (bufsize > MAX_SEND_NIOV) {
        bufsize = MAX_SEND_NIOV;
    }
    bufs_ctx *buf;
    size_t total = 0;
    uint32_t cnt = 0;
    for (size_t i = 0; i < bufsize; i++) {
        buf = qu_bufs_at(buf_s, i);
        iov[i].IOV_PTR_FIELD = ((char *)buf->data) + buf->offset;
        iov[i].IOV_LEN_FIELD = (IOV_LEN_TYPE)(buf->len - buf->offset);
        total += (size_t)iov[i].IOV_LEN_FIELD;
        cnt++;
        if (total >= MAX_SEND_SIZE) {
            break;
        }
    }
    return cnt;
}
static inline void _bufs_size_del(qu_bufs *buf_s, size_t len) {
    if (0 == len) {
        return;
    }
    size_t buflen;
    bufs_ctx *buf;
    while (len > 0) {
        buf = qu_bufs_peek(buf_s);
        buflen = buf->len - buf->offset;
        if (len >= buflen) {
            FREE(buf->data);
            len -= buflen;
            qu_bufs_pop(buf_s);
        } else {
            buf->offset += len;
            len = 0;
        }
    }
}
static inline int32_t _sock_send_iov(SOCKET fd, IOV_TYPE *iov, uint32_t niov, size_t *sended) {
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
static inline int32_t _sock_send_normal(SOCKET fd, qu_bufs *buf_s, size_t *nsend) {
    int32_t rtn = ERR_OK;
    size_t size;
    uint32_t niov;
    IOV_TYPE iov[MAX_SEND_NIOV];
    while (0 != (size = qu_bufs_size(buf_s))) {
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
static inline int32_t _sock_send_ssl(SSL *ssl, qu_bufs *buf_s, size_t *nsend) {
    int32_t rtn = ERR_OK;
    size_t sended;
    bufs_ctx *buf;
    for (;;) {
        buf = qu_bufs_peek(buf_s);
        if (NULL == buf) {
            break;
        }
        rtn = evssl_send(ssl, (char *)buf->data + buf->offset, buf->len - buf->offset, &sended);
        if (ERR_OK == rtn) {
            (*nsend) += sended;
            buf->offset += sended;
            if (buf->offset == buf->len) {
                qu_bufs_pop(buf_s);
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
int32_t _sock_send(SOCKET fd, qu_bufs *buf_s, size_t *nsend, void *arg) {
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
void _set_ud_typstat_cmd(char *typsta, int8_t pktype, int8_t status) {
    if (-1 != pktype) {
        typsta[0] |= 1;
        typsta[1] = pktype;
    }
    if (-1 != status) {
        typsta[0] |= 2;
        typsta[2] = status;
    }
}
void _set_ud_typstat(char *typsta, ud_cxt *ud) {
    if (typsta[0] & 1) {
        ud->pktype = (uint8_t)typsta[1];
    }
    if (typsta[0] & 2) {
        ud->status = (uint8_t)typsta[2];
    }
}
