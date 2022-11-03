#include "netutils.h"
#include "netaddr.h"

#define MSEC    1000

#ifdef OS_WIN
static volatile atomic_t _init_sock = 0;
#endif
void sock_init()
{
#ifdef OS_WIN
    if (ATOMIC_CAS(&_init_sock, 0, 1))
    {
        WSADATA wsdata;
        WORD ver = MAKEWORD(2, 2);
        ASSERTAB(ERR_OK == WSAStartup(ver, &wsdata), ERRORSTR(ERRNO));
    }
#endif
}
void sock_clean()
{
#ifdef OS_WIN
    if (ATOMIC_CAS(&_init_sock, 1, 0))
    {
        (void)WSACleanup();
    }
#endif
}
int32_t sock_nread(SOCKET fd)
{
#if defined(OS_WIN)
    u_long nread = 0;
    if (ioctlsocket(fd, FIONREAD, &nread) < ERR_OK)
    {
        return ERR_FAILED;
    }
    return (int32_t)nread;
#else
    int32_t nread = 0;
    if (ioctl(fd, FIONREAD, &nread) < ERR_OK)
    {
        return ERR_FAILED;
    }
    return nread;
#endif
}
int32_t sock_type(SOCKET fd)
{
    int32_t stype = 0;
    int32_t len = (int32_t)sizeof(stype);
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, (char *)&stype, (socklen_t*)&len) < ERR_OK)
    {
        PRINT("getsockopt(%d, ...) failed. %s", (int32_t)fd, ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    return stype;
}
int32_t sock_family(SOCKET fd)
{
#if defined(OS_WIN)
    WSAPROTOCOL_INFO info;
    int32_t lens = (int32_t)sizeof(info);
    if (getsockopt(fd, SOL_SOCKET, SO_PROTOCOL_INFO, (char *)&info, &lens) < ERR_OK)
    {
        PRINT("getsockopt(%d, SOL_SOCKET, SO_PROTOCOL_INFO, ...) failed. %s", (int32_t)fd, ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    return info.iAddressFamily;
#else
#ifdef SO_DOMAIN
    int32_t family = 0;
    int32_t lens = (int32_t)sizeof(family);
    if (getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &family, (socklen_t*)&lens) < 0)
    {
        PRINT("getsockopt(%d, SOL_SOCKET, SO_DOMAIN, ...) failed. %s", (int32_t)fd, ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    return family;
#endif
    return AF_INET;
#endif
}
void sock_nodelay(SOCKET fd)
{
    int32_t flag = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, (int32_t)sizeof(flag)) < ERR_OK)
    {
        PRINT("setsockopt(%d, IPPROTO_TCP, TCP_NODELAY, %d, %d) failed. %s",
            (int32_t)fd, flag, (int32_t)sizeof(flag), ERRORSTR(ERRNO));
    }
}
void sock_nbio(SOCKET fd)
{
#if defined(OS_WIN)
    u_long flag = 1;
    if (ioctlsocket(fd, FIONBIO, &flag) < ERR_OK)
    {
        PRINT("ioctlsocket(%d, FIONBIO, 1) failed. %s", (int32_t)fd, ERRORSTR(ERRNO));
    }
#else
    int32_t flag = fcntl(fd, F_GETFL, NULL);
    if (ERR_FAILED == flag)
    {
        PRINT("fcntl(%d, F_GETFL, NULL) failed.", fd);
        return;
    }
    if (!(flag & O_NONBLOCK))
    {
        if (ERR_FAILED == fcntl(fd, F_SETFL, flag | O_NONBLOCK))
        {
            PRINT("fcntl(%d, F_SETFL, %d) failed", fd, flag | O_NONBLOCK);
            return;
        }
    }
#endif
}
void sock_raddr(SOCKET fd)
{
    int32_t flag = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&flag, (int32_t)sizeof(flag)) < ERR_OK)
    {
        PRINT("setsockopt(%d, SOL_SOCKET, SO_REUSEADDR, %d, %d) failed. %s",
            (int32_t)fd, flag, (int32_t)sizeof(flag), ERRORSTR(ERRNO));
    }
}
int32_t sock_checkrport()
{
#ifdef SO_REUSEPORT
    return ERR_OK;
#else
    return ERR_FAILED;
#endif
}
void sock_rport(SOCKET fd)
{
#ifdef SO_REUSEPORT
    int32_t flag = 1;
   if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, (char *)&flag, (int32_t)sizeof(flag)) < ERR_OK)
   {
       PRINT("setsockopt(%d, SOL_SOCKET, SO_REUSEPORT, %d, %d) failed. %s",
           fd, flag, (int32_t)sizeof(flag), ERRORSTR(ERRNO));
   }
#endif 
}
void sock_kpa(SOCKET fd, const int32_t delay, const int32_t intvl)
{
    int32_t flag = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (char *)&flag, (int32_t)sizeof(flag)) < ERR_OK)
    {
        PRINT("setsockopt(%d, SOL_SOCKET, SO_KEEPALIVE, %d, %d) failed. %s", 
            (int32_t)fd, flag, (int32_t)sizeof(flag), ERRORSTR(ERRNO));
        return;
    }
    if (0 >= delay)
    {
        return;
    }
#if defined(OS_WIN)
    struct tcp_keepalive kpa;
    struct tcp_keepalive out;
    DWORD ret = 0;
    kpa.keepalivetime = delay * MSEC;
    kpa.keepaliveinterval = intvl * MSEC;
    if (WSAIoctl(fd, SIO_KEEPALIVE_VALS, (LPVOID)&kpa, sizeof(struct tcp_keepalive),
        (LPVOID)&out, sizeof(struct tcp_keepalive), &ret, NULL, NULL) < ERR_OK)
    {
        PRINT("WSAIoctl(%d, SIO_KEEPALIVE_VALS...) failed. %s", (int32_t)fd, ERRORSTR(ERRNO));
    }
#else
#ifdef TCP_KEEPIDLE
    int32_t cnt = 3;
    //多久后发送keepalive 秒
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, (char *)&delay, (int32_t)sizeof(delay)) < ERR_OK)
    {
        PRINT("setsockopt(%d, IPPROTO_TCP, TCP_KEEPIDLE, %d, %d) failed. %s",
            fd, delay, (int32_t)sizeof(delay), ERRORSTR(ERRNO));
        return;
    }
    //时间间隔
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, (char *)&intvl, (int32_t)sizeof(intvl)) < ERR_OK)
    {
        PRINT("setsockopt(%d, IPPROTO_TCP, TCP_KEEPINTVL, %d, %d) failed. %s",
            fd, intvl, (int32_t)sizeof(intvl), ERRORSTR(ERRNO));
        return;
    }
    //重试次数
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, (char *)&cnt, (int32_t)sizeof(cnt)) < ERR_OK)
    {
        PRINT("setsockopt(%d, IPPROTO_TCP, TCP_KEEPCNT, %d, %d) failed. %s",
            fd, cnt, (int32_t)sizeof(cnt), ERRORSTR(ERRNO));
    }
    return;
#endif
#if defined(TCP_KEEPALIVE) && !defined(OS_SUN)
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, (char *)&delay, (int32_t)sizeof(delay)) < ERR_OK)
    {
        PRINT("setsockopt(%d, IPPROTO_TCP, TCP_KEEPALIVE, %d, %d) failed. %s",
            fd, delay, (int32_t)sizeof(delay), ERRORSTR(ERRNO));
    }
#endif
#endif
}
void sock_linger(SOCKET fd)
{
    struct linger lg = { 1, 0 };
    if (setsockopt(fd, SOL_SOCKET, SO_LINGER, (char *)&lg, (int32_t)sizeof(lg)) < ERR_OK)
    {
        PRINT("setsockopt(%d, SOL_SOCKET, SO_LINGER, 1 0) failed. %s",
            (int32_t)fd, ERRORSTR(ERRNO));
    }
}
static SOCKET _sock_listen()
{
    netaddr_ctx addr;
    if (ERR_OK != netaddr_sethost(&addr, "127.0.0.1", 0))
    {
        PRINT("%s", ERRORSTR(ERRNO));
        return INVALID_SOCK;
    }
    SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
    if (INVALID_SOCK == fd)
    {
        PRINT("socket(AF_INET, SOCK_STREAM, 0) failed. %s", ERRORSTR(ERRNO));
        return INVALID_SOCK;
    }
    if (ERR_OK != bind(fd, netaddr_addr(&addr), netaddr_size(&addr)))
    {
        PRINT("bind(%d, ...) failed. %s", (int32_t)fd, ERRORSTR(ERRNO));
        CLOSE_SOCK(fd);
        return INVALID_SOCK;
    }
    if (ERR_OK != listen(fd, 1))
    {
        PRINT("listen(%d, 1) failed. %s", (int32_t)fd, ERRORSTR(ERRNO));
        CLOSE_SOCK(fd);
        return INVALID_SOCK;
    }
    return fd;
}
static SOCKET _sockcnt(union netaddr_ctx *paddr)
{
    SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
    if (INVALID_SOCK == fd)
    {
        PRINT("socket(AF_INET, SOCK_STREAM, 0) failed. %s", ERRORSTR(ERRNO));
        return INVALID_SOCK;
    }
    if (ERR_OK != connect(fd, netaddr_addr(paddr), netaddr_size(paddr)))
    {
        PRINT("connect(%d, ...) failed. %s", (int32_t)fd, ERRORSTR(ERRNO));
        CLOSE_SOCK(fd);
        return INVALID_SOCK;
    }
    return fd;
}
int32_t sock_pair(SOCKET acSock[2])
{
    SOCKET fdlsn = _sock_listen();
    if (INVALID_SOCK == fdlsn)
    {
        return ERR_FAILED;
    }
    netaddr_ctx addr;
    if (ERR_OK != netaddr_localaddr(&addr, fdlsn, AF_INET))
    {
        CLOSE_SOCK(fdlsn);
        PRINT("%s", "netaddr_localaddr failed.");
        return ERR_FAILED;
    }
    SOCKET fdcn = _sockcnt(&addr);
    if (INVALID_SOCK == fdcn)
    {
        CLOSE_SOCK(fdlsn);
        return ERR_FAILED;
    }
    struct sockaddr_in listen_addr;
    socklen_t addrlen = (socklen_t)sizeof(listen_addr);
    SOCKET fdacp = accept(fdlsn, (struct sockaddr *) &listen_addr, &addrlen);
    if (INVALID_SOCK == fdacp)
    {
        PRINT("accept(%d, ...) failed. %s", (int32_t)fdlsn, ERRORSTR(ERRNO));
        CLOSE_SOCK(fdlsn);
        CLOSE_SOCK(fdcn);
        return ERR_FAILED;
    }
    CLOSE_SOCK(fdlsn);
    if (ERR_OK != netaddr_localaddr(&addr, fdcn, AF_INET))
    {
        CLOSE_SOCK(fdacp);
        CLOSE_SOCK(fdcn);
        return ERR_FAILED;
    }
    struct sockaddr_in *connect_addr = (struct sockaddr_in*)netaddr_addr(&addr);
    if (listen_addr.sin_family != connect_addr->sin_family
        || listen_addr.sin_addr.s_addr != connect_addr->sin_addr.s_addr
        || listen_addr.sin_port != connect_addr->sin_port)
    {
        CLOSE_SOCK(fdacp);
        CLOSE_SOCK(fdcn);
        return ERR_FAILED;
    }

    sock_nodelay(fdacp);
    sock_nodelay(fdcn);
    sock_nbio(fdacp);
    sock_nbio(fdcn);
    acSock[0] = fdacp;
    acSock[1] = fdcn;
    return ERR_OK;
}
