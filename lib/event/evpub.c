#include "containers/hashmap.h"
#include "utils/netutils.h"
#ifdef EV_IOCP
#include "event/iocp.h"
#else
#include "event/uev.h"
#endif

// hashmap 哈希函数：以 fd 为 key
uint64_t _evpub_sockel_hash(const void *item, uint64_t seed0, uint64_t seed1) {
    (void)seed0;
    (void)seed1;
    return hash_u64((uint64_t)(*(const sock_ctx **)item)->fd);
}
// hashmap比较函数：比较两个sock_ctx的fd
int _evpub_sockel_compare(const void *a, const void *b, void *ud) {
    (void)ud;
    SOCKET fa = (*(const sock_ctx **)a)->fd;
    SOCKET fb = (*(const sock_ctx **)b)->fd;
    return (fa < fb) ? -1 : (fa > fb) ? 1 : 0; // 三路比较，避免 UINT_PTR 相减截断为 int 溢出
}
sock_ctx *_evpub_sockel_get(watcher_ctx *watcher, SOCKET fd) {
    sock_ctx key;
    key.fd = fd;
    sock_ctx *pkey = &key;
    void **tmp = (void **)hashmap_get(watcher->element, &pkey);
    return NULL == tmp ? NULL : *tmp;
}
void _evpub_sockel_add(watcher_ctx *watcher, sock_ctx *skctx) {
    ASSERTAB(NULL == hashmap_set(watcher->element, &skctx), "socket repeat.");
    ASSERTAB(!hashmap_oom(watcher->element), "hashmap oom.");
}
void *_evpub_sockel_remove(watcher_ctx *watcher, SOCKET fd) {
    if (INVALID_SOCK == fd) {
        return NULL;
    }
    sock_ctx key;
    key.fd = fd;
    sock_ctx *pkey = &key;
    return (void *)hashmap_delete(watcher->element, &pkey);
}
void _evpub_off_buf_release(off_buf_ctx *buf) {
    if (NULL == buf->shared) {
        FREE(buf->data);
        return;
    }
    // 多播路径：N 个 buf 共享 shared->data，最后一个释放方才 FREE
    if (1 == ATOMIC_ADD(&buf->shared->ref, -1)) {
        FREE(buf->shared->data);
        FREE(buf->shared);
    }
    buf->data = NULL;
    buf->shared = NULL;
}
void _evpub_off_buf_clear(queue_ctx *bufs) {
    off_buf_ctx *buf;
    while (NULL != (buf = queue_pop(bufs))) {
        _evpub_off_buf_release(buf);
    }
    queue_clear(bufs);
}
void _evpub_sendto_clear(queue_ctx *bufs) {
    sendto_ctx *buf;
    while (NULL != (buf = queue_pop(bufs))) {
        FREE(buf->data);
    }
    queue_clear(bufs);
}
int32_t _evpub_nodelay_nonblock(SOCKET fd) {
    if (ERR_OK != sock_nodelay(fd)
        || ERR_OK != sock_nonblock(fd)) {
        return ERR_FAILED;
    }
    return ERR_OK;
}
SOCKET _evpub_create_sock(int32_t type, int32_t family) {
#ifdef EV_IOCP
    return WSASocket(family, type, SOCK_STREAM == type ? IPPROTO_TCP : IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED);
#else
    return socket(family, type, 0);
#endif
}
SOCKET _evpub_listen(netaddr_ctx *addr) {
    SOCKET fd = _evpub_create_sock(SOCK_STREAM, netaddr_family(addr));
    if (INVALID_SOCK == fd) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return INVALID_SOCK;
    }
    if (ERR_OK != sock_reuseaddr(fd)
        || ERR_OK != sock_reuseport(fd)
        || ERR_OK != sock_nonblock(fd)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        CLOSE_SOCK(fd);
        return INVALID_SOCK;
    }
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
SOCKET _evpub_udp(netaddr_ctx *addr) {
    SOCKET fd = _evpub_create_sock(SOCK_DGRAM, netaddr_family(addr));
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
    if (ERR_OK != sock_reuseaddr(fd)
        || ERR_OK != sock_nonblock(fd)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        CLOSE_SOCK(fd);
        return INVALID_SOCK;
    }
    if (ERR_OK != bind(fd, netaddr_addr(addr), netaddr_size(addr))) {
        CLOSE_SOCK(fd);
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return INVALID_SOCK;
    }
    return fd;
}
#if WITH_SSL
// SSL不支持scatter I/O，每次只能读一个TLS记录，外层循环驱动重复调用直到socket耗尽
static inline int32_t _evpub_sock_read_ssl(SSL *ssl, IOV_TYPE *iov, size_t *nread) {
    return evssl_read(ssl, iov[0].IOV_PTR_FIELD, (size_t)iov[0].IOV_LEN_FIELD, nread);
}
#endif
// 从socket普通读取（使用readv/WSARecv）
static int32_t _evpub_sock_read_normal(SOCKET fd, IOV_TYPE *iov, uint32_t niov, size_t *readed) {
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
    ssize_t rtn = readv(fd, iov, niov);
    if (rtn > 0) {
        *readed = (size_t)rtn;
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
int32_t _evpub_sock_read(SOCKET fd, IOV_TYPE *iov, uint32_t niov, void *arg, size_t *readed) {
    *readed = 0;
#if WITH_SSL
    if (NULL == arg) {
        return _evpub_sock_read_normal(fd, iov, niov, readed);
    }
    /* Force niov=1: SSL_read reads one TLS record at a time into a single buffer */
    return _evpub_sock_read_ssl((SSL *)arg, iov, readed);
#else
    (void)arg;
    return _evpub_sock_read_normal(fd, iov, niov, readed);
#endif
}
// 将发送队列中的前N个缓冲区填充到iov数组，返回实际填充数量（最多MAX_SEND_NIOV，总大小不超过MAX_SEND_SIZE）
static uint32_t _evpub_off_buf_fill_iov(queue_ctx *buf_s, size_t nbuf,
                                        IOV_TYPE iov[MAX_SEND_NIOV],
                                        off_buf_ctx *sndbuf[MAX_SEND_NIOV]) {
    if (nbuf > MAX_SEND_NIOV) {
        nbuf = MAX_SEND_NIOV;
    }
    size_t total = 0;
    uint32_t cnt = 0;
    off_buf_ctx *buf;
    for (uint32_t i = 0; i < (uint32_t)nbuf; i++) {
        buf = queue_at(buf_s, i);
        iov[i].IOV_PTR_FIELD = ((char *)buf->data) + buf->offset;
        iov[i].IOV_LEN_FIELD = (IOV_LEN_TYPE)(buf->lens - buf->offset);
        sndbuf[i] = buf;
        total += (size_t)iov[i].IOV_LEN_FIELD;
        cnt++;
        if (total >= MAX_SEND_SIZE) {
            break;
        }
    }
    return cnt;
}
// 根据实际发送字节数sent，从发送队列头部消费已完成的缓冲区，更新offset或弹出并释放
static void _evpub_off_buf_apply_sent(queue_ctx *buf_s, off_buf_ctx *sndbuf[MAX_SEND_NIOV],
                                      uint32_t niov, size_t sent) {
    for (uint32_t i = 0; i < niov && sent > 0; i++) {
        off_buf_ctx *buf = sndbuf[i];
        size_t buflen = buf->lens - buf->offset;
        if (sent >= buflen) {
            sent -= buflen;
            _evpub_off_buf_release(buf);
            queue_pop(buf_s);
        } else {
            buf->offset += sent;
            sent = 0;
        }
    }
}
// 调用writev/WSASend发送iov数组中的数据，返回ERR_OK并更新sended
static int32_t _evpub_sock_send_iov(SOCKET fd, IOV_TYPE *iov, uint32_t niov, size_t *sended) {
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
    ssize_t rtn = writev(fd, iov, (int)niov);
    if (rtn < 0) {
        if (!ERR_RW_RETRIABLE(ERRNO)) {
            return ERR_FAILED;
        }
        return ERR_OK;
    }
    *sended = (size_t)rtn;
    return ERR_OK;
#endif
}
// 循环发送队列中所有普通（非SSL）数据，直到队列空或发生错误
static int32_t _evpub_sock_send_normal(SOCKET fd, queue_ctx *buf_s, size_t *nsend) {
    int32_t rtn = ERR_OK;
    size_t nbuf, sended;
    uint32_t niov;
    IOV_TYPE iov[MAX_SEND_NIOV];
    off_buf_ctx *sndbuf[MAX_SEND_NIOV];
    while (0 != (nbuf = queue_size(buf_s))) {
        niov = _evpub_off_buf_fill_iov(buf_s, nbuf, iov, sndbuf);
        rtn = _evpub_sock_send_iov(fd, iov, niov, &sended);
        if (ERR_OK == rtn) {
            *nsend += sended;
            _evpub_off_buf_apply_sent(buf_s, sndbuf, niov, sended);
            if (0 == sended) {
                break;
            }
        } else {
            break;
        }
    }
    return rtn;
}
#if WITH_SSL
// 通过SSL逐条发送队列中的数据（SSL_write每次只能发一条记录）
static int32_t _evpub_sock_send_ssl(SSL *ssl, queue_ctx *buf_s, size_t *nsend) {
    int32_t rtn = ERR_OK;
    size_t sended;
    off_buf_ctx *buf;
    for (;;) {
        buf = queue_peek(buf_s);
        if (NULL == buf) {
            break;
        }
        rtn = evssl_send(ssl, (char *)buf->data + buf->offset, buf->lens - buf->offset, &sended);
        if (ERR_OK == rtn) {
            (*nsend) += sended;
            buf->offset += sended;
            if (buf->offset == buf->lens) {
                queue_pop(buf_s);
                _evpub_off_buf_release(buf);
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
int32_t _evpub_sock_send(SOCKET fd, queue_ctx *buf_s, size_t *nsend, void *arg) {
    *nsend = 0;
#if WITH_SSL
    if (NULL == arg) {
        return _evpub_sock_send_normal(fd, buf_s, nsend);
    }
    return _evpub_sock_send_ssl((SSL *)arg, buf_s, nsend);
#else
    (void)arg;
    return _evpub_sock_send_normal(fd, buf_s, nsend);
#endif
}
