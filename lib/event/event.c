#include "event/event.h"
#include "netutils.h"
#include "loger.h"

void _bufs_clear(qu_bufs *bufs)
{
    bufs_ctx *buf;
    while (NULL != (buf = qu_bufs_pop(bufs)))
    {
        FREE(buf->data);
    }
    qu_bufs_clear(bufs);
}
int32_t _set_sockops(SOCKET fd)
{
    if (ERR_OK != sock_linger(fd)
        || ERR_OK != sock_nodelay(fd)
        || ERR_OK != sock_nbio(fd))
    {
        return ERR_FAILED;
    }
    return ERR_OK;
}
SOCKET _create_sock(int32_t type, int32_t family)
{
#ifdef EV_IOCP
    return WSASocket(family, type, SOCK_STREAM == type ? IPPROTO_TCP : IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED);
#else
    return socket(family, type, 0);
#endif
}
SOCKET _listen(netaddr_ctx *addr)
{
    SOCKET fd = _create_sock(SOCK_STREAM, netaddr_family(addr));
    if (INVALID_SOCK == fd)
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return INVALID_SOCK;
    }
    sock_raddr(fd);
    sock_rport(fd);
    sock_nbio(fd);
    if (ERR_OK != bind(fd, netaddr_addr(addr), netaddr_size(addr)))
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        CLOSE_SOCK(fd);
        return INVALID_SOCK;
    }
    if (ERR_OK != listen(fd, SOMAXCONN))
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        CLOSE_SOCK(fd);
        return INVALID_SOCK;
    }
    return fd;
}
SOCKET _udp(netaddr_ctx *addr)
{
    SOCKET fd = _create_sock(SOCK_DGRAM, netaddr_family(addr));
    if (INVALID_SOCK == fd)
    {
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
                 NULL) < ERR_OK)
    {
        CLOSE_SOCK(fd);
        LOG_ERROR("WSAIoctl(%d, SIO_UDP_CONNRESET...) failed. %s", (int32_t)fd, ERRORSTR(ERRNO));
        return INVALID_SOCK;
    }
#endif
    sock_raddr(fd);
    sock_nbio(fd);
    if (ERR_OK != bind(fd, netaddr_addr(addr), netaddr_size(addr)))
    {
        CLOSE_SOCK(fd);
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return INVALID_SOCK;
    }
    return fd;
}
#if WITH_SSL
static inline int32_t _sock_read_ssl(SSL *ssl, IOV_TYPE *iov, uint32_t niov)
{
    int32_t rtn, nread = 0;
    for (uint32_t i = 0; i < niov; i++)
    {
        rtn = evssl_read(ssl, iov[i].IOV_PTR_FIELD, (size_t)iov[i].IOV_LEN_FIELD);
        if (rtn > 0)
        {
            nread += rtn;
            if (rtn < (int32_t)iov[i].IOV_LEN_FIELD)
            {
                return nread;
            }
            continue;
        }
        return rtn;
    }
    return nread;
}
#endif
static inline int32_t _sock_read_normal(SOCKET fd, IOV_TYPE *iov, uint32_t niov)
{
#ifdef EV_IOCP 
    DWORD bytes, flags = 0;
    if (SOCKET_ERROR != WSARecv(fd, 
                                iov, 
                                niov, 
                                &bytes, 
                                &flags, 
                                NULL, 
                                NULL))
    {
        if (bytes > 0)
        {
            return (int32_t)bytes;
        }
        return ERR_FAILED;
    }
    if (!IS_EAGAIN(ERRNO))
    {
        return ERR_FAILED;
    }
    return ERR_OK;
#else
    int32_t nread = (int32_t)readv(fd, iov, niov);
    if (nread > 0)
    {
        return nread;
    }
    if (0 == nread)
    {
        return ERR_FAILED;
    }
    if (nread < 0)
    {
        if (!ERR_RW_RETRIABLE(ERRNO))
        {
            return ERR_FAILED;
        }
    }
    return ERR_OK;
#endif
}
int32_t _sock_read(SOCKET fd, IOV_TYPE *iov, uint32_t niov, void *arg)
{
#if WITH_SSL
    if (NULL == arg)
    {
        return _sock_read_normal(fd, iov, niov);
    }
    return _sock_read_ssl((SSL *)arg, iov, niov);
#else
    return _sock_read_normal(fd, iov, niov);
#endif
}
static inline void _bufs_rdlock(rwlock_ctx *rwlck)
{
#ifdef EV_IOCP
    rwlock_rdlock(rwlck);
#endif
}
static inline void _bufs_wrlock(rwlock_ctx *rwlck)
{
#ifdef EV_IOCP
    rwlock_wrlock(rwlck);
#endif
}
static inline void _bufs_unlock(rwlock_ctx *rwlck)
{
#ifdef EV_IOCP
    rwlock_unlock(rwlck);
#endif
}
static inline size_t _bufs_size(qu_bufs *buf_s, rwlock_ctx *rwlck)
{
    _bufs_rdlock(rwlck);
    size_t size = qu_bufs_size(buf_s);
    _bufs_unlock(rwlck);
    return size;
}
static inline uint32_t _bufs_fill_iov(qu_bufs *buf_s, rwlock_ctx *rwlck, size_t bufsize, IOV_TYPE iov[MAX_SEND_NIOV])
{
    if (bufsize > MAX_SEND_NIOV)
    {
        bufsize = MAX_SEND_NIOV;
    }
    bufs_ctx *buf;
    size_t total = 0;
    uint32_t cnt = 0;
    for (size_t i = 0; i < bufsize; i++)
    {
        _bufs_rdlock(rwlck);
        buf = qu_bufs_at(buf_s, i);
        iov[i].IOV_PTR_FIELD = ((char *)buf->data) + buf->offset;
        iov[i].IOV_LEN_FIELD = (IOV_LEN_TYPE)(buf->len - buf->offset);
        _bufs_unlock(rwlck);
        total += (size_t)iov[i].IOV_LEN_FIELD;
        cnt++;
        if (total >= MAX_SEND_SIZE)
        {
            break;
        }
    }
    return cnt;
}
static inline void _bufs_size_del(qu_bufs *buf_s, rwlock_ctx *rwlck, size_t len)
{
    if (0 == len)
    {
        return;
    }
    size_t buflen;
    bufs_ctx *buf;
    while (len > 0)
    {
        _bufs_wrlock(rwlck);
        buf = qu_bufs_peek(buf_s);
        buflen = buf->len - buf->offset;
        if (len >= buflen)
        {
            FREE(buf->data);
            len -= buflen;
            qu_bufs_pop(buf_s);
        }
        else
        {
            buf->offset += len;
            len = 0;
        }
        _bufs_unlock(rwlck);
    }
}
static inline int32_t _sock_send_iov(SOCKET fd, IOV_TYPE *iov, uint32_t niov)
{
#ifdef EV_IOCP
    DWORD bytes;
    if (SOCKET_ERROR != WSASend(fd,
                                iov, 
                                niov, 
                                &bytes,
                                0, 
                                NULL, 
                                NULL))
    {
        return (int32_t)bytes;
    }
    if (!IS_EAGAIN(ERRNO))
    {
        return ERR_FAILED;
    }
    return ERR_OK;
#else
    int32_t nsend = (int32_t)writev(fd, iov, (int32_t)niov);
    if (nsend >= 0)
    {
        return nsend;
    }
    if (nsend < 0)
    {
        if (!ERR_RW_RETRIABLE(ERRNO))
        {
            return ERR_FAILED;
        }
    }
    return ERR_OK;
#endif
}
static inline int32_t _sock_send_normal(SOCKET fd, qu_bufs *buf_s, rwlock_ctx *rwlck, size_t *nsend)
{
    int32_t rtn = ERR_OK;
    size_t bufsize;
    uint32_t niov;
    IOV_TYPE iov[MAX_SEND_NIOV];
    while (0 != (bufsize = _bufs_size(buf_s, rwlck)))
    {
        niov = _bufs_fill_iov(buf_s, rwlck, bufsize, iov);
        rtn = _sock_send_iov(fd, iov, niov);
        if (rtn > 0)
        {
            *nsend += rtn;
            _bufs_size_del(buf_s, rwlck, rtn);
            rtn = ERR_OK;
        }
        else
        {
            break;
        }
    }
    return rtn;
}
#if WITH_SSL
static inline int32_t _sock_send_ssl(SSL *ssl, qu_bufs *buf_s, rwlock_ctx *rwlck, size_t *nsend)
{
    int32_t rtn, err = ERR_OK;
    bufs_ctx buf, *tmp;
    for (;;)
    {
        _bufs_rdlock(rwlck);
        tmp = qu_bufs_peek(buf_s);
        if (NULL != tmp)
        {
            buf = *tmp;
        }
        _bufs_unlock(rwlck);
        if (NULL == tmp)
        {
            break;
        }
        rtn = evssl_send(ssl, (char *)buf.data + buf.offset, buf.len - buf.offset);
        if (rtn > 0)
        {
            (*nsend) += rtn;
            buf.offset += rtn;
            if (buf.offset == buf.len)
            {
                _bufs_wrlock(rwlck);
                qu_bufs_pop(buf_s);
                _bufs_unlock(rwlck);
                FREE(buf.data);
                continue;
            }
            else
            {
                break;
            }
        }
        err = rtn;
        break;
    }
    return err;
}
#endif
int32_t _sock_send(SOCKET fd, qu_bufs *buf_s, rwlock_ctx *rwlck, size_t *nsend, void *arg)
{
    *nsend = 0;
#if WITH_SSL
    if (NULL == arg)
    {
        return _sock_send_normal(fd, buf_s, rwlck, nsend);
    }
    return _sock_send_ssl((SSL *)arg, buf_s, rwlck, nsend);
#else
    return _sock_send_normal(fd, buf_s, rwlck, nsend);
#endif
}
