#include "lib.h"

#ifdef OS_WIN
#include "../vld/vld.h"
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "lib.lib")
#pragma comment(lib, "vld.lib")
#endif
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
int32_t istop = 0;
#ifndef OS_WIN
void sigHandEntry(int iSigNum)
{
    PRINTF("catch signal %d.", iSigNum);
    istop = 1;
}
#endif
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
                PRINTF("%d  %d %d  %d", iremoved, uilens, sock_type(pscok), uold);
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
                PRINTF("%d  %d", iremoved, uilens);
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
            /*struct netaddr_ctx addr;
            netaddr_sethost(&addr, pip, uport);
            sendto(pscok->sock, acpack, isendsize, 0, netaddr_addr(&addr), netaddr_size(&addr));*/
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

    SOCKET udp = _creat_udp_sock(bindip, 15001);
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
int main(int argc, char *argv[])
{
#ifndef OS_WIN
    signal(SIGPIPE, SIG_IGN);//若某一端关闭连接，而另一端仍然向它写数据，第一次写数据后会收到RST响应，此后再写数据，内核将向进程发出SIGPIPE信号
    signal(SIGINT, sigHandEntry);//终止进程
    signal(SIGHUP, sigHandEntry);//终止进程
    signal(SIGTSTP, sigHandEntry);//ctrl+Z
    signal(SIGTERM, sigHandEntry);//终止一个进程
    signal(SIGKILL, sigHandEntry);//立即结束程序
    signal(SIGABRT, sigHandEntry);//中止一个程序
    //signal(H_SIGNAL_EXIT, sigHandEntry);   
#endif
    unlimit();
    LOGINIT();    
    u_long ulaccuracy = 1000 * 1000 * 10;
    struct timer_ctx timer;
    timer_init(&timer);
    struct tw_ctx tw;
    pnetev = netev_new(&tw, 0);
    netev_loop(pnetev);
    tw_init(&tw, (u_long)(timer_nanosec(&timer) / ulaccuracy));
    struct listener_ctx *plsn = netev_listener(pnetev, bindip, 15000, u_accept_cb, NULL);
    MSLEEP(100);
    struct sock_ctx *pconnsock = netev_connecter(pnetev, 100, linkip, 15000, u_connect_cb, NULL);

    SOCKET udp = _creat_udp_sock(bindip, 15001);
    struct sock_ctx *pudp = netev_add_sock(pnetev, udp, SOCK_DGRAM, ifamily);
    ASSERTAB(ERR_OK == netev_enable_rw(pnetev, pudp, u_read_cb, u_write_cb, u_close_cb, NULL), "netev_enable_rw udp");
    ATOMIC_ADD(&ullinkNum, 1);
    tw_add(&tw, 500, -1, udp_close, pudp);
    tw_add(&tw, 200, -1, print_info_cb, NULL);
    //int32_t itwcnt = 0;
    while (0 == istop)
    {
        tw_run(&tw, (u_long)(timer_nanosec(&timer) / ulaccuracy));
        MSLEEP(10);
    }    
    tw_free(&tw);
    netev_free(pnetev);
    LOGFREE();
    return 0;
}
