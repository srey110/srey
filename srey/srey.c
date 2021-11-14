#include "lib.h"

#ifdef OS_WIN
#include "../vld/vld.h"
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "lib.lib")
#pragma comment(lib, "vld.lib")
#endif
int32_t istop = 0;
struct netev_ctx *pnetev;
volatile atomic_t ullinkNum = 0;
volatile atomic_t ultcprecvnum = 0;
volatile atomic_t ultcpsendnum = 0;
volatile atomic_t uludprecvnum = 0;
volatile atomic_t uludpsendnum = 0;
void u_read_cb(struct sock_ctx *pscok, struct buffer_ctx *pbuf, size_t uilens, const char *pip, uint16_t uport, void *pdata)
{    
    int32_t itype = sock_type(pscok);
    if (SOCK_STREAM == itype)
    {
        ATOMIC_ADD(&ultcprecvnum, 1);
    }
    else
    {
        ATOMIC_ADD(&uludprecvnum, 1);
    }
    int32_t iremoved;
    size_t isendsize;
    char acpack[4096];
    size_t uold = uilens;
    while (uilens > 0)
    {
        if (uilens >= sizeof(acpack))
        {
            iremoved = buffer_remove(pbuf, acpack, sizeof(acpack));
            if (sizeof(acpack) != iremoved)
            {
                PRINTF("%d  %d %d  %d", iremoved, (int32_t)uilens, sock_type(pscok), (int32_t)uold);
                ASSERTAB(0, "remove buffer lens error.");
            }            
            isendsize = sizeof(acpack);
            uilens -= sizeof(acpack);
        }
        else
        {
            iremoved = buffer_remove(pbuf, acpack, sizeof(acpack));
            if (uilens != iremoved)
            {
                PRINTF("%d  %d", iremoved, (int32_t)uilens);
                ASSERTAB(0, "remove buffer lens error.");
            }
            isendsize = uilens;
            uilens = 0;
        }
        if (itype == SOCK_STREAM)
        {  
            sock_send(pscok, acpack, isendsize);
            if (rand() % 10 == 0)
            {
                sock_close(pscok);
            }
        }
        else
        {
            sock_sendto(pscok, pip, uport, acpack, isendsize);
        }
    }
}
void u_write_cb(struct sock_ctx *pscok, size_t uilens, void *pdata)
{
    if (SOCK_STREAM == sock_type(pscok))
    {
        ATOMIC_ADD(&ultcpsendnum, 1);
    }
    else
    {
        ATOMIC_ADD(&uludpsendnum, 1);
    }
}
void u_close_cb(struct sock_ctx *pscok, void *pdata)
{
    sock_free(pscok);
    ATOMIC_ADD(&ullinkNum, -1);
}
void u_accept_cb(struct sock_ctx *pscok, void *pdata)
{
    ASSERTAB(ERR_OK == netev_enable_rw(pnetev, pscok, u_read_cb, u_write_cb, u_close_cb, pdata), "u_accept_cb");
    ATOMIC_ADD(&ullinkNum, 1);
}
void u_connect_cb(struct sock_ctx *pscok, int32_t ierr, void *pdata)
{
    if (ERR_OK != ierr)
    {
        sock_free(pscok);
        PRINTF("u_connect_cb %d", ierr);
        return;
    }
    ASSERTAB(ERR_OK == netev_enable_rw(pnetev, pscok, u_read_cb, u_write_cb, u_close_cb, pdata), "u_connect_cb");
    ATOMIC_ADD(&ullinkNum, 1);
}
void print_info_cb(struct tw_node_ctx *ptw, void *pdata)
{
    PRINTF("link %d tcp recv %d send %d  udp recv %d send %d", 
        (uint32_t)ATOMIC_GET(&ullinkNum), 
        (uint32_t)ATOMIC_GET(&ultcprecvnum), (uint32_t)ATOMIC_GET(&ultcpsendnum),
        (uint32_t)ATOMIC_GET(&uludprecvnum), (uint32_t)ATOMIC_GET(&uludpsendnum));
}
//int32_t ifamily = AF_INET6;
//const char *bindip = "::";
//const char *linkip = "::1";
int32_t ifamily = AF_INET;
const char *bindip = "0.0.0.0";
const char *linkip = "127.0.0.1";
void udp_close(struct tw_node_ctx *ptw, void *pdata)
{
    struct sock_ctx *pudp = pdata;
    if (NULL != pudp)
    {
        sock_close(pudp);
    }

    SOCKET udp = sock_udp_bind(bindip, 15001);
    if (INVALID_SOCK != udp)
    {
        struct sock_ctx *pnewudp = netev_add_sock(pnetev, udp, SOCK_DGRAM, ifamily);
        ASSERTAB(ERR_OK == netev_enable_rw(pnetev, pnewudp, u_read_cb, u_write_cb, u_close_cb, NULL), "netev_enable_rw udp");
        tw_udata(ptw, pnewudp);
        ATOMIC_ADD(&ullinkNum, 1);
    }
    else
    {
        tw_udata(ptw, NULL);
    }
}
void sig_exit(int32_t isig)
{
    istop = 1;
}
int main(int argc, char *argv[])
{
    sighandle(sig_exit);
    unlimit();

    LOGINIT();
    struct event_ctx *pevent = event_new();
    event_loop(pevent);
    pnetev = event_netev(pevent);
    struct listener_ctx *plsn = netev_listener(pnetev, bindip, 15000, u_accept_cb, NULL);
    MSLEEP(100);
    struct sock_ctx *pconnsock = netev_connecter(pnetev, 100, linkip, 15000, u_connect_cb, NULL);

    SOCKET udp = sock_udp_bind(bindip, 15001);
    struct sock_ctx *pudp = netev_add_sock(pnetev, udp, SOCK_DGRAM, ifamily);
    ASSERTAB(ERR_OK == netev_enable_rw(pnetev, pudp, u_read_cb, u_write_cb, u_close_cb, NULL), "netev_enable_rw udp");
    ATOMIC_ADD(&ullinkNum, 1);
    tw_add(event_tw(pevent), 5000, -1, udp_close, pudp);
    tw_add(event_tw(pevent), 2000, -1, print_info_cb, NULL);
    while (0 == istop)
    {
        MSLEEP(10);
    }
    event_free(pevent);
    LOGFREE();
    return 0;
}
