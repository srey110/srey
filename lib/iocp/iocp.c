#include "iocp/iocp.h"
#include "thread.h"
#include "netutils.h"
#include "utils.h"
#include "loger.h"

#if defined(OS_WIN)
#define MAX_RECV_IOV_SIZE   4096
#define MAX_RECV_IOV_COUNT  4
#define MAX_SEND_IOV_SIZE   4096
#define MAX_SEND_IOV_COUNT  16
#define MAX_ACCEPT_SOCKEX   128
#define NOTIFIEXIT_KEY ((ULONG_PTR)-1)
typedef BOOL(WINAPI *AcceptExPtr)(SOCKET, SOCKET, PVOID, DWORD, DWORD, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL(WINAPI *ConnectExPtr)(SOCKET, const struct sockaddr *, int, PVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef void (WINAPI *GetAcceptExSockaddrsPtr)(PVOID, DWORD, DWORD, DWORD, LPSOCKADDR *, LPINT, LPSOCKADDR *, LPINT);
typedef struct sock_ol
{
    OVERLAPPED overlapped;
    int32_t evtype;
    DWORD bytes;
    struct sock_ctx *sockctx;
}sock_ol;
typedef struct accept_ol
{
    struct sock_ol ol;
    SOCKET sock;
    char addrbuf[(sizeof(struct sockaddr_storage) + 16) * 2];
}accept_ol;
struct recv_ol
{
    struct sock_ol ol;
    DWORD flag;
    size_t iovcount;
    IOV_TYPE wsabuf[MAX_RECV_IOV_COUNT];
}recv_ol;
struct send_ol
{
    struct sock_ol ol;
    size_t iovcount;
    IOV_TYPE wsabuf[MAX_SEND_IOV_COUNT];
}send_ol;
struct rw_ol
{
    struct recv_ol recvol;
    struct send_ol sendol;
}rw_ol;
typedef struct exptr_ctx
{
    AcceptExPtr accept;
    ConnectExPtr connect;
    GetAcceptExSockaddrsPtr acceptaddrs;
}exptr_ctx;
typedef struct iocp_ctx
{
    struct netio_ctx netio;
    struct exptr_ctx exfunc;
    HANDLE ioport;
}iocp_ctx;
//获取EX函数
static void *_getexfunc(const SOCKET fd, GUID  *pguid)
{
    void *pfunc = NULL;
    DWORD dbytes = 0;

    int32_t irtn = WSAIoctl(fd,
        SIO_GET_EXTENSION_FUNCTION_POINTER,
        pguid, 
        sizeof(GUID),
        &pfunc, 
        sizeof(pfunc),
        &dbytes,
        NULL, 
        NULL);
    ASSERTAB(irtn != SOCKET_ERROR, ERRORSTR(ERRNO));

    return pfunc;
}
//获取EX函数
static void _initexfunc(struct exptr_ctx *exfunc)
{
    GUID accept_uid = WSAID_ACCEPTEX;
    GUID connect_uid = WSAID_CONNECTEX;
    GUID acceptaddrs_uid = WSAID_GETACCEPTEXSOCKADDRS;
    SOCKET sock = WSASocket(AF_INET, 
        SOCK_STREAM,
        0, 
        NULL,
        0,
        0);
    ASSERTAB(INVALID_SOCK != sock, ERRORSTR(ERRNO));

    exfunc->accept = (AcceptExPtr)_getexfunc(sock, &accept_uid);
    exfunc->connect = (ConnectExPtr)_getexfunc(sock, &connect_uid);
    exfunc->acceptaddrs = (GetAcceptExSockaddrsPtr)_getexfunc(sock, &acceptaddrs_uid);

    SAFE_CLOSESOCK(sock);
}
//初始化
struct netio_ctx *netio_new(volatile atomic_t *pstop)
{
    WSADATA wsdata;
    WORD ver = MAKEWORD(2, 2);
    ASSERTAB(ERR_OK == WSAStartup(ver, &wsdata), ERRORSTR(ERRNO));

    struct iocp_ctx *piocp = (struct iocp_ctx *)MALLOC(sizeof(struct iocp_ctx));
    ASSERTAB(NULL != piocp, ERRSTR_MEMORY);
    piocp->netio.stop = pstop;
    piocp->netio.threadnum = (int32_t)procsnum() * 2 + 2;
    piocp->netio.thread = MALLOC(sizeof(struct thread_ctx) * piocp->netio.threadnum);
    ASSERTAB(NULL != piocp->netio.thread, ERRSTR_MEMORY);
    for (int32_t i = 0; i < piocp->netio.threadnum; i++)
    {
        thread_init(&piocp->netio.thread[i]);
    }
    _initexfunc(&piocp->exfunc);
    piocp->ioport = CreateIoCompletionPort(INVALID_HANDLE_VALUE, 
        NULL,
        0,
        piocp->netio.threadnum);
    ASSERTAB(NULL != piocp->ioport, ERRORSTR(ERRNO));

    return &piocp->netio;
}
//释放
void netio_free(struct netio_ctx *pctx)
{
    int32_t i;
    struct iocp_ctx *piocp = UPCAST(pctx, struct iocp_ctx, netio);
    for (i = 0; i < piocp->netio.threadnum; i++)
    {
        if (!PostQueuedCompletionStatus(piocp->ioport, 
            0,
            NOTIFIEXIT_KEY, 
            NULL))
        {
            LOG_ERROR("%s", ERRORSTR(ERRNO));
        }
    }
    for (i = 0; i < piocp->netio.threadnum; i++)
    {
        thread_join(&piocp->netio.thread[i]);
    }

    (void)CloseHandle(piocp->ioport);
    SAFE_FREE(piocp->netio.thread);
    SAFE_FREE(piocp);
    (void)WSACleanup();
}
//创建一套接字并发起AcceptEX
static inline int32_t _post_accept(struct exptr_ctx *pexfunc, 
    struct sock_ctx *plistensock, struct accept_ol *poverlapped)
{
    int32_t irtn;
    SOCKET sock = WSASocket(netaddr_addrfamily(&plistensock->addr), 
        SOCK_STREAM,
        IPPROTO_TCP,
        NULL, 
        0, 
        WSA_FLAG_OVERLAPPED);
    if (INVALID_SOCK == sock)
    {
        irtn = ERRNO;
        LOG_ERROR("%s", ERRORSTR(irtn));
        return irtn;
    }
    
    poverlapped->sock = sock;
    if (!pexfunc->accept(plistensock->sock,         //ListenSocket
        poverlapped->sock,                     //AcceptSocket
        &poverlapped->addrbuf,
        0,
        sizeof(poverlapped->addrbuf) / 2,
        sizeof(poverlapped->addrbuf) / 2,
        &poverlapped->ol.bytes,
        &poverlapped->ol.overlapped))
    {
        irtn = ERRNO;
        if (ERROR_IO_PENDING != irtn)
        {
            LOG_FATAL("%s", ERRORSTR(irtn));
            SAFE_CLOSESOCK(sock);
            return irtn;
        }
    }

    return ERR_OK;
}
//申请iov
static inline size_t _recv_iov_application(struct buffer_ctx *pbuf, size_t uisize, 
    IOV_TYPE *wsabuf, size_t uiovlens)
{
    buffer_lock(pbuf);
    ASSERTAB(0 == pbuf->freeze_write, "buffer tail already freezed.");
    pbuf->freeze_write = 1;
    size_t uicoun = _buffer_expand_iov(pbuf, uisize, wsabuf, uiovlens);
    buffer_unlock(pbuf);

    return uicoun;
}
//提交iov
static inline void _recv_iov_commit(struct buffer_ctx *pbuf, size_t ilens, 
    IOV_TYPE *piov, const size_t uicount)
{
    buffer_lock(pbuf);
    ASSERTAB(0 != pbuf->freeze_write, "buffer tail already unfreezed.");
    size_t uisize = _buffer_commit_iov(pbuf, ilens, piov, uicount);
    ASSERTAB(uisize == ilens, "_buffer_commit_iov lens error.");
    pbuf->freeze_write = 0;
    buffer_unlock(pbuf);
}
//WSARecv
static inline int32_t _post_recv(struct sock_ctx *psock)
{
    struct rw_ol *prwol = (struct rw_ol *)psock->data;
    ZERO(&prwol->recvol.ol.overlapped, sizeof(prwol->recvol.ol.overlapped));
    prwol->recvol.flag = 0;
    prwol->recvol.iovcount = _recv_iov_application(psock->bufrecv, MAX_RECV_IOV_SIZE,
        prwol->recvol.wsabuf, MAX_RECV_IOV_COUNT);
    int32_t irtn = WSARecv(psock->sock,
        prwol->recvol.wsabuf,
        (DWORD)prwol->recvol.iovcount,
        &prwol->recvol.ol.bytes,
        &prwol->recvol.flag,
        &prwol->recvol.ol.overlapped,
        NULL);
    if (ERR_OK != irtn)
    {
        irtn = ERRNO;
        if (WSA_IO_PENDING != irtn)
        {
            _recv_iov_commit(psock->bufrecv, 0, prwol->recvol.wsabuf, prwol->recvol.iovcount);
            return irtn;
        }
    }

    return ERR_OK;
}
static inline size_t _send_iov_application(struct buffer_ctx *pbuf, size_t uisize,
    IOV_TYPE *wsabuf, size_t uiovlens)
{
    size_t uicount = 0;

    buffer_lock(pbuf);
    if (0 == pbuf->freeze_read)
    {
        uicount = _buffer_get_iov(pbuf, uisize, wsabuf, uiovlens);
        if (uicount > 0)
        {
            pbuf->freeze_read = 1;
        }
    }
    buffer_unlock(pbuf);

    return uicount;
}
static inline void _send_iov_commit(struct buffer_ctx *pbuf, size_t uilens)
{
    buffer_lock(pbuf);
    ASSERTAB(1 == pbuf->freeze_read, "logic error.");
    ASSERTAB(uilens == _buffer_drain(pbuf, uilens), "logic error.");
    pbuf->freeze_read = 0;
    buffer_unlock(pbuf);
}
static inline void _send_iov_commit_failed(struct buffer_ctx *pbuf)
{
    buffer_lock(pbuf);
    pbuf->freeze_read = 0;
    buffer_unlock(pbuf);
}
int32_t _post_send(struct sock_ctx *psock)
{
    struct rw_ol *prwol = (struct rw_ol *)psock->data;    
    size_t uicount = _send_iov_application(psock->bufsend, MAX_SEND_IOV_SIZE, 
        prwol->sendol.wsabuf, MAX_SEND_IOV_COUNT);
    if (0 == uicount)
    {
        return ERR_OK;
    }
    
    ZERO(&prwol->sendol.ol.overlapped, sizeof(prwol->sendol.ol.overlapped));
    prwol->sendol.iovcount = uicount;
    int32_t irtn = WSASend(psock->sock,
        prwol->sendol.wsabuf,
        (DWORD)uicount,
        &prwol->sendol.ol.bytes,
        0,
        &prwol->sendol.ol.overlapped,
        NULL);
    if (ERR_OK != irtn)
    {
        irtn = ERRNO;
        if (WSA_IO_PENDING != irtn)
        {
            _send_iov_commit_failed(psock->bufsend);
            return irtn;
        }
    }

    return ERR_OK;
}
//ConnectEX
int32_t _post_connect(struct exptr_ctx *pexfunc, struct sock_ctx *psock, struct sock_ol *poverlapped)
{
    ZERO(&poverlapped->overlapped, sizeof(poverlapped->overlapped));
    poverlapped->bytes = 0;
    if (!pexfunc->connect(psock->sock,
        netaddr_addr(&psock->addr),
        netaddr_size(&psock->addr),
        NULL,
        0,
        &poverlapped->bytes,
        &poverlapped->overlapped))
    {
        int32_t irtn = ERRNO;
        if (ERROR_IO_PENDING != irtn)
        {
            LOG_WARN("%s", ERRORSTR(irtn));
            return irtn;
        }
    }

    return ERR_OK;
}
static inline void _on_accept(struct iocp_ctx *piocp, struct sock_ol *psockol, int32_t ierr)
{
    struct accept_ol *pacceptol = UPCAST(psockol, struct accept_ol, ol);
    if (ERR_OK != ierr)
    {
        LOG_FATAL("%s", ERRORSTR(ierr));
        SAFE_CLOSESOCK(pacceptol->sock);
        ASSERTAB(ERR_OK == _post_accept(&piocp->exfunc, pacceptol->ol.sockctx, pacceptol),
            "_postaccept failed.");
        return;
    }

    struct sockaddr *plocal = NULL, *premote = NULL;
    int32_t ilocal = 0, iremote = 0;
    SOCKET sock = pacceptol->sock;
    //获取地址
    piocp->exfunc.acceptaddrs(pacceptol->addrbuf, 0,
        sizeof(pacceptol->addrbuf) / 2, sizeof(pacceptol->addrbuf) / 2,
        &plocal, &ilocal, &premote, &iremote);
    // 新加一个socket，继续AcceptEX
    ASSERTAB(ERR_OK == _post_accept(&piocp->exfunc, pacceptol->ol.sockctx, pacceptol),
        "_postaccept failed.");
    //设置，让getsockname, getpeername, shutdown可用
    (void)setsockopt(sock, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
        (char *)&pacceptol->ol.sockctx->sock, sizeof(&pacceptol->ol.sockctx->sock));
    //创建sock_ctx
    struct sock_ctx *pctx = sockctx_new(0);
    pctx->sock = sock;
    (void)netaddr_setaddr(&pctx->addr, premote);
    //加进IOCP
    if (NULL == CreateIoCompletionPort((HANDLE)pctx->sock, 
        piocp->ioport,
        (ULONG_PTR)pctx,
        piocp->netio.threadnum))
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        sockctx_free(pctx);
        return;
    }
    //用listen的chan投递EV_ACCEPT
    struct ev_ctx *pev = ev_new_accept(pctx);
    if (ERR_OK != sockctx_post_ev(pacceptol->ol.sockctx, pev))
    {
        LOG_ERROR("%s", "post accept event failed.");
        SAFE_FREE(pev);
        sockctx_free(pctx);
    }
}
static inline void _on_connect(struct sock_ol *psockol, int32_t ierr)
{
    struct sock_ctx *psock = psockol->sockctx;
    SAFE_FREE(psock->data);
    //投递EV_CONNECT
    struct ev_ctx *pev = ev_new_connect(psock, ierr);
    if (ERR_OK != sockctx_post_ev(psock, pev))
    {
        LOG_ERROR("%s", "post connect event failed.");
        SAFE_FREE(pev);
        sockctx_free(psock);
    }
}
static inline int32_t _is_close(struct sock_ol *psockol, int32_t ierr, DWORD dbytes)
{
    if (dbytes > 0
        && ERR_OK == ierr)
    {
        return ERR_FAILED;
    }

    struct sock_ctx *psock = psockol->sockctx;
    struct rw_ol *prwol = (struct rw_ol *)psock->data;
    _recv_iov_commit(psock->bufrecv, 0, prwol->recvol.wsabuf, prwol->recvol.iovcount);
    _send_iov_commit_failed(psock->bufsend);
    //投递EV_CLOSE
    struct ev_ctx *pev = ev_new_close(psock);
    if (ERR_OK != sockctx_post_ev(psock, pev))
    {
        LOG_ERROR("%s", "post close event failed.");
        SAFE_FREE(pev);
        sockctx_free(psock);
    }

    return ERR_OK;
}
static inline void _on_recv(struct sock_ol *psockol, int32_t ierr, DWORD dbytes)
{
    if (ERR_OK == _is_close(psockol, ierr, dbytes))
    {
        return;
    }

    struct sock_ctx *psock = psockol->sockctx;
    struct rw_ol *prwol = (struct rw_ol *)psock->data;
    _recv_iov_commit(psock->bufrecv, (size_t)dbytes, prwol->recvol.wsabuf, prwol->recvol.iovcount);
    //投递EV_RECV消息
    struct ev_ctx *pev = ev_new_recv(psock, (size_t)dbytes);
    if (ERR_OK != sockctx_post_ev(psock, pev))
    {
        LOG_ERROR("%s", "post recv event failed.");
        SAFE_FREE(pev);
    }
    //继续接收
    int32_t irtn = _post_recv(psock);
    if (ERR_OK != irtn)
    {
        LOG_ERROR("%s", ERRORSTR(irtn));
        //投递EV_CLOSE
        pev = ev_new_close(psock);
        if (ERR_OK != sockctx_post_ev(psock, pev))
        {
            LOG_ERROR("%s", "post close event failed.");
            SAFE_FREE(pev);
            sockctx_free(psock);
        }
    }
}
static inline void _on_send(struct sock_ol *psockol, int32_t ierr, DWORD dbytes)
{
    if (0 == dbytes
        || ERR_OK != ierr)
    {
        return;
    }

    _send_iov_commit(psockol->sockctx->bufsend, (size_t)dbytes);
    if (0 != psockol->sockctx->postsendev)
    {
        //投递EV_SEND消息
        struct ev_ctx *pev = ev_new_send(psockol->sockctx, (size_t)dbytes);
        if (ERR_OK != sockctx_post_ev(psockol->sockctx, pev))
        {
            LOG_ERROR("%s", "post send event failed.");
            SAFE_FREE(pev);
        }
    }
    //尝试继续发送
    (void)_post_send(psockol->sockctx);
}
//IOCP循环
static void _icop_loop(void *p1, void *p2, void *p3)
{
    BOOL bok;
    DWORD dbytes;
    ULONG_PTR ulkey;
    int32_t ierr;
    OVERLAPPED *poverlapped;
    struct sock_ol *psockol;
    struct iocp_ctx *piocp = (struct iocp_ctx *)p1;
    while (0 == ATOMIC_GET(piocp->netio.stop))
    {
        ierr = ERR_OK;
        bok = GetQueuedCompletionStatus(piocp->ioport, 
            &dbytes, 
            &ulkey, 
            &poverlapped, 
            INFINITE);
        if (NOTIFIEXIT_KEY == ulkey)
        {
            break;
        }
        if (!bok)
        {
            ierr = ERRNO;
            if (NULL == poverlapped
                || WAIT_TIMEOUT == ierr)
            {
                continue;
            }
        }

        psockol = UPCAST(poverlapped, struct sock_ol, overlapped);
        switch (psockol->evtype)
        {
            case EV_ACCEPT:
                _on_accept(piocp, psockol, ierr);
                break;
            case EV_CONNECT:
                _on_connect(psockol, ierr);
                break;
            case EV_RECV:
                _on_recv(psockol, ierr, dbytes);
                break;
            case EV_SEND:
                _on_send(psockol, ierr, dbytes);
                break;
        }
    }
}
//启动工作线程
void netio_run(struct netio_ctx *pctx)
{
    struct iocp_ctx *piocp = UPCAST(pctx, struct iocp_ctx, netio);
    for (int32_t i = 0; i < piocp->netio.threadnum; i++)
    {
        thread_creat(&piocp->netio.thread[i], _icop_loop, pctx, NULL, NULL);
        thread_waitstart(&piocp->netio.thread[i]);
    }
}
//初始化一struct sock_ctx
static inline struct sock_ctx *_init_tcp_sock(struct chan_ctx *pchan,
    const char *phost, const uint16_t usport, const uint8_t uclistener)
{
    struct sock_ctx *psock = sockctx_new(uclistener);
    int32_t irtn = netaddr_sethost(&psock->addr, phost, usport);
    if (ERR_OK != irtn)
    {
        LOG_ERROR("%s", gai_strerror(irtn));
        sockctx_free(psock);
        return NULL;
    }

    psock->chan = pchan;
    psock->sock = WSASocket(netaddr_addrfamily(&psock->addr), 
        SOCK_STREAM,
        IPPROTO_TCP,
        NULL,
        0, 
        WSA_FLAG_OVERLAPPED);
    if (INVALID_SOCK == psock->sock)
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        sockctx_free(psock);
        return NULL;
    }

    return psock;
}
int32_t _accptex(struct iocp_ctx *piocp, struct sock_ctx *psock)
{
    int32_t irtn;
    if (NULL == CreateIoCompletionPort((HANDLE)psock->sock,
        piocp->ioport,
        (ULONG_PTR)psock,
        piocp->netio.threadnum))
    {
        irtn = ERRNO;
        LOG_ERROR("%s", ERRORSTR(irtn));
        return irtn;
    }

    ASSERTAB(NULL == psock->data, "date used by another.");
    struct accept_ol *poverlapped = MALLOC(sizeof(struct accept_ol) * MAX_ACCEPT_SOCKEX);
    ASSERTAB(NULL != poverlapped, ERRSTR_MEMORY);
    psock->data = poverlapped;

    for (int32_t i = 0; i < MAX_ACCEPT_SOCKEX; i++)
    {
        ZERO(&poverlapped[i].ol.overlapped, sizeof(poverlapped[i].ol.overlapped));
        poverlapped[i].ol.evtype = EV_ACCEPT;
        poverlapped[i].ol.sockctx = psock;
        poverlapped[i].ol.bytes = 0;
        irtn = _post_accept(&piocp->exfunc, psock, &poverlapped[i]);
        if (ERR_OK != irtn)
        {
            SAFE_FREE(psock->data);
            return irtn;
        }
    }

    return ERR_OK;
}
struct sock_ctx *netio_listen(struct netio_ctx *pctx, struct chan_ctx *pchan,
    size_t ichannum, const char *phost, const uint16_t usport)
{
    ASSERTAB(ichannum >= 1, "param error.");
    struct sock_ctx *psock = _init_tcp_sock(pchan, phost, usport, 1);
    if (NULL == psock)
    {
        return NULL;
    }

    psock->channum = ichannum;
    sockraddr(psock->sock);
    if (ERR_OK != bind(psock->sock, netaddr_addr(&psock->addr), netaddr_size(&psock->addr)))
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        sockctx_free(psock);
        return NULL;
    }
    if (ERR_OK != listen(psock->sock, SOCKK_BACKLOG))
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        sockctx_free(psock);
        return NULL;
    }
    if (ERR_OK != _accptex(UPCAST(pctx, struct iocp_ctx, netio), psock))
    {
        sockctx_free(psock);
        return NULL;
    }

    return psock;
}
int32_t _trybind(struct sock_ctx *psock)
{
    int32_t irtn;
    struct netaddr_ctx addr;
    if (ERR_OK == netaddr_isipv4(&psock->addr))
    {
        irtn = netaddr_sethost(&addr, "0.0.0.0", 0);
    }
    else
    {
        irtn = netaddr_sethost(&addr, "0:0:0:0:0:0:0:0", 0);
    }
    if (ERR_OK != irtn)
    {
        LOG_ERROR("%s", gai_strerror(irtn));
        return irtn;
    }

    if (ERR_OK != bind(psock->sock, netaddr_addr(&addr), netaddr_size(&addr)))
    {
        irtn = ERRNO;
        LOG_ERROR("%s", ERRORSTR(irtn));
        return irtn;
    }

    return ERR_OK;
}
int32_t _connectex(struct iocp_ctx *piocp, struct sock_ctx *psock)
{
    int32_t irtn;
    if (NULL == CreateIoCompletionPort((HANDLE)psock->sock, 
        piocp->ioport,
        (ULONG_PTR)psock,
        piocp->netio.threadnum))
    {
        irtn = ERRNO;
        LOG_ERROR("%s", ERRORSTR(irtn));
        return irtn;
    }

    ASSERTAB(NULL == psock->data, "date used by another.");
    struct sock_ol *poverlapped = (struct sock_ol *)MALLOC(sizeof(struct sock_ol));
    ASSERTAB(NULL != poverlapped, ERRSTR_MEMORY);
    psock->data = poverlapped;

    poverlapped->evtype = EV_CONNECT;
    poverlapped->sockctx = psock;
    irtn = _post_connect(&piocp->exfunc, psock, poverlapped);
    if (ERR_OK != irtn)
    {
        SAFE_FREE(psock->data);
    }

    return irtn;
}
struct sock_ctx *netio_connect(struct netio_ctx *pctx, struct chan_ctx *pchan,
    const char *phost, const uint16_t usport)
{
    struct sock_ctx *psock = _init_tcp_sock(pchan, phost, usport, 0);
    if (NULL == psock)
    {
        return NULL;
    }

    int32_t irtn = _trybind(psock);
    if (ERR_OK != irtn)
    {
        sockctx_free(psock);
        return NULL;
    }
    if (ERR_OK != _connectex(UPCAST(pctx, struct iocp_ctx, netio), psock))
    {
        sockctx_free(psock);
        return NULL;
    }

    return psock;
}
struct sock_ctx *netio_addsock(struct netio_ctx *pctx, SOCKET fd)
{
    WSAPROTOCOL_INFO info;
    int32_t ilens = (int32_t)sizeof(info);
    if (getsockopt(fd, SOL_SOCKET, SO_PROTOCOL_INFO, (char *)&info, &ilens) < ERR_OK)
    {
        LOG_ERROR("getsockopt(%d, SOL_SOCKET, SO_PROTOCOL_INFO, ...) failed. %s", (int32_t)fd, ERRORSTR(ERRNO));
        return NULL;
    }
    if (SOCK_DGRAM == info.iSocketType)
    {
        LOG_ERROR("%s", "not support udp.");
        return NULL;
    }

    socknbio(fd);
    struct sock_ctx *psock = sockctx_new(0);
    psock->sock = fd;

    struct iocp_ctx *piocp = UPCAST(pctx, struct iocp_ctx, netio);
    if (NULL == CreateIoCompletionPort((HANDLE)psock->sock,
        piocp->ioport,
        (ULONG_PTR)psock,
        piocp->netio.threadnum))
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        sockctx_free(psock);
        return NULL;
    }

    return psock;
}
int32_t netio_enable_rw(struct sock_ctx *psock, struct chan_ctx *pchan, const uint8_t ucpostsendev)
{
    ASSERTAB(0 == psock->listener, "can't enbale read write on listen socket.");

    int32_t irtn;
    psock->chan = pchan;
    psock->postsendev = ucpostsendev;
    socknodelay(psock->sock);
    sockkpa(psock->sock, SOCKKPA_DELAY, SOCKKPA_INTVL);

    ASSERTAB(NULL == psock->data, "data used by another.");
    struct rw_ol *prwol = (struct rw_ol *)MALLOC(sizeof(struct rw_ol));
    ASSERTAB(NULL != prwol, ERRSTR_MEMORY);
    psock->data = prwol;

    prwol->sendol.ol.evtype = EV_SEND;
    prwol->sendol.ol.sockctx = psock;
    prwol->sendol.ol.bytes = 0;

    prwol->recvol.ol.sockctx = psock;
    prwol->recvol.ol.evtype = EV_RECV;
    prwol->recvol.ol.bytes = 0;
    irtn = _post_recv(psock);
    if (ERR_OK != irtn)
    {
        LOG_ERROR("%s", ERRORSTR(irtn));
        SAFE_FREE(psock->data);
    }

    return irtn;
}
int32_t netio_send(struct sock_ctx *psock, void *pdata, size_t ilens)
{
    ASSERTAB(ERR_OK == buffer_append(psock->bufsend, pdata, ilens), "buffer_append failed.");
    return _post_send(psock);
}

#endif // OS_WIN
