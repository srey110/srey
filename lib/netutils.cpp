#include "netutils.h"
#include "netaddr.h"
#include "errcode.h"

SREY_NS_BEGIN

int32_t socknread(const SOCKET &fd)
{
#ifdef OS_WIN
    unsigned long ulread = INIT_NUMBER;
    if (ioctlsocket(fd, FIONREAD, &ulread) < 0)
    {
        return ERR_FAILED;
    }

    return (int32_t)ulread;
#else
    int32_t iread = INIT_NUMBER;
    if (ioctl(fd, FIONREAD, &iread) < 0)
    {
        return ERR_FAILED;
    }

    return iread;
#endif
}
//int32_t sockrecv(const SOCKET &fd, class cchainbuffer *pbuf)
//{
//    return 0;
//}
SOCKET socklsn(const char *ip, const uint16_t &port, const int32_t &backlog)
{
    if (INIT_NUMBER == backlog)
    {
        return INVALID_SOCK;
    }

    cnetaddr addr;
    if (!addr.setaddr(ip, port))
    {
        return INVALID_SOCK;
    }

    SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
    if (INVALID_SOCK == fd)
    {
        return INVALID_SOCK;
    }
    if (ERR_OK != bind(fd, addr.getaddr(), (int32_t)addr.getsize()))
    {
        SAFE_CLOSESOCK(fd);
        return INVALID_SOCK;
    }
    if (ERR_OK != listen(fd, (-1 == backlog) ? 128 : backlog))
    {
        SAFE_CLOSESOCK(fd);
        return INVALID_SOCK;
    }

    return fd;
}
SOCKET sockcnt(const char *ip, const uint16_t &port)
{
    cnetaddr addr;
    if (!addr.setaddr(ip, port))
    {
        return INVALID_SOCK;
    }

    SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
    if (INVALID_SOCK == fd)
    {
        return INVALID_SOCK;
    }
    if (ERR_OK != connect(fd, addr.getaddr(), (int32_t)addr.getsize()))
    {
        SAFE_CLOSESOCK(fd);
        return INVALID_SOCK;
    }

    return fd;
}
void sockopts(SOCKET &fd)
{
    int nodelay = 1;
    int keepalive = 1;

    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *)&nodelay, (int)sizeof(nodelay));
    (void)setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (char *)&keepalive, (int)sizeof(keepalive));

#ifdef OS_WIN
    unsigned long nonblocking = 1;
    (void)ioctlsocket(fd, FIONBIO, &nonblocking);
#else
    int flags = fcntl(fd, F_GETFL, NULL);
    if (ERR_FAILED == flags)
    {
        PRINTF("fcntl(%d, F_GETFL) error.", fd);
        return;
    }
    if (!(flags & O_NONBLOCK))
    {
        if (ERR_FAILED == fcntl(fd, F_SETFL, flags | O_NONBLOCK))
        {
            PRINTF("fcntl(%d, F_SETFL)", fd);
            return;
        }
    }
#endif
}
bool sockpair(SOCKET acSock[2])
{
    SOCKET fdlsn = socklsn("127.0.0.1", 0, 1);
    if (INVALID_SOCK == fdlsn)
    {
        return false;
    }

    cnetaddr addr;
    if (!addr.setlocaddr(fdlsn))
    {
        SAFE_CLOSESOCK(fdlsn);
        return false;
    }
    SOCKET fdcn = sockcnt(addr.getip().c_str(), addr.getport());
    if (INVALID_SOCK == fdcn)
    {
        SAFE_CLOSESOCK(fdlsn);
        return false;
    }

    struct sockaddr_in listen_addr;
    int32_t isize = sizeof(listen_addr);
    SOCKET fdacp = accept(fdlsn, (struct sockaddr *) &listen_addr, (socklen_t*)&isize);
    if (INVALID_SOCK == fdacp)
    {
        SAFE_CLOSESOCK(fdlsn);
        SAFE_CLOSESOCK(fdcn);
        return false;
    }
    SAFE_CLOSESOCK(fdlsn);
    if (!addr.setlocaddr(fdcn))
    {
        SAFE_CLOSESOCK(fdacp);
        SAFE_CLOSESOCK(fdcn);
        return false;
    }
    struct sockaddr_in *connect_addr = (sockaddr_in*)addr.getaddr();
    if (listen_addr.sin_family != connect_addr->sin_family
        || listen_addr.sin_addr.s_addr != connect_addr->sin_addr.s_addr
        || listen_addr.sin_port != connect_addr->sin_port)
    {
        SAFE_CLOSESOCK(fdacp);
        SAFE_CLOSESOCK(fdcn);
        return false;
    }

    sockopts(fdacp);
    sockopts(fdcn);
    acSock[0] = fdacp;
    acSock[1] = fdcn;

    return true;
}

SREY_NS_END
