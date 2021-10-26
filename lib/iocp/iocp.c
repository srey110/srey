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
#define MAX_RECV_FROM_IOV_SIZE   4096
#define MAX_RECV_FROM_IOV_COUNT  4
#define MAX_ACCEPT_SOCKEX   128
#define NOTIFIEXIT_KEY ((ULONG_PTR)-1)

typedef BOOL(WINAPI *DisconnectExPtr)(SOCKET, LPOVERLAPPED, DWORD, DWORD);
typedef BOOL(WINAPI *AcceptExPtr)(SOCKET, SOCKET, PVOID, DWORD, DWORD, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL(WINAPI *ConnectExPtr)(SOCKET, const struct sockaddr *, int, PVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef void (WINAPI *GetAcceptExSockaddrsPtr)(PVOID, DWORD, DWORD, DWORD, LPSOCKADDR *, LPINT, LPSOCKADDR *, LPINT);
typedef struct sock_ol//EV_CLOSE  EV_CONNECT
{
    OVERLAPPED overlapped;
    int32_t evtype;
    DWORD bytes;
    struct sock_ctx *sockctx;
}sock_ol;
typedef struct accept_ol//EV_ACCEPT
{
    struct sock_ol ol;
    SOCKET sock;
    char addrbuf[(sizeof(struct sockaddr_storage) + 16) * 2];
}accept_ol;
struct recv_ol//EV_RECV
{
    struct sock_ol ol;
    DWORD flag;
    size_t iovcount;
    IOV_TYPE wsabuf[MAX_RECV_IOV_COUNT];
}recv_ol;
struct send_ol//EV_SEND
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
struct recv_from_ol//udp EV_RECV
{
    struct sock_ol ol;
    DWORD flag;
    size_t iovcount;
    IOV_TYPE wsabuf[MAX_RECV_FROM_IOV_COUNT];
    struct sockaddr_storage  rmtaddr;  //存储数据来源IP地址
    int32_t addrlen;                   //存储数据来源IP地址长度
}recv_from_ol;
struct send_to_ol//udp EV_SEND
{
    struct sock_ol ol;
    size_t iovcount;
    IOV_TYPE *wsabuf;
    struct netaddr_ctx addr;
}send_to_ol;
typedef struct iocp_ctx
{
    struct netio_ctx netio;
    HANDLE ioport;
    AcceptExPtr acceptex;
    ConnectExPtr connectex;
    GetAcceptExSockaddrsPtr acceptaddrsex;
    DisconnectExPtr disconnectex;
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
static void _initexfunc(struct iocp_ctx *piocp)
{
    GUID accept_uid = WSAID_ACCEPTEX;
    GUID connect_uid = WSAID_CONNECTEX;
    GUID acceptaddrs_uid = WSAID_GETACCEPTEXSOCKADDRS;
    GUID disconnect_uid = WSAID_DISCONNECTEX;
    SOCKET sock = WSASocket(AF_INET, 
        SOCK_STREAM,
        0, 
        NULL,
        0,
        0);
    ASSERTAB(INVALID_SOCK != sock, ERRORSTR(ERRNO));

    piocp->acceptex = (AcceptExPtr)_getexfunc(sock, &accept_uid);
    piocp->connectex = (ConnectExPtr)_getexfunc(sock, &connect_uid);
    piocp->acceptaddrsex = (GetAcceptExSockaddrsPtr)_getexfunc(sock, &acceptaddrs_uid);
    piocp->disconnectex = (DisconnectExPtr)_getexfunc(sock, &disconnect_uid);

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

    _initexfunc(piocp);
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
            int32_t irtn = ERRNO;
            LOG_ERROR("error code:%d message:%s", irtn, ERRORSTR(irtn));
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
static inline int32_t _post_accept(struct iocp_ctx *piocp, struct sock_ctx *plistensock, struct accept_ol *poverlapped)
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
        LOG_ERROR("error code:%d message:%s", irtn, ERRORSTR(irtn));
        return irtn;
    }
    
    poverlapped->sock = sock;
    if (!piocp->acceptex(plistensock->sock,         //ListenSocket
        poverlapped->sock,                          //AcceptSocket
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
            LOG_FATAL("error code:%d message:%s", irtn, ERRORSTR(irtn));
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
    prwol->recvol.iovcount = _recv_iov_application(psock->bufrecv, 
        MAX_RECV_IOV_SIZE, prwol->recvol.wsabuf, MAX_RECV_IOV_COUNT);

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
static inline int32_t _post_recv_from(struct sock_ctx *psock)
{
    struct recv_from_ol *poverlapped = (struct recv_from_ol *)psock->data;
    ZERO(&(poverlapped->ol.overlapped), sizeof(poverlapped->ol.overlapped));
    poverlapped->flag = 0;
    poverlapped->ol.bytes = 0;
    poverlapped->addrlen = sizeof(poverlapped->rmtaddr);
    poverlapped->iovcount = _recv_iov_application(psock->bufrecv, 
        MAX_RECV_FROM_IOV_SIZE, poverlapped->wsabuf, MAX_RECV_FROM_IOV_COUNT);

    int32_t irtn = WSARecvFrom(
        psock->sock,
        poverlapped->wsabuf,
        (DWORD)poverlapped->iovcount,
        &poverlapped->ol.bytes,
        &poverlapped->flag,
        (struct sockaddr*)&(poverlapped->rmtaddr),
        &(poverlapped->addrlen),
        &(poverlapped->ol.overlapped),
        NULL);
    if (ERR_OK != irtn)
    {
        irtn = ERRNO;
        if (WSA_IO_PENDING != irtn)
        {
            _recv_iov_commit(psock->bufrecv, 0, poverlapped->wsabuf, poverlapped->iovcount);
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
    ASSERTAB(1 == pbuf->freeze_read, "buffer head already unfreezed.");
    ASSERTAB(uilens == _buffer_drain(pbuf, uilens), "drain size not equ commit size.");
    pbuf->freeze_read = 0;
    buffer_unlock(pbuf);
}
static inline void _send_iov_commit_failed(struct buffer_ctx *pbuf)
{
    buffer_lock(pbuf);
    ASSERTAB(1 == pbuf->freeze_read, "buffer head already unfreezed.");
    pbuf->freeze_read = 0;
    buffer_unlock(pbuf);
}
static inline int32_t _post_send(struct sock_ctx *psock)
{
    struct rw_ol *prwol = (struct rw_ol *)psock->data;    
    size_t uicount = _send_iov_application(psock->bufsend, 
        MAX_SEND_IOV_SIZE, prwol->sendol.wsabuf, MAX_SEND_IOV_COUNT);
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
static inline int32_t _post_connect(struct iocp_ctx *piocp, struct sock_ctx *psock)
{
    struct sock_ol *poverlapped = (struct sock_ol *)MALLOC(sizeof(struct sock_ol));
    ASSERTAB(NULL != poverlapped, ERRSTR_MEMORY);
    psock->data = poverlapped;
    ZERO(&poverlapped->overlapped, sizeof(poverlapped->overlapped));
    poverlapped->evtype = EV_CONNECT;
    poverlapped->sockctx = psock;    
    poverlapped->bytes = 0;

    if (!piocp->connectex(psock->sock,
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
            LOG_WARN("error code:%d message:%s", irtn, ERRORSTR(irtn));
            SAFE_FREE(psock->data);
            return irtn;
        }
    }

    return ERR_OK;
}
static inline int32_t _post_disconnect(struct iocp_ctx *piocp, struct sock_ctx *psock)
{    
    struct sock_ol *pcloseol = (struct sock_ol *)MALLOC(sizeof(struct sock_ol));
    ASSERTAB(NULL != pcloseol, ERRSTR_MEMORY);
    ZERO(&pcloseol->overlapped, sizeof(pcloseol->overlapped));
    pcloseol->evtype = EV_CLOSE;
    pcloseol->sockctx = psock;
    struct rw_ol *prwol = (struct rw_ol *)psock->data;
    shutdown(psock->sock, SHUT_WR);
    //buffer_lock(psock->bufsend);
    //if (1 == psock->bufsend->freeze_read)
    //{
    //    if (!CancelIoEx((HANDLE)psock->sock, &prwol->sendol.ol.overlapped))
    //    {
    //        LOG_ERROR("%s", ERRORSTR(ERRNO));
    //    }
    //    //psock->bufsend->freeze_read = 0;
    //}
    //buffer_unlock(psock->bufsend);
    if (!piocp->disconnectex(psock->sock, &pcloseol->overlapped, 0, 0))
    {
        int32_t irtn = ERRNO;
        if (ERROR_IO_PENDING != irtn)
        {
            LOG_WARN("error code:%d message:%s", irtn, ERRORSTR(irtn));
            SAFE_FREE(pcloseol);
            return irtn;
        }
    }

    return ERR_OK;
}
void netio_close(struct sock_ctx *psock)
{
    PRINTF("netio_close %d", (int32_t)psock->sock);
    struct rw_ol *prwol = (struct rw_ol *)psock->data;
    shutdown(psock->sock, SHUT_RDWR);
    if (!CancelIoEx((HANDLE)psock->sock, NULL))
    {
        int32_t irtn = ERRNO;
        LOG_WARN("error code:%d message:%s", irtn, ERRORSTR(irtn));
    }
    /*if (SOCK_DGRAM == psock->socktype)
    {*/
        //CLOSESOCKET(psock->sock);
    //}
}
static inline void _on_accept(struct iocp_ctx *piocp, struct sock_ol *psockol, int32_t ierr)
{
    struct accept_ol *pacceptol = UPCAST(psockol, struct accept_ol, ol);
    SOCKET sock = pacceptol->sock;
    if (ERR_OK != ierr)
    {
        LOG_ERROR("error code:%d message:%s", ierr, ERRORSTR(ierr));
        CLOSESOCKET(sock);
        //还有其他不能继续的?
        if (ERROR_OPERATION_ABORTED != ierr)
        {
            (void)_post_accept(piocp, pacceptol->ol.sockctx, pacceptol);
        }
        return;
    }
    //获取地址
    struct sockaddr *plocal = NULL, *premote = NULL;
    int32_t ilocal = 0, iremote = 0;      
    piocp->acceptaddrsex(pacceptol->addrbuf, 0,
        sizeof(pacceptol->addrbuf) / 2, sizeof(pacceptol->addrbuf) / 2,
        &plocal, &ilocal, &premote, &iremote);
    // 新加一个socket，继续AcceptEX
    (void)_post_accept(piocp, pacceptol->ol.sockctx, pacceptol);
    //设置，让getsockname, getpeername, shutdown可用
    (void)setsockopt(sock, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
        (char *)&pacceptol->ol.sockctx->sock, sizeof(&pacceptol->ol.sockctx->sock));
    //创建sock_ctx
    struct sock_ctx *pctx = sockctx_new(0, SOCK_STREAM);
    pctx->sock = sock;
    (void)netaddr_setaddr(&pctx->addr, premote);
    //加进IOCP
    if (NULL == CreateIoCompletionPort((HANDLE)pctx->sock, 
        piocp->ioport,
        (ULONG_PTR)pctx,
        piocp->netio.threadnum))
    {
        int32_t irtn = ERRNO;
        LOG_ERROR("error code:%d message:%s", irtn, ERRORSTR(irtn));
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
static inline void _send_close_ev(struct sock_ctx *psock)
{
    struct ev_ctx *pev = ev_new_close(psock);
    if (ERR_OK != sockctx_post_ev(psock, pev))
    {
        LOG_ERROR("%s", "post close event failed.");
        SAFE_FREE(pev);
        sockctx_free(psock);
    }
}
static inline void _on_close(struct sock_ol *psockol, int32_t ierr)
{
    struct sock_ctx *psock = psockol->sockctx;
    SAFE_FREE(psockol);
    _send_close_ev(psock);
}
static inline int32_t _is_close(struct iocp_ctx *piocp, struct sock_ol *psockol, int32_t ierr, DWORD dbytes)
{
    if ((dbytes > 0
        && ERR_OK == ierr))
    {
        return ERR_FAILED;
    }

    struct sock_ctx *psock = psockol->sockctx;
    if (SOCK_DGRAM == psock->socktype)
    {
        struct recv_from_ol *precvfromol = UPCAST(psockol, struct recv_from_ol, ol);
        _recv_iov_commit(psock->bufrecv, 0, precvfromol->wsabuf, precvfromol->iovcount);
        //投递EV_CLOSE
        _send_close_ev(psock);
    }
    else
    {
        struct rw_ol *prwol = (struct rw_ol *)psock->data;
        _recv_iov_commit(psock->bufrecv, 0, prwol->recvol.wsabuf, prwol->recvol.iovcount); 
        (void)_post_disconnect(piocp, psock);
    }

    return ERR_OK;
}
static inline void _on_recv(struct iocp_ctx *piocp, struct sock_ol *psockol, int32_t ierr, DWORD dbytes)
{
    if (ERR_OK == _is_close(piocp, psockol, ierr, dbytes))
    {
        return;
    }

    struct sock_ctx *psock = psockol->sockctx;
    if (SOCK_DGRAM == psock->socktype)
    {
        struct recv_from_ol *precvfromol = UPCAST(psockol, struct recv_from_ol, ol);
        _recv_iov_commit(psock->bufrecv, (size_t)dbytes, precvfromol->wsabuf, precvfromol->iovcount);
        //投递EV_RECV消息
        struct ev_udp_recv_ctx *pudpev = ev_new_recv_from(psock, dbytes);
        char acport[PORT_LENS];
        int32_t irtn = getnameinfo((struct sockaddr *)&precvfromol->rmtaddr, precvfromol->addrlen,
            pudpev->host, sizeof(pudpev->host), acport, sizeof(acport),
            NI_NUMERICHOST | NI_NUMERICSERV);
        if (ERR_OK != irtn)
        {
            pudpev->port = 0;
            LOG_WARN("error code:%d message:%s", irtn, gai_strerror(irtn));
        }
        else
        {
            pudpev->port = atoi(acport);
        }
        if (ERR_OK != sockctx_post_ev(psock, &pudpev->ev))
        {
            LOG_ERROR("%s", "post recv event failed.");
            SAFE_FREE(pudpev);
        }
        //继续接收
        irtn = _post_recv_from(psock);
        if (ERR_OK != irtn)
        {
            LOG_ERROR("error code:%d message:%s", irtn, ERRORSTR(irtn));
            //投递EV_CLOSE
            _send_close_ev(psock);
        }
    }
    else
    {
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
            LOG_ERROR("error code:%d message:%s", irtn, ERRORSTR(irtn));
            (void)_post_disconnect(piocp, psock);
        }
    }
}
static inline void _on_send(struct sock_ol *psockol, int32_t ierr, DWORD dbytes)
{
    if (0 == dbytes
        || ERR_OK != ierr)
    {
        _send_iov_commit_failed(psockol->sockctx->bufsend);
        return;
    }

    struct sock_ctx *psock = psockol->sockctx;
    if (SOCK_DGRAM == psock->socktype)
    {
        struct send_to_ol *psendtool = UPCAST(psockol, struct send_to_ol, ol);
        if (0 != psock->postsendev)
        {
            //投递EV_SEND消息
            struct ev_udp_send_ctx *pev = ev_new_send_to(psock, (size_t)dbytes);
            netaddr_setaddr(&pev->addr, netaddr_addr(&psendtool->addr));
            pev->iovcount = psendtool->iovcount;
            pev->wsabuf = psendtool->wsabuf;
            if (ERR_OK != sockctx_post_ev(psock, &pev->ev))
            {
                LOG_ERROR("%s", "post send event failed.");
                SAFE_FREE(pev);
            }
        }
        SAFE_FREE(psendtool);
    }
    else
    {
        _send_iov_commit(psock->bufsend, (size_t)dbytes);
        int32_t irtn = _post_send(psock);
        if (0 != psock->postsendev
            && ERR_OK == irtn)
        {
            //投递EV_SEND消息
            struct ev_ctx *pev = ev_new_send(psock, (size_t)dbytes);
            if (ERR_OK != sockctx_post_ev(psock, pev))
            {
                LOG_ERROR("%s", "post send event failed.");
                SAFE_FREE(pev);
            }
        }
    }    
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
                _on_recv(piocp, psockol, ierr, dbytes);
                break;
            case EV_SEND:
                _on_send(psockol, ierr, dbytes);
                break;
            case EV_CLOSE:
                _on_close(psockol, ierr);
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
        thread_creat(&piocp->netio.thread[i], _icop_loop, piocp, NULL, NULL);
        thread_waitstart(&piocp->netio.thread[i]);
    }
}
//初始化一struct sock_ctx
static inline struct sock_ctx *_init_tcp_sock(struct chan_ctx *pchan,
    const char *phost, const uint16_t usport, const uint8_t uclistener)
{
    struct sock_ctx *psock = sockctx_new(uclistener, SOCK_STREAM);
    int32_t irtn = netaddr_sethost(&psock->addr, phost, usport);
    if (ERR_OK != irtn)
    {
        LOG_ERROR("error code:%d message:%s", irtn, gai_strerror(irtn));        
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
        irtn = ERRNO;
        LOG_ERROR("error code:%d message:%s", irtn, ERRORSTR(irtn));
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
        LOG_ERROR("error code:%d message:%s", irtn, ERRORSTR(irtn));
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

        irtn = _post_accept(piocp, psock, &poverlapped[i]);
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
        LOG_ERROR("error code:%d message:%s", irtn, gai_strerror(irtn));
        return irtn;
    }

    if (ERR_OK != bind(psock->sock, netaddr_addr(&addr), netaddr_size(&addr)))
    {
        irtn = ERRNO;
        LOG_ERROR("error code:%d message:%s", irtn, ERRORSTR(irtn));
        return irtn;
    }

    return ERR_OK;
}
static int32_t _connectex(struct iocp_ctx *piocp, struct sock_ctx *psock)
{
    ASSERTAB(NULL == psock->data, "date used by another.");

    int32_t irtn;
    if (NULL == CreateIoCompletionPort((HANDLE)psock->sock, 
        piocp->ioport,
        (ULONG_PTR)psock,
        piocp->netio.threadnum))
    {
        irtn = ERRNO;
        LOG_ERROR("error code:%d message:%s", irtn, ERRORSTR(irtn));
        return irtn;
    }
    
    return _post_connect(piocp, psock);
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

    socknbio(fd);
    struct sock_ctx *psock = sockctx_new(0, info.iSocketType);
    psock->sock = fd;

    struct iocp_ctx *piocp = UPCAST(pctx, struct iocp_ctx, netio);
    if (NULL == CreateIoCompletionPort((HANDLE)psock->sock,
        piocp->ioport,
        (ULONG_PTR)psock,
        piocp->netio.threadnum))
    {
        int32_t irtn = ERRNO;
        LOG_ERROR("error code:%d message:%s", irtn, ERRORSTR(irtn));
        sockctx_free(psock);
        return NULL;
    }

    return psock;
}
int32_t netio_enable_rw(struct sock_ctx *psock, struct chan_ctx *pchan, const uint8_t postsendev)
{
    ASSERTAB(0 == psock->listener, "can't enbale read write on listen socket.");
    psock->chan = pchan;
    psock->postsendev = postsendev;
    if (SOCK_STREAM == psock->socktype)
    {
        socknodelay(psock->sock);
        sockkpa(psock->sock, SOCKKPA_DELAY, SOCKKPA_INTVL);
    }

    int32_t irtn;
    ASSERTAB(NULL == psock->data, "data used by another.");
    if (SOCK_DGRAM == psock->socktype)
    {
        struct recv_from_ol *precvfromol = (struct recv_from_ol *)MALLOC(sizeof(struct recv_from_ol));
        ASSERTAB(NULL != precvfromol, ERRSTR_MEMORY);        
        psock->data = precvfromol;
        precvfromol->ol.evtype = EV_RECV;
        precvfromol->ol.sockctx = psock;

        irtn = _post_recv_from(psock);
        if (ERR_OK != irtn)
        {
            LOG_ERROR("error code:%d message:%s", irtn, ERRORSTR(irtn));
            SAFE_FREE(psock->data);
        }

        return irtn;
    }

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
        LOG_ERROR("error code:%d message:%s", irtn, ERRORSTR(irtn));
        SAFE_FREE(psock->data);
    }

    return irtn;
}
int32_t netio_send(struct sock_ctx *psock, void *pdata, size_t ilens)
{
    ASSERTAB(ERR_OK == buffer_append(psock->bufsend, pdata, ilens), "buffer_append failed.");
    return _post_send(psock);
}
int32_t netio_sendto(struct sock_ctx *psock, const char *phost, uint16_t usport,
    IOV_TYPE *wsabuf, size_t uicount)
{
    struct send_to_ol *poverlapped = (struct send_to_ol *)MALLOC(sizeof(struct send_to_ol));
    ASSERTAB(NULL != poverlapped, ERRSTR_MEMORY);
    ZERO(&poverlapped->ol.overlapped, sizeof(poverlapped->ol.overlapped));
    poverlapped->ol.bytes = 0;
    poverlapped->ol.evtype = EV_SEND;
    poverlapped->ol.sockctx = psock;
    poverlapped->iovcount = uicount;
    poverlapped->wsabuf = wsabuf;
    int32_t irtn = netaddr_sethost(&poverlapped->addr, phost, usport);
    if (ERR_OK != irtn)
    {
        LOG_ERROR("error code:%d message:%s", irtn, gai_strerror(irtn));
        SAFE_FREE(poverlapped);
        return irtn;
    }

    irtn = WSASendTo(psock->sock,
        poverlapped->wsabuf,
        (DWORD)poverlapped->iovcount,
        &poverlapped->ol.bytes,
        0,
        netaddr_addr(&poverlapped->addr),
        netaddr_size(&poverlapped->addr),
        &poverlapped->ol.overlapped,
        NULL);
    if (ERR_OK != irtn)
    {
        irtn = ERRNO;
        if (ERROR_IO_PENDING != irtn)
        {
            LOG_ERROR("error code:%d message:%s", irtn, ERRORSTR(irtn));
            return irtn;
        }
    }

    return irtn;
}

#endif // OS_WIN
