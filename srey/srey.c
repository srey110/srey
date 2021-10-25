#include "lib.h"

#ifdef OS_WIN
#include "../vld/vld.h"
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "lib.lib")
#pragma comment(lib, "vld.lib")
#endif

struct server_ctx sv;
uint16_t usthreadnum = 0;
uint64_t ulaccuracy = 1000 * 1000 * 10;
volatile uint32_t uirecvsize = 0;
volatile uint32_t uisendsize = 0;
volatile uint32_t uilinknum = 0;
volatile uint32_t uistop = 0;
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
    char acpack[ONEK];
    size_t ilens;
    struct ev_ctx *pev;
    struct sock_ctx *psock;
    struct twnode_ctx *pnode;
    struct chan_ctx *pchan_timeout = p1;
    while (0 == ATOMIC_GET(&uistop))
    {
        if (ERR_OK == chan_recv(pchan_timeout, &pev))
        {
            pnode = UPCAST(pev, struct twnode_ctx, ev);
            PRINTF("time out diff:%d,link count %d recv pack %d send pack %d", 
                (int32_t)(server_tick(&sv) - pnode->expires), 
                ATOMIC_GET(&uilinknum), ATOMIC_GET(&uirecvsize), ATOMIC_GET(&uisendsize));
            psock = pnode->ev.data;
            ilens = _fill_data(acpack, sizeof(acpack));
            server_send(psock, acpack, ilens);
            SAFE_FREE(pnode);

            server_timeout(&sv, pchan_timeout, 200, psock);
        }
    }
}
void _accept_cb(void *p1, void *p2, void *p3)
{
    struct ev_ctx *pev;
    struct sock_ctx *psock;
    struct chan_ctx *pchan_accept = p1;
    struct chan_ctx *pchan_recvs = p2;
    while (0 == ATOMIC_GET(&uistop))
    {
        if (ERR_OK == chan_recv(pchan_accept, &pev))
        {
            psock = pev->data;
            server_enable_rw(psock, &pchan_recvs[rand() % usthreadnum], 1);
            ATOMIC_ADD(&uilinknum, 1);
            SAFE_FREE(pev);
        }
    }
}
void _connet_cb(void *p1, void *p2, void *p3)
{
    struct ev_ctx *pev;
    struct sock_ctx *psock;
    struct chan_ctx *pchan_connect = p1;
    struct chan_ctx *pchan_recvs = p2;
    while (0 == ATOMIC_GET(&uistop))
    {
        if (ERR_OK == chan_recv(pchan_connect, &pev))
        {
            psock = pev->data;
            if (ERR_OK == pev->code)
            {
                server_enable_rw(psock, &pchan_recvs[rand() % usthreadnum], 1);
                ATOMIC_ADD(&uilinknum, 1);
            }            
            SAFE_FREE(pev);
        }
    }
}
void _recv_cb(void *p1, void *p2, void *p3)
{
    struct ev_ctx *pev;
    struct sock_ctx *psock;
    struct chan_ctx *pchan_recv = p1;
    while (0 == ATOMIC_GET(&uistop))
    {
        if (ERR_OK == chan_recv(pchan_recv, &pev))
        {
            psock = pev->data;
            switch (pev->evtype)
            {
            case EV_RECV:
                ATOMIC_ADD(&uirecvsize, 1);                
                buffer_drain(psock->bufrecv, pev->code);
                break;
            case EV_SEND:
                ATOMIC_ADD(&uisendsize, 1);
                break;
            case EV_CLOSE:
                ATOMIC_ADD(&uilinknum, -1);
                sockctx_free(psock);
                break;
            }
            
            SAFE_FREE(pev);
        }
    }
}
void _send_cb(void *p1, void *p2, void *p3)
{
    char acpack[ONEK];
    size_t ilens;
    struct sock_ctx *psock = p1;
    while (0 == ATOMIC_GET(&uistop))
    {
        ilens = _fill_data(acpack, sizeof(acpack));
        server_send(psock, acpack, ilens);
        MSLEEP(100);
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
        SAFE_CLOSESOCK(fd);
        return INVALID_SOCK;
    }

    return fd;
}

int main(int argc, char *argv[])
{
    LOGINIT();

    usthreadnum = procsnum() * 2 + 2;
    struct chan_ctx chan_timeout;
    chan_init(&chan_timeout, ONEK);
    struct thread_ctx thread_timeout;
    thread_init(&thread_timeout);
    thread_creat(&thread_timeout, _timeout_cb, &chan_timeout, NULL, NULL);
    
    struct chan_ctx *pchan_recv = MALLOC(sizeof(struct chan_ctx) * usthreadnum);
    struct chan_ctx chan_connect;
    chan_init(&chan_connect, ONEK);
    struct thread_ctx thread_connect;
    thread_init(&thread_connect);
    thread_creat(&thread_connect, _connet_cb, &chan_connect, pchan_recv, NULL);
    
    struct chan_ctx *pchan_accept = MALLOC(sizeof(struct chan_ctx) * usthreadnum);    
    struct thread_ctx *pthread_accept = MALLOC(sizeof(struct thread_ctx) * usthreadnum);   
    struct thread_ctx *pthread_recv = MALLOC(sizeof(struct thread_ctx) * usthreadnum);
    for (uint16_t ui = 0; ui < usthreadnum; ui++)
    {
        chan_init(&pchan_accept[ui], ONEK);
        chan_init(&pchan_recv[ui], ONEK);
        thread_init(&pthread_accept[ui]);
        thread_init(&pthread_recv[ui]);
        thread_creat(&pthread_accept[ui], _accept_cb, &pchan_accept[ui], pchan_recv, NULL);
        thread_creat(&pthread_recv[ui], _recv_cb, &pchan_recv[ui], NULL, NULL);
    }

    server_init(&sv, ulaccuracy);
    server_run(&sv);

    const char *plistenhost = "0.0.0.0";
    uint16_t uslistenport = 15000;
    const char *pconnhost = "127.0.0.1";
    uint16_t usconnport = 15000;
    struct sock_ctx *plisten = server_listen(&sv, pchan_accept, usthreadnum, plistenhost, uslistenport);
    ASSERTAB(NULL != plisten, "1");

    struct sock_ctx *psock;

    /*psock = server_connect(&sv, &chan_connect, pconnhost, usconnport);
    ASSERTAB(NULL != psock, "server_connect error.");
    server_timeout(&sv, &chan_timeout, 200, psock);
    MSLEEP(1000);*/

    SOCKET fd = _creat_tcp_sock(pconnhost, usconnport);
    ASSERTAB(INVALID_SOCK != fd, "_creat_tcp_sock error.");
    psock = server_addsock(&sv, fd);
    ASSERTAB(NULL != psock, "server_addsock error.");
    server_enable_rw(psock, &pchan_recv[rand() % usthreadnum], 1);
    server_timeout(&sv, &chan_timeout, 200, psock);
    struct thread_ctx thread_send;
    thread_init(&thread_send);
    thread_creat(&thread_send, _send_cb, psock, NULL, NULL);
    
    char acpack[ONEK];
    size_t ilens;
    size_t uicounttime = 0;
    while (0 == ATOMIC_GET(&sv.stop))
    {
        ilens = _fill_data(acpack, sizeof(acpack));
        server_send(psock, acpack, ilens);
        MSLEEP(100);        
        /*uicounttime += 100;
        if (uicounttime >= 1000 * 10)
        {
            SAFE_CLOSESOCK(psock->sock);
            ATOMIC_SET(&uistop, 1);
            break;
        }*/
    }
    MSLEEP(1000);
    chan_close(&chan_timeout);
    thread_join(&thread_timeout);
    chan_free(&chan_timeout);    
    chan_close(&chan_connect);
    thread_join(&thread_connect);
    chan_free(&chan_connect);
    for (uint16_t ui = 0; ui < usthreadnum; ui++)
    {
        chan_close(&pchan_accept[ui]);
        chan_close(&pchan_recv[ui]);
        thread_join(&pthread_accept[ui]);
        thread_join(&pthread_recv[ui]);
        chan_free(&pchan_accept[ui]);
        chan_free(&pchan_recv[ui]);
    }

    server_free(&sv);
    sockctx_free(plisten);

    SAFE_FREE(pchan_accept);
    SAFE_FREE(pchan_recv);
    SAFE_FREE(pthread_recv);
    SAFE_FREE(pthread_accept);

    LOGFREE();
    return 0;
}
