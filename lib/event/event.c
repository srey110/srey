#include "event/event.h"

SOCKET _ev_sock(int32_t family)
{
#ifdef OS_WIN
    return WSASocket(family, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
#else
    #ifdef OS_LINUX
        return socket(family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    #else
        return socket(family, SOCK_STREAM, 0);
    #endif
#endif
}
SOCKET _ev_listen(netaddr_ctx *addr)
{
    SOCKET sock = _ev_sock(netaddr_family(addr));
    if (INVALID_SOCK == sock)
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return INVALID_SOCK;
    }
    sock_raddr(sock);
    sock_rport(sock);
    sock_nbio(sock);
    if (ERR_OK != bind(sock, netaddr_addr(addr), netaddr_size(addr)))
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        CLOSE_SOCK(sock);
        return INVALID_SOCK;
    }
    if (ERR_OK != listen(sock, SOMAXCONN))
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        CLOSE_SOCK(sock);
        return INVALID_SOCK;
    }
    return sock;
}
