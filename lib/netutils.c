#include "netutils.h"
#include "netaddr.h"

int32_t socknread(SOCKET fd)
{
#if defined(OS_WIN)
    u_long ulread = 0;
    if (ioctlsocket(fd, FIONREAD, &ulread) < ERR_OK)
    {
        return ERR_FAILED;
    }
    return (int32_t)ulread;
#else
    int32_t iread = 0;
    if (ioctl(fd, FIONREAD, &iread) < ERR_OK)
    {
        return ERR_FAILED;
    }
    return iread;
#endif
}
int32_t socktype(SOCKET fd)
{
    int32_t itype = 0;
    int32_t ilen = (int32_t)sizeof(itype);
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, (char *)&itype, (socklen_t*)&ilen) < ERR_OK)
    {
        PRINTF("getsockopt(%d, ...) failed. %s", (int32_t)fd, ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    return itype;
}
int32_t sockaddrfamily(SOCKET fd)
{
#if defined(OS_WIN)
    WSAPROTOCOL_INFO info;
    int32_t ilens = (int32_t)sizeof(info);
    if (getsockopt(fd, SOL_SOCKET, SO_PROTOCOL_INFO, (char *)&info, &ilens) < ERR_OK)
    {
        PRINTF("getsockopt(%d, SOL_SOCKET, SO_PROTOCOL_INFO, ...) failed. %s", (int32_t)fd, ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    return info.iAddressFamily;
#else
#ifdef SO_DOMAIN
    int32_t ifamily = 0;
    int32_t ilens = (int32_t)sizeof(ifamily);
    if (getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &ifamily, (socklen_t*)&ilens) < 0)
    {
        PRINTF("getsockopt(%d, SOL_SOCKET, SO_DOMAIN, ...) failed. %s", (int32_t)fd, ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    return ifamily;
#endif
    return AF_INET;
#endif
}
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
   if (setsockopt(lsfd, SOL_SOCKET, SO_REUSEPORT, (char *)&iflag, (int32_t)sizeof(iflag)) < ERR_OK)
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
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, (char *)&idelay, (int32_t)sizeof(idelay)) < ERR_OK)
    {
        PRINTF("setsockopt(%d, IPPROTO_TCP, TCP_KEEPIDLE, %d, %d) failed. %s",
            fd, idelay, (int32_t)sizeof(idelay), ERRORSTR(ERRNO));
        return;
    }
    //时间间隔
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, (char *)&iintvl, (int32_t)sizeof(iintvl)) < ERR_OK)
    {
        PRINTF("setsockopt(%d, IPPROTO_TCP, TCP_KEEPINTVL, %d, %d) failed. %s",
            fd, iintvl, (int32_t)sizeof(iintvl), ERRORSTR(ERRNO));
        return;
    }
    //重试次数
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, (char *)&icnt, (int32_t)sizeof(icnt)) < ERR_OK)
    {
        PRINTF("setsockopt(%d, IPPROTO_TCP, TCP_KEEPCNT, %d, %d) failed. %s",
            fd, icnt, (int32_t)sizeof(icnt), ERRORSTR(ERRNO));
    }
    return;
#endif

#if defined(TCP_KEEPALIVE) && !defined(OS_SUN)
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, (char *)&idelay, (int32_t)sizeof(idelay)) < ERR_OK)
    {
        PRINTF("setsockopt(%d, IPPROTO_TCP, TCP_KEEPALIVE, %d, %d) failed. %s",
            fd, idelay, (int32_t)sizeof(idelay), ERRORSTR(ERRNO));
    }
#endif

#endif
}
void closereset(SOCKET fd)
{
    struct linger stlg = { 1, 0 };
    if (setsockopt(fd, SOL_SOCKET, SO_LINGER, (char *)&stlg, (int32_t)sizeof(stlg)) < ERR_OK)
    {
        PRINTF("setsockopt(%d, SOL_SOCKET, SO_LINGER, 1 0) failed. %s",
            (int32_t)fd, ERRORSTR(ERRNO));
    }
}
SOCKET _sock_listen()
{
    union netaddr_ctx addr;
    if (ERR_OK != netaddr_sethost(&addr, "127.0.0.1", 0))
    {
        PRINTF("%s", ERRORSTR(ERRNO));
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
        SOCK_CLOSE(fd);
        return INVALID_SOCK;
    }
    if (ERR_OK != listen(fd, 1))
    {
        PRINTF("listen(%d, 1) failed. %s", (int32_t)fd, ERRORSTR(ERRNO));
        SOCK_CLOSE(fd);
        return INVALID_SOCK;
    }
    return fd;
}
SOCKET _sockcnt(union netaddr_ctx *paddr)
{
    SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
    if (INVALID_SOCK == fd)
    {
        PRINTF("socket(AF_INET, SOCK_STREAM, 0) failed. %s", ERRORSTR(ERRNO));
        return INVALID_SOCK;
    }
    if (ERR_OK != connect(fd, netaddr_addr(paddr), netaddr_size(paddr)))
    {
        PRINTF("connect(%d, ...) failed. %s", (int32_t)fd, ERRORSTR(ERRNO));
        SOCK_CLOSE(fd);
        return INVALID_SOCK;
    }
    return fd;
}
int32_t sockpair(SOCKET acSock[2])
{
    SOCKET fdlsn = _sock_listen();
    if (INVALID_SOCK == fdlsn)
    {
        return ERR_FAILED;
    }
    union netaddr_ctx addr;
    if (ERR_OK != netaddr_localaddr(&addr, fdlsn, AF_INET))
    {
        SOCK_CLOSE(fdlsn);
        PRINTF("%s", "netaddr_localaddr failed.");
        return ERR_FAILED;
    }
    SOCKET fdcn = _sockcnt(&addr);
    if (INVALID_SOCK == fdcn)
    {
        SOCK_CLOSE(fdlsn);
        return ERR_FAILED;
    }
    struct sockaddr_in listen_addr;
    socklen_t iaddrlens = (socklen_t)sizeof(listen_addr);
    SOCKET fdacp = accept(fdlsn, (struct sockaddr *) &listen_addr, &iaddrlens);
    if (INVALID_SOCK == fdacp)
    {
        PRINTF("accept(%d, ...) failed. %s", (int32_t)fdlsn, ERRORSTR(ERRNO));
        SOCK_CLOSE(fdlsn);
        SOCK_CLOSE(fdcn);
        return ERR_FAILED;
    }
    SOCK_CLOSE(fdlsn);
    if (ERR_OK != netaddr_localaddr(&addr, fdcn, AF_INET))
    {
        SOCK_CLOSE(fdacp);
        SOCK_CLOSE(fdcn);
        return ERR_FAILED;
    }
    struct sockaddr_in *connect_addr = (struct sockaddr_in*)netaddr_addr(&addr);
    if (listen_addr.sin_family != connect_addr->sin_family
        || listen_addr.sin_addr.s_addr != connect_addr->sin_addr.s_addr
        || listen_addr.sin_port != connect_addr->sin_port)
    {
        SOCK_CLOSE(fdacp);
        SOCK_CLOSE(fdcn);
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
