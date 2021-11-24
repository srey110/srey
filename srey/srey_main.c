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
struct srey_ctx *pevent;
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
            /*if (rand() % pscok->sock == 0)
            {
                sock_close(pscok);
            }*/
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
void print_info_cb(struct ud_ctx *pud)
{
    PRINTF("link %d tcp recv %d send %d  udp recv %d send %d", 
        (uint32_t)ATOMIC_GET(&ullinkNum), 
        (uint32_t)ATOMIC_GET(&ultcprecvnum), (uint32_t)ATOMIC_GET(&ultcpsendnum),
        (uint32_t)ATOMIC_GET(&uludprecvnum), (uint32_t)ATOMIC_GET(&uludpsendnum));
    srey_wakeup(pevent);
    tw_add(srey_tw(pevent), 2000, print_info_cb, NULL);
}
//int32_t ifamily = AF_INET6;
//const char *bindip = "::";
//const char *linkip = "::1";
int32_t ifamily = AF_INET;
const char *bindip = "0.0.0.0";
const char *linkip = "127.0.0.1";
void udp_close(struct ud_ctx *pud)
{
    if (0 != istop)
    {
        return;
    }
    struct sock_ctx *pudp = (struct sock_ctx *)pud->handle;
    if (NULL != pudp)
    {
        sock_close(pudp);
    }

    SOCKET udp = sock_udp_bind(bindip, 15001);
    if (INVALID_SOCK != udp)
    {
        struct sock_ctx *pnewudp = netev_add_sock(pnetev, udp, SOCK_DGRAM, ifamily);
        ASSERTAB(ERR_OK == netev_enable_rw(pnetev, pnewudp, u_read_cb, u_write_cb, u_close_cb, NULL), "netev_enable_rw udp");
        pud->handle = (uintptr_t)pnewudp;
        tw_add(srey_tw(pevent), 5000, udp_close, pud);
        ATOMIC_ADD(&ullinkNum, 1);
    }
    else
    {
        tw_add(srey_tw(pevent), 5000, udp_close, NULL);
    }
}
void sig_exit(int32_t isig)
{
    istop = 1;
}

struct map_test
{
    int32_t id;
};
uint64_t _hash(void *item)
{
    struct map_test *pit = item;
    return fnv1a_hash((const char *)&pit->id, sizeof(pit->id));
}
int32_t _compare(void *a, void *b, void *udata)
{
    struct map_test *pa = a;
    struct map_test *pb = b;
    return pa->id == pb->id ? ERR_OK : ERR_FAILED;
}
int32_t _iter(void *item, void *udata)
{
    //struct map_test *pa = item;
    //PRINTF("%d", pa->id);
    return ERR_OK;
}
int main(int argc, char *argv[])
{
    /*int32_t i, iloop = 10000000;
    struct map_test map;
    struct timer_ctx timer;
    timer_init(&timer);
    struct map_ctx *pmap = map_new(sizeof(struct map_test), _hash, _compare, NULL);
    timer_start(&timer);
    for (i = 1; i < iloop; i++)
    {
        map.id = i;
        _map_set(pmap, &map);
    }
    int32_t irtn;
    struct map_test maprtn;
    for (i = 1; i < iloop; i++)
    {
        map.id = i;
        irtn = _map_get(pmap, &map, &maprtn);
        if (ERR_OK != irtn
            || maprtn.id != i)
        {
            PRINTF("%s", "11111111111111111111111");
        }
    }

    map_iter(pmap, _iter, NULL);
    for (i = 1; i < iloop + 1; i++)
    {
        map.id = i;
        irtn = _map_remove(pmap, &map, &maprtn);
        if (ERR_OK != irtn
            || maprtn.id != i)
        {
            PRINTF("%s", "222222222222222222");
        }
    }
    PRINTF("3333333333333  %lld", timer_elapsed(&timer)/ (1000 * 1000));*/
    PRINTF("%d  %d", (int32_t)sizeof(size_t), (int32_t)sizeof(int64_t));
    volatile atomic64_t iii = 0;
    ATOMIC64_ADD(&iii, 1);
    sighandle(sig_exit);
    unlimit();
    LOGINIT();
    SETLOGPRT(1);
    LOG_DEBUG("%s", "LOG_DEBUG");
    LOG_INFO("%s", "LOG_INFO");
    LOG_WARN("%s", "LOG_WARN");
    LOG_ERROR("%s", "LOG_ERROR");
    LOG_FATAL("%s", "LOG_FATAL");

    pevent = srey_new();
    srey_loop(pevent);
    pnetev = srey_netev(pevent);
    struct listener_ctx *plsn = netev_listener(pnetev, bindip, 15000, u_accept_cb, NULL);
    MSLEEP(100);
    struct sock_ctx *pconnsock = netev_connecter(pnetev, 3000, "192.168.92.135", 15000, u_connect_cb, NULL);
    struct sock_ctx *pconnsock2 = netev_connecter(pnetev, 3000, linkip, 15000, u_connect_cb, NULL);

    SOCKET udp = sock_udp_bind(bindip, 15001);
    struct sock_ctx *pudp = netev_add_sock(pnetev, udp, SOCK_DGRAM, ifamily);
    ASSERTAB(ERR_OK == netev_enable_rw(pnetev, pudp, u_read_cb, u_write_cb, u_close_cb, NULL), "netev_enable_rw udp");
    ATOMIC_ADD(&ullinkNum, 1);
    struct ud_ctx ud;
    ud.handle = (uintptr_t)pudp;
    //tw_add(srey_tw(pevent), 5000, udp_close, &ud);
    tw_add(srey_tw(pevent), 2000, print_info_cb, NULL);
    while (0 == istop)
    {
        MSLEEP(10);
    }
    sock_close(pconnsock2);
    listener_free(plsn);
    MSLEEP(1000);
    srey_free(pevent);
    LOGFREE();
    return 0;
}
