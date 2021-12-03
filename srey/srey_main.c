#include "lib.h"

#ifdef OS_WIN
#include "../vld/vld.h"
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "lib.lib")
#pragma comment(lib, "vld.lib")
#endif
int32_t istop = 0;
volatile atomic_t uilinkcnt = 0;
volatile atomic_t uitcprecv = 0;
volatile atomic_t uitcpsend = 0;
volatile atomic_t uiudprecv = 0;
volatile atomic_t uiudpsend = 0;
struct srey_ctx *psrey;
//int32_t ifamily = AF_INET6;
//const char *bindip = "::";
//const char *linkip = "::1";
int32_t ifamily = AF_INET;
const char *bindip = "0.0.0.0";
const char *linkip = "127.0.0.1";
void sig_exit(int32_t isig)
{
    istop = 1;
}
struct listener_ctx *plsn;
int32_t lsn_init(struct task_ctx *ptask, void *pinst, void *pudata)
{
    plsn = srey_listener(ptask, bindip, 15000);
    if (NULL == plsn)
    {
        return ERR_FAILED;
    }
    return ERR_OK;
}
void lsn_release(struct task_ctx *ptask, void *pinst, void *pudata)
{
    srey_freelsn(plsn);
}
void lsn_cb(struct task_ctx *ptask, uint32_t itype, uint64_t srcid, uint32_t uisess, void *pmsg, uint32_t uisize, void *pudata)
{
    struct sock_ctx *psock = pmsg;
    char acname[NAME_LENS] = { 0 };
    SNPRINTF(acname, sizeof(acname), "tcp gate%d", rand() % 4);
    struct task_ctx *pbind = srey_grabnam(psrey, acname);
    if (NULL == pbind)
    {
        sock_close(psock);
        return;
    }
    srey_enable(pbind, psock, 1);
    srey_release(pbind);
    ATOMIC_ADD(&uilinkcnt, 1);
}
void tcp_gate_cb(struct task_ctx *ptask, uint32_t itype, uint64_t srcid, uint32_t uisess, void *pmsg, uint32_t uisize, void *pudata)
{
    struct buffer_ctx *pbuf;
    int32_t iremoved, isendsize;
    char acpack[4096];
    struct sock_ctx *psock = pmsg;
    switch (itype)
    {
    case MSG_TYPE_RECV:
        ATOMIC_ADD(&uitcprecv, 1);
        pbuf = sock_buffer_r(psock);
        while (uisize > 0)
        {
            if (uisize >= sizeof(acpack))
            {
                iremoved = buffer_remove(pbuf, acpack, sizeof(acpack));
                ASSERTAB(sizeof(acpack) == iremoved, "tcp_gate_cb  1111111111111111111111");
                isendsize = sizeof(acpack);
                uisize -= sizeof(acpack);
            }
            else
            {
                iremoved = buffer_remove(pbuf, acpack, uisize);
                ASSERTAB(iremoved == uisize, "tcp_gate_cb  2222222222222222222222");
                isendsize = uisize;
                uisize = 0;
            }
            sock_send(psock, acpack, isendsize);
            if (rand() % psock->sock == 0)
            {
                sock_close(psock);
            }
        }
        break;
    case MSG_TYPE_SEND:
        ATOMIC_ADD(&uitcpsend, 1);
        break;
    case MSG_TYPE_CLOSE:
        ATOMIC_ADD(&uilinkcnt, -1);
        break;
    case MSG_TYPE_REQUEST:
        if (0 != srcid)
        {
            struct task_ctx *pto = srey_grabid(psrey, srcid);
            if (NULL != pto)
            {
                srey_response(pto, uisess, NULL, 0);
                srey_release(pto);
            }
        }
        else
        {
            PRINTF("MSG_TYPE_REQUEST call %s", task_name(ptask));
        }
        break;
    }
}
struct sock_ctx *pudpsock;
int32_t udp_init(struct task_ctx *ptask, void *pinst, void *pudata)
{
    SOCKET sock = sock_udp_bind(bindip, 15001);
    pudpsock = srey_newsock(ptask, sock, SOCK_DGRAM, ifamily);
    srey_enable(ptask, pudpsock, 1);
    srey_timeout(ptask, task_new_session(ptask), 2000);
    return ERR_OK;
}
void udp_release(struct task_ctx *ptask, void *pinst, void *pudata)
{
    sock_close(pudpsock);
}
void udp_gate_cb(struct task_ctx *ptask, uint32_t itype, uint64_t srcid, uint32_t uisess, void *pmsg, uint32_t uisize, void *pudata)
{
    int32_t iremoved, iold;
    struct buffer_ctx *pbuf;
    char acpack[4096];
    struct udp_recv_msg *pudpmsg;
    switch (itype)
    {
    case MSG_TYPE_RECVFROM:
        ATOMIC_ADD(&uiudprecv, 1);
        pudpmsg = pmsg;
        pbuf = sock_buffer_r(pudpmsg->sock);
        iold = (int32_t)buffer_size(pbuf);
        iremoved = buffer_remove(pbuf, acpack, uisize);
        if (iremoved != uisize)
        {
            PRINTF("%d  %d  %d", iold, iremoved, uisize);
            ASSERTAB(0, "udp_gate_cb 111111111111111111111111111");
        }
        sock_sendto(pudpmsg->sock, pudpmsg->ip, pudpmsg->port, acpack, uisize);
        break;
    case MSG_TYPE_SEND:
        ATOMIC_ADD(&uiudpsend, 1);
        break;
    case MSG_TYPE_CLOSE:
        break;
    case MSG_TYPE_TIMEOUT:
        PRINTF("tcp recv %d send %d  udp recv %d send %d  linkcnt: %d", 
            ATOMIC_GET(&uitcprecv), ATOMIC_GET(&uitcpsend), ATOMIC_GET(&uiudprecv), ATOMIC_GET(&uiudpsend), ATOMIC_GET(&uilinkcnt));
        char acname[NAME_LENS] = { 0 };
        SNPRINTF(acname, sizeof(acname), "tcp gate%d", rand() % 4);
        struct task_ctx *pto  = srey_grabnam(psrey, acname);
        if (NULL != pto)
        {
            if (rand() % 2 == 0)
            {
                srey_call(pto, NULL, 0);
            }
            else
            {
                srey_request(pto, task_id(ptask), task_new_session(ptask), NULL, 0);
            }
            srey_release(pto);
        }
        srey_timeout(ptask, task_new_session(ptask), 2000);
        break;
    case MSG_TYPE_RESPONSE:
        PRINTF("MSG_TYPE_RESPONSE %s %d", task_name(ptask), uisess);
        break;
    }
}
struct sock_ctx *pconnsock;
int32_t conn_init(struct task_ctx *ptask, void *pinst, void *pudata)
{
    pconnsock = srey_connecter(ptask, task_new_session(ptask), 3000, linkip, 15000);
    srey_connecter(ptask, task_new_session(ptask), 3000, "192.168.92.150", 15003);
    return ERR_OK;
}
void conn_release(struct task_ctx *ptask, void *pinst, void *pudata)
{
    sock_close(pconnsock);
}
void conn_cb(struct task_ctx *ptask, uint32_t itype, uint64_t srcid, uint32_t uisess, void *pmsg, uint32_t uisize, void *pudata)
{
    switch (itype)
    {
    case MSG_TYPE_CONNECT:
        PRINTF("sock connect error: %d.", uisize);
        struct sock_ctx *psock = pmsg;
        if (ERR_OK == uisize)
        {
            char acname[NAME_LENS] = { 0 };
            SNPRINTF(acname, sizeof(acname), "tcp gate%d", rand() % 4);
            struct task_ctx *pbind = srey_grabnam(psrey, acname);
            if (NULL == pbind)
            {
                sock_close(psock);
            }
            else
            {
                srey_enable(pbind, psock, 1);
                srey_release(pbind);
                ATOMIC_ADD(&uilinkcnt, 1);
            }
        }
        else
        {
            PRINTF("sock id %"PRIu64" session %d connect error.", psock->id, uisess);
        }
        break;
    }
}
int main(int argc, char *argv[])
{
    sighandle(sig_exit);
    unlimit();
    LOGINIT();
    SETLOGPRT(1);
    LOG_DEBUG("%s", "LOG_DEBUG");
    LOG_INFO("%s", "LOG_INFO");
    LOG_WARN("%s", "LOG_WARN");
    LOG_ERROR("%s", "LOG_ERROR");
    LOG_FATAL("%s", "LOG_FATAL");

    struct module_ctx mdlsn;
    mdlsn.md_new = NULL;
    mdlsn.md_init = lsn_init;
    mdlsn.maxcnt = 4;
    const char *plsnname = "listener";
    size_t ilens = strlen(plsnname);
    memcpy(mdlsn.name, plsnname, ilens);
    mdlsn.name[ilens] = '\0';
    mdlsn.md_stop = lsn_release;
    mdlsn.md_run = lsn_cb;
    mdlsn.md_free = NULL;
    psrey = srey_new(0, NULL, 5000);
    srey_loop(psrey);
    srey_newtask(psrey, &mdlsn, NULL);

    struct module_ctx mdgate;
    mdgate.md_new = NULL;
    mdgate.md_init = NULL;
    mdgate.maxcnt = 4;
    mdgate.md_free = NULL;
    mdgate.md_stop = NULL;
    mdgate.md_run = tcp_gate_cb;
    for (int32_t i = 0; i < 4; i++)
    {
        ZERO(mdgate.name, sizeof(mdgate.name));
        SNPRINTF(mdgate.name, sizeof(mdgate.name) - 1, "tcp gate%d", i);
        srey_newtask(psrey, &mdgate, NULL);
    }

    mdgate.md_init = udp_init;
    mdgate.md_stop = udp_release;
    mdgate.md_run = udp_gate_cb;
    ZERO(mdgate.name, sizeof(mdgate.name));
    SNPRINTF(mdgate.name, sizeof(mdgate.name), "%s", "udp gate");
    srey_newtask(psrey, &mdgate, NULL);

    mdgate.md_init = conn_init;
    mdgate.md_stop = conn_release;
    mdgate.md_run = conn_cb;
    SNPRINTF(mdgate.name, sizeof(mdgate.name), "%s", "connecter");
    srey_newtask(psrey, &mdgate, NULL);

    while (0 == istop)
    {
        MSLEEP(100);
    }
    srey_free(psrey);
    LOGFREE();
    return 0;
}
