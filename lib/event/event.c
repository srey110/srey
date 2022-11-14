#include "event/event.h"
#include "netutils.h"
#include "loger.h"

void _qu_bufs_clear(qu_bufs *bufs)
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
        || ERR_OK != sock_kpa(fd, SOCKKPA_DELAY, SOCKKPA_INTVL)
        || ERR_OK != sock_nbio(fd))
    {
        return ERR_FAILED;
    }
    return ERR_OK;
}
SOCKET _create_sock(int32_t family)
{
#ifdef OS_WIN
    return WSASocket(family, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
#else
    return socket(family, SOCK_STREAM, 0);
#endif
}
SOCKET _listen(netaddr_ctx *addr)
{
    SOCKET fd = _create_sock(netaddr_family(addr));
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
int32_t _sock_read(SOCKET fd, void *buf, size_t len, void *arg)
{
    int32_t rtn = recv(fd, (char*)buf, (int32_t)len, 0);
    if (0 == rtn)
    {
        return ERR_FAILED;
    }
    if (rtn < 0)
    {
        if (!IS_EAGAIN(ERRNO))
        {
            return ERR_FAILED;
        }
        return ERR_OK;
    }
    return rtn;
}
int32_t _sock_send(SOCKET fd, qu_bufs *sendbufs, size_t *nsend, void *arg)
{
    *nsend = 0;
    int32_t err = ERR_OK;
    int32_t rtn, size;
    bufs_ctx *buf;
    while (NULL != (buf = qu_bufs_peek(sendbufs)))
    {
        size = (int32_t)(buf->len - buf->offset);
        rtn = send(fd, (char*)buf->data + buf->offset, size, 0);
        if (0 == rtn)
        {
            err = ERR_FAILED;
            break;
        }
        if (0 > rtn)
        {
            if (!IS_EAGAIN(ERRNO))
            {
                err = ERR_FAILED;
            }
            break;
        }
        (*nsend) += rtn;
        if (rtn < size)
        {
            buf->offset += rtn;
            break;
        }
        FREE(buf->data);
        qu_bufs_pop(sendbufs);
    }
    return err;
}
