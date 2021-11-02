#include "lib.h"

#ifdef OS_WIN
#include "../vld/vld.h"
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "lib.lib")
#pragma comment(lib, "vld.lib")
#endif

struct event_ctx sv;
uint32_t usthreadnum = 0;
uint64_t ulaccuracy = 1000 * 1000 * 10;
volatile uint32_t uirecvsize = 0;
volatile uint32_t uisendsize = 0;
volatile uint32_t uilinknum = 0;
volatile uint32_t uistop = 0;
volatile uint32_t uiudprecvsize = 0;
volatile uint32_t uiudpsendsize = 0;
volatile uint32_t uitcpclose = 0;
const int32_t ipostsendev = 1;
size_t _fill_data(char *acpack, size_t ilens)
{
    size_t isize = 50 + rand() % (ilens - 50);
    ASSERTAB(isize <= ilens - 1, "failed.");
    for (size_t i = 0; i < isize; i++)
    {
        acpack[i] = 'a' + rand() %  ('z' - 'a' + 1);
    }
    acpack[isize] = '\0';

    return isize;
}
void _timeout_cb(void *p1, void *p2, void *p3)
{
    struct ev_ctx *pev;
    struct ev_time_ctx *pnode;
    struct chan_ctx *pchan_timeout = p1;
    while (0 == ATOMIC_GET(&uistop))
    {
        pev = chan_recv(pchan_timeout);
        if (NULL != pev)
        {
            pnode = UPCAST(pev, struct ev_time_ctx, ev);
            PRINTF("time out diff:%d,link count %d recv pack %d send pack %d udp recv %d udp send %d  close time %d",
                (int32_t)(event_tick(&sv) - pnode->expires),
                ATOMIC_GET(&uilinknum), ATOMIC_GET(&uirecvsize), ATOMIC_GET(&uisendsize),
                ATOMIC_GET(&uiudprecvsize), ATOMIC_GET(&uiudpsendsize), ATOMIC_GET(&uitcpclose));
            SAFE_FREE(pnode);

            event_timeout(&sv, pchan_timeout, 200, NULL);
        }
    }
}
void _accept_cb(void *p1, void *p2, void *p3)
{
    struct ev_ctx *pev;
    struct ev_sock_ctx *psockev;
    struct chan_ctx *pchan_accept = p1;
    struct chan_ctx *pchan_recvs = p2;
    while (0 == ATOMIC_GET(&uistop))
    {
        pev = chan_recv(pchan_accept);
        if (NULL != pev)
        {
            psockev = UPCAST(pev, struct ev_sock_ctx, ev);
            if (ERR_OK == event_addsock(&sv, psockev->sock))
            {
                event_enablerw(&sv, psockev->sock, &pchan_recvs[rand() % usthreadnum], ipostsendev);
                ATOMIC_ADD(&uilinknum, 1);
            }
            SAFE_FREE(psockev);
        }
    }
}
void _connet_cb(void *p1, void *p2, void *p3)
{
    struct ev_ctx *pev;
    struct ev_sock_ctx *psockev;
    struct chan_ctx *pchan_connect = p1;
    struct chan_ctx *pchan_recvs = p2;
    while (0 == ATOMIC_GET(&uistop))
    {
        pev = chan_recv(pchan_connect);
        if (NULL != pev)
        {
            psockev = UPCAST(pev, struct ev_sock_ctx, ev);
            if (ERR_OK == psockev->ev.result)
            {
                event_enablerw(&sv, psockev->sock, &pchan_recvs[rand() % usthreadnum], ipostsendev);
                ATOMIC_ADD(&uilinknum, 1);
            }
            else
            {
                PRINTF("connect error %s", ERRORSTR(psockev->ev.result));
            }
            SAFE_FREE(psockev);
        }
    }
}
void _recv_cb(void *p1, void *p2, void *p3)
{
    struct ev_ctx *pev;
    struct ev_sock_ctx *psockev;
    struct chan_ctx *pchan_recv = p1;
    char acpack[ONEK * 4];
    struct buffer_ctx *pbuf;
    struct netaddr_ctx addr;
    size_t ilens, isendsize;
    while (0 == ATOMIC_GET(&uistop))
    {
        pev = chan_recv(pchan_recv);
        if (NULL != pev)
        {
            psockev = UPCAST(pev, struct ev_sock_ctx, ev);
            switch (psockev->ev.evtype)
            {
                case EV_RECV:
                    ilens = (size_t)psockev->ev.result;
                    pbuf = sock_recvbuf(psockev->sockctx);
                    while (ilens > 0)
                    {
                        if (ilens >= sizeof(acpack))
                        {
                            ASSERTAB(sizeof(acpack) == buffer_remove(pbuf, acpack, sizeof(acpack)), "remove buffer lens error.");
                            isendsize = sizeof(acpack);
                            ilens -= sizeof(acpack);
                        }
                        else
                        {
                            ASSERTAB(ilens == buffer_remove(pbuf, acpack, ilens), "remove buffer lens error.");
                            isendsize = ilens;
                            ilens = 0;
                        }

                        if (psockev->socktpe == SOCK_STREAM)
                        {
                            ATOMIC_ADD(&uirecvsize, 1);
                            tcp_send(psockev->sockctx, acpack, isendsize);
                            //send(psockev->sock, acpack, isendsize, 0);
                            //sock_close(&sv.netev, psockev->sockctx);
                        }
                        else
                        {
                            ATOMIC_ADD(&uiudprecvsize, 1);
                            udp_send(psockev->sockctx, acpack, isendsize, psockev->ip, psockev->port);
                            //netaddr_sethost(&addr, psockev->ip, psockev->port);
                            //sendto(psockev->sock, acpack, isendsize, 0, netaddr_addr(&addr), netaddr_size(&addr));
                        }
                    }
                    break;
                case EV_SEND:                    
                    if (psockev->socktpe == SOCK_STREAM)
                    {
                        ATOMIC_ADD(&uisendsize, 1);
                    }
                    else
                    {
                        ATOMIC_ADD(&uiudpsendsize, 1);

                    }
                    break;
                case EV_CLOSE:
                    ATOMIC_ADD(&uilinknum, -1);
                    ATOMIC_ADD(&uitcpclose, 1);
                    event_freesock(&sv, psockev->sockctx);
                    break;
            }
            SAFE_FREE(psockev);
        }
    }
}
SOCKET _creat_tcp_sock(const char *phost, uint16_t usport)
{
    struct netaddr_ctx addr;
    if (ERR_OK != netaddr_sethost(&addr, phost, usport))
    {
        return INVALID_SOCK;
    }
    SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
    if (INVALID_SOCK == fd)
    {
        return INVALID_SOCK;
    }
    if (ERR_OK != connect(fd, netaddr_addr(&addr), netaddr_size(&addr)))
    {
        SOCK_CLOSE(fd);
        return INVALID_SOCK;
    }

    return fd;
}
SOCKET _creat_udp_sock(const char *phost, uint16_t usport)
{
    struct netaddr_ctx addr;
    if (ERR_OK != netaddr_sethost(&addr, phost, usport))
    {
        return INVALID_SOCK;
    }
    SOCKET fd = socket(netaddr_family(&addr), SOCK_DGRAM, 0);
    if (INVALID_SOCK == fd)
    {
        return INVALID_SOCK;
    }
    sockraddr(fd);
    if (ERR_OK != bind(fd, netaddr_addr(&addr), netaddr_size(&addr)))
    {
        SOCK_CLOSE(fd);
        return INVALID_SOCK;
    }

    return fd;
}
int main(int argc, char *argv[])
{
    LOGINIT();
    int32_t ibig = bigendian();
    usthreadnum = procscnt() * 2;
    struct chan_ctx chan_timeout;
    chan_init(&chan_timeout, ONEK, 0);
    struct thread_ctx thread_timeout;
    thread_init(&thread_timeout);
    thread_creat(&thread_timeout, _timeout_cb, &chan_timeout, NULL, NULL);
    
    struct chan_ctx *pchan_recv = MALLOC(sizeof(struct chan_ctx) * usthreadnum);
    struct chan_ctx chan_connect;
    chan_init(&chan_connect, ONEK, 0);
    struct thread_ctx thread_connect;
    thread_init(&thread_connect);
    thread_creat(&thread_connect, _connet_cb, &chan_connect, pchan_recv, NULL);
    
    struct chan_ctx *pchan_accept = MALLOC(sizeof(struct chan_ctx) * usthreadnum);    
    struct thread_ctx *pthread_accept = MALLOC(sizeof(struct thread_ctx) * usthreadnum);   
    struct thread_ctx *pthread_recv = MALLOC(sizeof(struct thread_ctx) * usthreadnum);
    for (uint32_t ui = 0; ui < usthreadnum; ui++)
    {
        chan_init(&pchan_accept[ui], ONEK * 4, 0);
        chan_init(&pchan_recv[ui], ONEK * 4, 0);
        thread_init(&pthread_accept[ui]);
        thread_init(&pthread_recv[ui]);
        thread_creat(&pthread_accept[ui], _accept_cb, &pchan_accept[ui], pchan_recv, NULL);
        thread_creat(&pthread_recv[ui], _recv_cb, &pchan_recv[ui], NULL, NULL);
    }

    event_init(&sv, (u_long)ulaccuracy);
    event_loop(&sv);

    const char *plistenhost = "0:0:0:0:0:0:0:0";
    uint16_t uslistenport = 15000;
    const char *pconnhost = "fe80::c95e:3ff8:a284:fe13%17";
    uint16_t usconnport = 15000;
    const char *pudphost = "0:0:0:0:0:0:0:0";
    uint16_t usudpport = 15001;
    SOCKET sock_listener = event_listener(&sv, pchan_accept, usthreadnum, plistenhost, uslistenport);
    ASSERTAB(INVALID_SOCK != sock_listener, "1");
    event_timeout(&sv, &chan_timeout, 200, NULL);
    
    SOCKET sock_connecter = event_connecter(&sv, &chan_connect, pconnhost, usconnport);
    ASSERTAB(INVALID_SOCK != sock_connecter, "server_connect error.");
    PRINTF("event_connecter %d", (int32_t)sock_connecter);
    ATOMIC_ADD(&uilinknum, 1);

    SOCKET udpfd = _creat_udp_sock(pudphost, usudpport);
    ASSERTAB(INVALID_SOCK != udpfd, "_creat_udp_sock error.");
    ASSERTAB(ERR_OK == event_addsock(&sv, udpfd), "server_addsock error.");
    struct sock_ctx *pudpctx = event_enablerw(&sv, udpfd, &pchan_recv[rand() % usthreadnum], ipostsendev);
    ATOMIC_ADD(&uilinknum, 1);

    size_t uicounttime = 0;
    while (0 == ATOMIC_GET(&uistop))
    {
        uicounttime += 100;
        MSLEEP(100);
        /*if (uicounttime >= 10000)
        {
            break;
        }*/
    }
    sock_close(pudpctx);
    SOCK_CLOSE(sock_connecter);
    SOCK_CLOSE(sock_listener);
    MSLEEP(1000 * 1);
    ATOMIC_SET(&uistop, 1);
    chan_close(&chan_timeout);
    thread_join(&thread_timeout);
    event_free(&sv);

    chan_close(&chan_connect);
    thread_join(&thread_connect);

    chan_free(&chan_connect);   
    chan_free(&chan_timeout);
    for (uint16_t ui = 0; ui < usthreadnum; ui++)
    {
        chan_close(&pchan_accept[ui]);
        chan_close(&pchan_recv[ui]);
        thread_join(&pthread_accept[ui]);
        thread_join(&pthread_recv[ui]);
        chan_free(&pchan_accept[ui]);
        chan_free(&pchan_recv[ui]);
    }

    SAFE_FREE(pchan_accept);
    SAFE_FREE(pchan_recv);
    SAFE_FREE(pthread_recv);
    SAFE_FREE(pthread_accept);

    LOGFREE();
    return 0;
}
