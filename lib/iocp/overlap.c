#include "overlap.h"
#include "buffer.h"
#include "netaddr.h"
#include "netutils.h"
#include "mutex.h"
#include "loger.h"

#if defined(OS_WIN)

#define MAX_ACCEPT_SOCKEX   128
#define MAX_RECV_IOV_SIZE   4096
#define MAX_RECV_IOV_COUNT  4
#define MAX_RECV_FROM_IOV_SIZE   4096
#define MAX_RECV_FROM_IOV_COUNT  4

typedef struct overlap_acpt_ctx
{
    struct overlap_ctx ol;
    struct listener_ctx *listener;
    DWORD bytes;
    char addrbuf[sizeof(struct sockaddr_storage)];
}overlap_acpt_ctx;
typedef struct listener_ctx
{
    uint8_t chancnt;
    uint8_t family;
    int32_t acpcnt;
    struct chan_ctx *chan;
    SOCKET  sock;    
    mutex_ctx mutex;
    struct overlap_acpt_ctx overlap_acpt[MAX_ACCEPT_SOCKEX];
}listener_ctx;
typedef struct overlap_conn_ctx
{
    struct overlap_ctx ol;    
    struct chan_ctx *chan;
    DWORD bytes;
}overlap_conn_ctx;
typedef struct overlap_recv_ctx
{
    struct overlap_ctx ol;
    struct buffer_ctx *buffer;
    struct chan_ctx *chan;
    uint32_t iovcnt;
    DWORD bytes;
    DWORD flag;
    IOV_TYPE wsabuf[MAX_RECV_IOV_COUNT];
}overlap_recv_ctx;
typedef struct overlap_recv_from_ctx
{
    struct overlap_ctx ol;
    struct chan_ctx *chan;
    struct buffer_ctx *buffer;
    int32_t addrlen;                   //存储数据来源IP地址长度
    uint32_t iovcnt;
    DWORD bytes;
    DWORD flag;
    IOV_TYPE wsabuf[MAX_RECV_FROM_IOV_COUNT];
    struct sockaddr_storage rmtaddr;    //存储数据来源IP地址    
}overlap_recv_from_ctx;

static inline int32_t _post_accept(struct netev_ctx *piocpctx, 
    struct overlap_acpt_ctx *pacpolctx)
{
    SOCKET sock = WSASocket(pacpolctx->listener->family,
        SOCK_STREAM,
        IPPROTO_TCP,
        NULL,
        0,
        WSA_FLAG_OVERLAPPED);
    if (INVALID_SOCK == sock)
    {
        return ERRNO;
    }

    ZERO(&pacpolctx->ol.overlapped, sizeof(pacpolctx->ol.overlapped));
    pacpolctx->bytes = 0;
    pacpolctx->ol.sock = sock;

    if (!piocpctx->acceptex(pacpolctx->listener->sock,//Listen Socket
        pacpolctx->ol.sock,                            //Accept Socket
        &pacpolctx->addrbuf,
        0,
        sizeof(pacpolctx->addrbuf) / 2,
        sizeof(pacpolctx->addrbuf) / 2,
        &pacpolctx->bytes,
        &pacpolctx->ol.overlapped))
    {
        int32_t irtn = ERRNO;
        if (ERROR_IO_PENDING != irtn)
        {
            SAFE_CLOSESOCK(pacpolctx->ol.sock);
            return irtn;
        }
    }

    return ERR_OK;
}
static inline void _check_free_listener(struct listener_ctx *plistener)
{
    int8_t ifree = 0;
    mutex_lock(&plistener->mutex);
    plistener->acpcnt--;
    if (0 == plistener->acpcnt)
    {
        ifree = 1;
    }
    mutex_unlock(&plistener->mutex);

    if (1 == ifree)
    {
        LOG_WARN("free listener %d.", (int32_t)plistener->sock);
        CLOSESOCKET(plistener->sock);
        mutex_free(&plistener->mutex);
        SAFE_FREE(plistener);
    }
}
static inline void _on_accept(struct netev_ctx *piocpctx, 
    struct overlap_ctx *polctx, const uint32_t uibyte, const int32_t ierr)
{
    struct overlap_acpt_ctx *pacceptol = UPCAST(polctx, struct overlap_acpt_ctx, ol);
    SOCKET sock = pacceptol->ol.sock;  
    int32_t irtn = _post_accept(piocpctx, pacceptol);
    if (ERR_OK != irtn)
    {
        LOG_ERROR("error code:%d message:%s", irtn, ERRORSTR(irtn));
        _check_free_listener(pacceptol->listener);
    }
    if (ERR_OK != ierr)
    {
        LOG_ERROR("error code:%d message:%s", ierr, ERRORSTR(ierr));
        CLOSESOCKET(sock);
        return;
    }

    //设置，让getsockname, getpeername, shutdown可用
    irtn = setsockopt(sock, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
        (char *)&pacceptol->listener->sock, sizeof(&pacceptol->listener->sock));
    if (irtn < ERR_OK)
    {
        irtn = ERRNO;
        LOG_ERROR("error code:%d message:%s", irtn, ERRORSTR(irtn));
        CLOSESOCKET(sock);
        return;
    }
    //加进IOCP
    if (NULL == CreateIoCompletionPort((HANDLE)sock,
        piocpctx->iocp,
        0,
        piocpctx->threadcnt))
    {
        irtn = ERRNO;
        LOG_ERROR("error code:%d message:%s", irtn, ERRORSTR(irtn));
        CLOSESOCKET(sock);
        return;
    }
    //用listen的chan投递EV_ACCEPT
    struct ev_sock_ctx *pev = ev_sock_accept(sock);
    if (ERR_OK != post_ev(pacceptol->listener->chan, pacceptol->listener->chancnt, &pev->ev))
    {
        LOG_ERROR("%s", "post accept event failed.");
        SAFE_FREE(pev);
        CLOSESOCKET(sock);
    }
}
static inline void _on_connect(struct netev_ctx *piocpctx, 
    struct overlap_ctx *polctx, const uint32_t uibyte, const int32_t ierr)
{
    struct overlap_conn_ctx *pconnol = UPCAST(polctx, struct overlap_conn_ctx, ol);
    SOCKET sock = pconnol->ol.sock;
    if (ERR_OK != ierr)
    {
        SAFE_CLOSESOCK(sock);
    }
    //投递EV_CONNECT
    struct ev_sock_ctx *pev = ev_sock_connect(pconnol->ol.sock, ierr);
    if (ERR_OK != post_ev(pconnol->chan, 1, &pev->ev))
    {
        LOG_ERROR("%s", "post accept event failed.");
        SAFE_FREE(pev);
        SAFE_CLOSESOCK(sock);
    }
    SAFE_FREE(pconnol);
}
//申请iov
static inline uint32_t _recv_iov_application(struct buffer_ctx *pbuf, const size_t uisize,
    IOV_TYPE *wsabuf, const uint32_t uiovlens)
{
    buffer_lock(pbuf);
    ASSERTAB(0 == pbuf->freeze_write, "buffer tail already freezed.");
    pbuf->freeze_write = 1;
    uint32_t uicoun = _buffer_expand_iov(pbuf, uisize, wsabuf, uiovlens);
    buffer_unlock(pbuf);
    return uicoun;
}
//提交iov
static inline void _recv_iov_commit(struct buffer_ctx *pbuf, size_t ilens,
    IOV_TYPE *piov, const uint32_t uicnt)
{
    buffer_lock(pbuf);
    ASSERTAB(0 != pbuf->freeze_write, "buffer tail already unfreezed.");
    size_t uisize = _buffer_commit_iov(pbuf, ilens, piov, uicnt);
    ASSERTAB(uisize == ilens, "_buffer_commit_iov lens error.");
    pbuf->freeze_write = 0;
    buffer_unlock(pbuf);
}
static inline int32_t _post_recv(struct overlap_recv_ctx *poverlap)
{
    ZERO(&poverlap->ol.overlapped, sizeof(poverlap->ol.overlapped));
    poverlap->flag = 0;
    poverlap->bytes = 0;
    poverlap->iovcnt = _recv_iov_application(poverlap->buffer,
        MAX_RECV_IOV_SIZE, poverlap->wsabuf, MAX_RECV_IOV_COUNT);

    int32_t irtn = WSARecv(poverlap->ol.sock,
        poverlap->wsabuf,
        (DWORD)poverlap->iovcnt,
        &poverlap->bytes,
        &poverlap->flag,
        &poverlap->ol.overlapped,
        NULL);
    if (ERR_OK != irtn)
    {
        irtn = ERRNO;
        if (WSA_IO_PENDING != irtn)
        {
            _recv_iov_commit(poverlap->buffer, 0, poverlap->wsabuf, poverlap->iovcnt);
            return irtn;
        }
    }

    return ERR_OK;
}
static inline void _tcp_close(struct overlap_recv_ctx *precvol)
{
    struct ev_sock_ctx *pev = ev_sock_close(precvol->ol.sock, precvol->buffer, SOCK_STREAM);
    if (ERR_OK != post_ev(precvol->chan, 1, &pev->ev))
    {
        LOG_ERROR("%s", "post close event failed.");
        SAFE_FREE(pev);
    }
    CLOSESOCKET(precvol->ol.sock);
    SAFE_FREE(precvol);
}
static inline void _on_recv(struct netev_ctx *piocpctx, 
    struct overlap_ctx *polctx, const uint32_t uibyte, const int32_t ierr)
{
    struct overlap_recv_ctx *precvol = UPCAST(polctx, struct overlap_recv_ctx, ol);
    if (0 == uibyte
        || ERR_OK != ierr)
    {
        _recv_iov_commit(precvol->buffer, 0, precvol->wsabuf, precvol->iovcnt);
        _tcp_close(precvol);
        return;
    }

    _recv_iov_commit(precvol->buffer, (size_t)uibyte, precvol->wsabuf, precvol->iovcnt);
    //投递EV_RECV消息
    struct ev_sock_ctx *pev = ev_sock_recv(precvol->ol.sock, 
        (int32_t)uibyte, precvol->buffer, SOCK_STREAM);
    if (ERR_OK != post_ev(precvol->chan, 1, &pev->ev))
    {
        LOG_ERROR("%s", "post recv event failed.");
        SAFE_FREE(pev);
    }
    //继续接收
    if (ERR_OK != _post_recv(precvol))
    {
        _tcp_close(precvol);
    }
}
static inline int32_t _post_recv_from(struct overlap_recv_from_ctx *poverlap)
{
    ZERO(&poverlap->ol.overlapped, sizeof(poverlap->ol.overlapped));
    poverlap->flag = 0;
    poverlap->bytes = 0;
    poverlap->addrlen = sizeof(poverlap->rmtaddr);
    poverlap->iovcnt = _recv_iov_application(poverlap->buffer,
        MAX_RECV_FROM_IOV_SIZE, poverlap->wsabuf, MAX_RECV_FROM_IOV_COUNT);

    int32_t irtn = WSARecvFrom(poverlap->ol.sock,
        poverlap->wsabuf,
        (DWORD)poverlap->iovcnt,
        &poverlap->bytes,
        &poverlap->flag,
        (struct sockaddr*)&(poverlap->rmtaddr),
        &(poverlap->addrlen),
        &(poverlap->ol.overlapped),
        NULL);
    if (ERR_OK != irtn)
    {
        irtn = ERRNO;
        if (WSA_IO_PENDING != irtn)
        {
            _recv_iov_commit(poverlap->buffer, 0, poverlap->wsabuf, poverlap->iovcnt);
            return irtn;
        }
    }

    return ERR_OK;
}
static inline void _udp_close(struct overlap_recv_from_ctx *precvfol)
{
    struct ev_sock_ctx *pev = ev_sock_close(precvfol->ol.sock, precvfol->buffer, SOCK_DGRAM);
    if (ERR_OK != post_ev(precvfol->chan, 1, &pev->ev))
    {
        LOG_ERROR("%s", "post close event failed.");
        SAFE_FREE(pev);
    }
    CLOSESOCKET(precvfol->ol.sock);
    SAFE_FREE(precvfol);
}
static inline void _on_recv_from(struct netev_ctx *piocpctx, 
    struct overlap_ctx *polctx, const uint32_t uibyte, const int32_t ierr)
{
    struct overlap_recv_from_ctx *precvfol = UPCAST(polctx, struct overlap_recv_from_ctx, ol);
    if (0 == uibyte
        || ERR_OK != ierr)
    {
        _recv_iov_commit(precvfol->buffer, 0, precvfol->wsabuf, precvfol->iovcnt);
        _udp_close(precvfol);
        return;
    }
    
    _recv_iov_commit(precvfol->buffer, (size_t)uibyte, precvfol->wsabuf, precvfol->iovcnt);
    //投递EV_RECV消息
    struct ev_sock_ctx *pev = ev_sock_recv(precvfol->ol.sock,
        (int32_t)uibyte, precvfol->buffer, SOCK_DGRAM);
    char acport[PORT_LENS];
    int32_t irtn = getnameinfo((struct sockaddr *)&precvfol->rmtaddr, precvfol->addrlen,
        pev->ip, sizeof(pev->ip), acport, sizeof(acport),
        NI_NUMERICHOST | NI_NUMERICSERV);
    if (ERR_OK != irtn)
    {
        pev->port = 0;
        LOG_ERROR("error code:%d message:%s", irtn, gai_strerror(irtn));
    }
    else
    {
        pev->port = atoi(acport);
    }
    if (ERR_OK != post_ev(precvfol->chan, 1, &pev->ev))
    {
        LOG_ERROR("%s", "post recv event failed.");
        SAFE_FREE(pev);
    }
    //继续接收
    if (ERR_OK != _post_recv_from(precvfol))
    {
        _udp_close(precvfol);
    }
}
static int32_t _accptex(struct netev_ctx *piocpctx, 
    SOCKET sock, const uint8_t ufamily, 
    struct chan_ctx *pchan, const uint8_t uchancnt)
{
    int32_t irtn;
    if (NULL == CreateIoCompletionPort((HANDLE)sock,
        piocpctx->iocp,
        0,
        piocpctx->threadcnt))
    {
        irtn = ERRNO;
        LOG_ERROR("error code:%d message:%s", irtn, ERRORSTR(irtn));
        return irtn;
    }
    struct listener_ctx *plistener = 
        (struct listener_ctx *)MALLOC(sizeof(struct listener_ctx));
    if (NULL == plistener)
    {
        LOG_ERROR("%s", ERRSTR_MEMORY);
        return ERR_FAILED;
    }

    plistener->sock = sock;
    plistener->chan = pchan;
    plistener->chancnt = uchancnt;
    plistener->family = ufamily;
    plistener->acpcnt = MAX_ACCEPT_SOCKEX;
    mutex_init(&plistener->mutex);

    struct overlap_acpt_ctx *pacpolctx;
    for (int32_t i = 0; i < MAX_ACCEPT_SOCKEX; i++)
    {
        pacpolctx = &plistener->overlap_acpt[i];        
        pacpolctx->listener = plistener;
        pacpolctx->ol.overlap_cb = _on_accept;
        irtn = _post_accept(piocpctx, pacpolctx);
        if (ERR_OK != irtn)
        {
            LOG_ERROR("error code:%d message:%s", irtn, ERRORSTR(irtn));
            for (int32_t j = 0; j < i; j++)
            {
                SAFE_CLOSESOCK(plistener->overlap_acpt[i].ol.sock);
            }
            mutex_free(&plistener->mutex);
            SAFE_FREE(plistener);
            return irtn;
        }
    }

    return ERR_OK;
}
SOCKET netev_listener(struct netev_ctx *piocpctx, struct chan_ctx *pchan,
    const uint8_t uchancnt, const char *phost, const uint16_t usport)
{
    struct netaddr_ctx addr;
    int32_t irtn = netaddr_sethost(&addr, phost, usport);
    if (ERR_OK != irtn)
    {
        LOG_ERROR("error code:%d message:%s", irtn, gai_strerror(irtn));
        return INVALID_SOCK;
    }
    uint8_t ufamily = netaddr_family(&addr);

    SOCKET sock = WSASocket(ufamily,
        SOCK_STREAM,
        IPPROTO_TCP,
        NULL,
        0,
        WSA_FLAG_OVERLAPPED);
    if (INVALID_SOCK == sock)
    {
        irtn = ERRNO;
        LOG_ERROR("error code:%d message:%s", irtn, ERRORSTR(irtn));
        return INVALID_SOCK;
    }
    sockraddr(sock);
    if (ERR_OK != bind(sock, netaddr_addr(&addr), netaddr_size(&addr)))
    {
        irtn = ERRNO;
        LOG_ERROR("error code:%d message:%s", irtn, ERRORSTR(irtn));
        CLOSESOCKET(sock);
        return INVALID_SOCK;
    }
    if (ERR_OK != listen(sock, SOCKK_BACKLOG))
    {
        irtn = ERRNO;
        LOG_ERROR("error code:%d message:%s", irtn, ERRORSTR(irtn));
        CLOSESOCKET(sock);
        return INVALID_SOCK;
    }
    if (ERR_OK != _accptex(piocpctx, sock, ufamily, pchan, uchancnt))
    {
        CLOSESOCKET(sock);
        return INVALID_SOCK;
    }

    return sock;
}
static inline int32_t _trybind(SOCKET sock, const uint8_t ufamily)
{
    int32_t irtn;
    struct netaddr_ctx addr;
    if (AF_INET == ufamily)
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
    if (ERR_OK != bind(sock, netaddr_addr(&addr), netaddr_size(&addr)))
    {
        irtn = ERRNO;
        LOG_ERROR("error code:%d message:%s", irtn, ERRORSTR(irtn));
        return irtn;
    }

    return ERR_OK;
}
static inline int32_t _connectex(struct netev_ctx *piocpctx, 
    struct netaddr_ctx *paddr, SOCKET sock, struct chan_ctx *pchan)
{
    int32_t irtn;
    if (NULL == CreateIoCompletionPort((HANDLE)sock,
        piocpctx->iocp,
        0,
        piocpctx->threadcnt))
    {
        irtn = ERRNO;
        LOG_ERROR("error code:%d message:%s", irtn, ERRORSTR(irtn));
        return irtn;
    }

    struct overlap_conn_ctx *poverlapped = 
        (struct overlap_conn_ctx *)MALLOC(sizeof(struct overlap_conn_ctx));
    if (NULL == poverlapped)
    {
        LOG_ERROR("%s", ERRSTR_MEMORY);
        return ERR_FAILED;
    }
    ZERO(&poverlapped->ol.overlapped, sizeof(poverlapped->ol.overlapped));
    poverlapped->ol.overlap_cb = _on_connect;
    poverlapped->ol.sock = sock;
    poverlapped->bytes = 0;
    poverlapped->chan = pchan;

    if (!piocpctx->connectex(poverlapped->ol.sock,
        netaddr_addr(paddr),
        netaddr_size(paddr),
        NULL,
        0,
        &poverlapped->bytes,
        &poverlapped->ol.overlapped))
    {
        irtn = ERRNO;
        if (ERROR_IO_PENDING != irtn)
        {
            LOG_ERROR("error code:%d message:%s", irtn, ERRORSTR(irtn));
            SAFE_FREE(poverlapped);
            return irtn;
        }
    }

    return ERR_OK;
}
SOCKET netev_connecter(struct netev_ctx *piocpctx, struct chan_ctx *pchan,
    const char *phost, const uint16_t usport)
{
    struct netaddr_ctx addr;
    int32_t irtn = netaddr_sethost(&addr, phost, usport);
    if (ERR_OK != irtn)
    {
        LOG_ERROR("error code:%d message:%s", irtn, gai_strerror(irtn));
        return INVALID_SOCK;
    }
    uint8_t ufamily = netaddr_family(&addr);

    SOCKET sock = WSASocket(ufamily,
        SOCK_STREAM,
        IPPROTO_TCP,
        NULL,
        0,
        WSA_FLAG_OVERLAPPED);
    if (INVALID_SOCK == sock)
    {
        irtn = ERRNO;
        LOG_ERROR("error code:%d message:%s", irtn, ERRORSTR(irtn));
        return INVALID_SOCK;
    }
    sockraddr(sock);
    if (ERR_OK != _trybind(sock, ufamily))
    {
        CLOSESOCKET(sock);
        return INVALID_SOCK;
    }
    if (ERR_OK != _connectex(piocpctx, &addr, sock, pchan))
    {
        CLOSESOCKET(sock);
        return INVALID_SOCK;
    }

    return sock;
}
int32_t netev_addsock(struct netev_ctx *piocpctx, SOCKET fd)
{
    socknbio(fd);
    if (NULL == CreateIoCompletionPort((HANDLE)fd,
        piocpctx->iocp,
        0,
        piocpctx->threadcnt))
    {
        int32_t irtn = ERRNO;
        LOG_ERROR("error code:%d message:%s", irtn, ERRORSTR(irtn));
        CLOSESOCKET(fd);
        return irtn;
    }

    return ERR_OK;
}
static inline int32_t _udp_enable_rw(SOCKET fd, struct chan_ctx *pchan, 
    struct buffer_ctx *precvbuf)
{
    struct overlap_recv_from_ctx *poverlap =
        (struct overlap_recv_from_ctx *)MALLOC(sizeof(struct overlap_recv_from_ctx));
    if (NULL == poverlap)
    {
        LOG_ERROR("%s", ERRSTR_MEMORY);
        return ERR_FAILED;
    }

    poverlap->buffer = precvbuf;
    poverlap->ol.sock = fd;
    poverlap->ol.overlap_cb = _on_recv_from;
    poverlap->chan = pchan;
    int32_t irtn = _post_recv_from(poverlap);
    if (ERR_OK != irtn)
    {
        LOG_ERROR("error code:%d message:%s", irtn, ERRORSTR(irtn));
        SAFE_FREE(poverlap);        
    }
    return irtn;
}
static inline int32_t _tcp_enable_rw(SOCKET fd, struct chan_ctx *pchan, 
    struct buffer_ctx *precvbuf)
{
    struct overlap_recv_ctx *poverlap =
        (struct overlap_recv_ctx *)MALLOC(sizeof(struct overlap_recv_ctx));
    if (NULL == poverlap)
    {
        LOG_ERROR("%s", ERRSTR_MEMORY);
        return ERR_FAILED;
    }

    socknodelay(fd);
    closereset(fd);
    sockkpa(fd, SOCKKPA_DELAY, SOCKKPA_INTVL);

    poverlap->buffer = precvbuf;
    poverlap->ol.sock = fd;
    poverlap->ol.overlap_cb = _on_recv;
    poverlap->chan = pchan;
    int32_t irtn = _post_recv(poverlap);
    if (ERR_OK != irtn)
    {
        LOG_ERROR("error code:%d message:%s", irtn, ERRORSTR(irtn));
        SAFE_FREE(poverlap);    
    }
    return irtn;
}
int32_t netev_enable_rw(SOCKET fd, struct chan_ctx *pchan, 
    struct buffer_ctx *precvbuf)
{
    int32_t isocktyep = socktype(fd);
    if (ERR_FAILED == isocktyep)
    {
        LOG_ERROR("%s", "get sock type error.");
        CLOSESOCKET(fd);
        return ERR_FAILED;
    }

    int32_t irtn;
    if (SOCK_DGRAM == isocktyep)
    {
        irtn = _udp_enable_rw(fd, pchan, precvbuf);
        if (ERR_OK != irtn)
        {
            CLOSESOCKET(fd);
        }
        return irtn;
    }
    else
    {
        irtn = _tcp_enable_rw(fd, pchan, precvbuf);
        if (ERR_OK != irtn)
        {
            CLOSESOCKET(fd);
        }
        return irtn;
    }
}

#endif
