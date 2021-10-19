#include "netutils.h"
#include "netaddr.h"

void socknodelay(SOCKET fd)
{
    int32_t iflag = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *)&iflag, (int32_t)sizeof(iflag)) < ERR_OK)
    {
        PRINTF("setsockopt(%d, IPPROTO_TCP, TCP_NODELAY, %d, %d) failed. %s",
            (int32_t)fd, iflag, (int32_t)sizeof(iflag), ERRORSTR(ERRNO));
    }
}
void socknbio(SOCKET fd)
{
#if defined(OS_WIN)
    u_long ulflag = 1;
    if (ioctlsocket(fd, FIONBIO, &ulflag) < ERR_OK)
    {
        PRINTF("ioctlsocket(%d, FIONBIO, 1) failed. %s", (int32_t)fd, ERRORSTR(ERRNO));
    }
#else
    int32_t iflag = fcntl(fd, F_GETFL, NULL);
    if (ERR_FAILED == iflag)
    {
        PRINTF("fcntl(%d, F_GETFL, NULL) failed.", fd);
        return;
    }
    if (!(iflag & O_NONBLOCK))
    {
        if (ERR_FAILED == fcntl(fd, F_SETFL, iflag | O_NONBLOCK))
        {
            PRINTF("fcntl(%d, F_SETFL, %d) failed", fd, iflag | O_NONBLOCK);
            return;
        }
    }
#endif
}
void sockraddr(SOCKET fd)
{
    int32_t iflag = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&iflag, (int32_t)sizeof(iflag)) < ERR_OK)
    {
        PRINTF("setsockopt(%d, SOL_SOCKET, SO_REUSEADDR, %d, %d) failed. %s",
            (int32_t)fd, iflag, (int32_t)sizeof(iflag), ERRORSTR(ERRNO));
    }
}
int32_t checkrport()
{
#ifdef SO_REUSEPORT
    return ERR_OK;
#else
    return ERR_FAILED;
#endif
}
void sockrport(SOCKET lsfd)
{
#ifdef SO_REUSEPORT
    int32_t iflag = 1;
   if (setsockopt(lsfd, SOL_SOCKET, SO_REUSEPORT, &iflag, sizeof(iflag)) < ERR_OK)
   {
       PRINTF("setsockopt(%d, SOL_SOCKET, SO_REUSEPORT, %d, %d) failed. %s",
           lsfd, iflag, (int32_t)sizeof(iflag), ERRORSTR(ERRNO));
   }
#endif 
}
void sockkpa(SOCKET fd, const int32_t idelay, const int32_t iintvl)
{
    int32_t iflag = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (char *)&iflag, (int32_t)sizeof(iflag)) < ERR_OK)
    {
        PRINTF("setsockopt(%d, SOL_SOCKET, SO_KEEPALIVE, %d, %d) failed. %s", 
            (int32_t)fd, iflag, (int32_t)sizeof(iflag), ERRORSTR(ERRNO));
        return;
    }
    if (0 >= idelay)
    {
        return;
    }

#if defined(OS_WIN)
    struct tcp_keepalive stkpa;
    struct tcp_keepalive stout;
    DWORD ulret = 0;
    stkpa.keepalivetime = idelay * MSEC;
    stkpa.keepaliveinterval = iintvl * MSEC;
    if (WSAIoctl(fd, SIO_KEEPALIVE_VALS, (LPVOID)&stkpa, sizeof(struct tcp_keepalive),
        (LPVOID)&stout, sizeof(struct tcp_keepalive), &ulret, NULL, NULL) < ERR_OK)
    {
        PRINTF("WSAIoctl(%d, SIO_KEEPALIVE_VALS...) failed. %s", (int32_t)fd, ERRORSTR(ERRNO));
    }
#else

#ifdef TCP_KEEPIDLE
    int32_t icnt = 5;
    //多久后发送keepalive 秒
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idelay, sizeof(idelay)) < ERR_OK)
    {
        PRINTF("setsockopt(%d, IPPROTO_TCP, TCP_KEEPIDLE, %d, %d) failed. %s",
            fd, idelay, (int32_t)sizeof(idelay), ERRORSTR(ERRNO));
        return;
    }
    //时间间隔
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &iintvl, sizeof(iintvl)) < ERR_OK)
    {
        PRINTF("setsockopt(%d, IPPROTO_TCP, TCP_KEEPINTVL, %d, %d) failed. %s",
            fd, iintvl, (int32_t)sizeof(iintvl), ERRORSTR(ERRNO));
        return;
    }
    //重试次数
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &icnt, sizeof(icnt)) < ERR_OK)
    {
        PRINTF("setsockopt(%d, IPPROTO_TCP, TCP_KEEPCNT, %d, %d) failed. %s",
            fd, icnt, (int32_t)sizeof(icnt), ERRORSTR(ERRNO));
    }
    return;
#endif

#if defined(TCP_KEEPALIVE) && !defined(OS_SUN)
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &idelay, sizeof(idelay)) < ERR_OK)
    {
        PRINTF("setsockopt(%d, IPPROTO_TCP, TCP_KEEPALIVE, %d, %d) failed. %s",
            fd, idelay, (int32_t)sizeof(idelay), ERRORSTR(ERRNO));
    }
#endif

#endif
}
SOCKET _socklsn(const char *pip, const uint16_t usport, const int32_t ibacklog)
{
    if (0 == ibacklog)
    {
        return INVALID_SOCK;
    }

    struct netaddr_ctx addr;
    if (ERRNO != netaddr_sethost(&addr, pip, usport))
    {
        return INVALID_SOCK;
    }

    SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
    if (INVALID_SOCK == fd)
    {
        PRINTF("socket(AF_INET, SOCK_STREAM, 0) failed. %s", ERRORSTR(ERRNO));
        return INVALID_SOCK;
    }
    if (ERR_OK != bind(fd, netaddr_addr(&addr), netaddr_size(&addr)))
    {
        PRINTF("bind(%d, ...) failed. %s", (int32_t)fd, ERRORSTR(ERRNO));
        SAFE_CLOSESOCK(fd);
        return INVALID_SOCK;
    }
    if (ERR_OK != listen(fd, ibacklog))
    {
        PRINTF("listen(%d, %d) failed. %s", (int32_t)fd, ibacklog, ERRORSTR(ERRNO));
        SAFE_CLOSESOCK(fd);
        return INVALID_SOCK;
    }

    return fd;
}
SOCKET _sockcnt(const char *ip, const uint16_t port)
{
    struct netaddr_ctx addr;
    if (ERR_OK != netaddr_sethost(&addr, ip, port))
    {
        return INVALID_SOCK;
    }

    SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
    if (INVALID_SOCK == fd)
    {
        PRINTF("socket(AF_INET, SOCK_STREAM, 0) failed. %s", ERRORSTR(ERRNO));
        return INVALID_SOCK;
    }
    if (ERR_OK != connect(fd, netaddr_addr(&addr), netaddr_size(&addr)))
    {
        PRINTF("connect(%d, ...) failed. %s", (int32_t)fd, ERRORSTR(ERRNO));
        SAFE_CLOSESOCK(fd);
        return INVALID_SOCK;
    }

    return fd;
}
int32_t sockpair(SOCKET acSock[2])
{
    SOCKET fdlsn = _socklsn("127.0.0.1", 0, 1);
    if (INVALID_SOCK == fdlsn)
    {
        return ERR_FAILED;
    }

    struct netaddr_ctx addr;
    if (ERR_OK != netaddr_localaddr(&addr, fdlsn))
    {
        SAFE_CLOSESOCK(fdlsn);
        return ERR_FAILED;
    }
    char acip[64];
    if (ERR_OK != netaddr_ip(&addr, acip))
    {
        SAFE_CLOSESOCK(fdlsn);
        return ERR_FAILED;
    }

    SOCKET fdcn = _sockcnt(acip, netaddr_port(&addr));
    if (INVALID_SOCK == fdcn)
    {
        SAFE_CLOSESOCK(fdlsn);
        return ERR_FAILED;
    }

    struct sockaddr_in listen_addr;
    socklen_t isize = (socklen_t)sizeof(listen_addr);
    SOCKET fdacp = accept(fdlsn, (struct sockaddr *) &listen_addr, &isize);
    if (INVALID_SOCK == fdacp)
    {
        PRINTF("accept(%d, ...) failed. %s", (int32_t)fdlsn, ERRORSTR(ERRNO));
        SAFE_CLOSESOCK(fdlsn);
        SAFE_CLOSESOCK(fdcn);
        return ERR_FAILED;
    }
    SAFE_CLOSESOCK(fdlsn);
    if (ERR_OK != netaddr_localaddr(&addr, fdcn))
    {
        SAFE_CLOSESOCK(fdacp);
        SAFE_CLOSESOCK(fdcn);
        return ERR_FAILED;
    }
    struct sockaddr_in *connect_addr = (struct sockaddr_in*)netaddr_addr(&addr);
    if (listen_addr.sin_family != connect_addr->sin_family
        || listen_addr.sin_addr.s_addr != connect_addr->sin_addr.s_addr
        || listen_addr.sin_port != connect_addr->sin_port)
    {
        SAFE_CLOSESOCK(fdacp);
        SAFE_CLOSESOCK(fdcn);
        return ERR_FAILED;
    }

    socknodelay(fdacp);
    socknodelay(fdcn);
    socknbio(fdacp);
    socknbio(fdcn);

    acSock[0] = fdacp;
    acSock[1] = fdcn;

    return ERR_OK;
}
