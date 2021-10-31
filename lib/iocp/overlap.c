#include "overlap.h"
#include "buffer.h"
#include "netaddr.h"
#include "netutils.h"
#include "loger.h"
#include "mutex.h"

#if defined(OS_WIN)

#define MAX_ACCEPT_SOCKEX   SOCKK_BACKLOG
#define MAX_RECV_IOV_SIZE   4096
#define MAX_RECV_IOV_COUNT  4
#define MAX_RECV_FROM_IOV_SIZE   4096
#define MAX_RECV_FROM_IOV_COUNT  4
#define MAX_SEND_IOV_SIZE   4096
#define MAX_SEND_IOV_COUNT  16
#define MAX_SENDTO_IOV_COUNT  16

typedef struct overlap_acpt_ctx
{
    struct overlap_ctx ol;//overlapped
    struct listener_ctx *listener;
    DWORD bytes;
    char addrbuf[sizeof(struct sockaddr_storage)];
}overlap_acpt_ctx;
typedef struct listener_ctx
{
    uint16_t chancnt;
    uint16_t family;
    int32_t acpcnt;
    struct chan_ctx *chan;//EV_ACCEPT
    SOCKET  sock;
    mutex_ctx mutex;//锁acpcnt
    struct overlap_acpt_ctx overlap_acpt[MAX_ACCEPT_SOCKEX];
}listener_ctx;
typedef struct overlap_conn_ctx
{
    struct overlap_ctx ol;//overlapped
    struct chan_ctx *chan;//EV_CONNECT
    DWORD bytes;
}overlap_conn_ctx;
typedef struct overlap_recv_ctx
{
    struct overlap_ctx ol;//overlapped
    struct sock_ctx *sockctx;
    uint32_t iovcnt;
    DWORD bytes;
    DWORD flag;
    IOV_TYPE wsabuf[MAX_RECV_IOV_COUNT];
}overlap_recv_ctx;
typedef struct overlap_recv_from_ctx
{
    struct overlap_ctx ol;//overlapped
    struct sock_ctx *sockctx;
    int32_t addrlen;
    uint32_t iovcnt;
    DWORD bytes;
    DWORD flag;
    IOV_TYPE wsabuf[MAX_RECV_FROM_IOV_COUNT];
    struct sockaddr_storage rmtaddr;//存储数据来源IP地址    
}overlap_recv_from_ctx;
typedef struct overlap_send_ctx
{
    struct overlap_ctx ol;//overlapped
    struct sock_ctx *sockctx;
    uint32_t iovcnt;
    DWORD bytes;
    IOV_TYPE wsabuf[MAX_SEND_IOV_COUNT];
}overlap_send_ctx;
typedef struct udp_send_node_ctx
{
    struct udp_send_node_ctx *next;
    size_t size;
    struct netaddr_ctx addr;
}udp_send_node_ctx;
typedef struct overlap_sendto_ctx
{
    struct overlap_ctx ol;//overlapped
    struct udp_send_node_ctx *head;
    struct udp_send_node_ctx *tail;
    struct sock_ctx *sockctx;
    uint32_t iovcnt;
    DWORD bytes;
    IOV_TYPE wsabuf[MAX_SENDTO_IOV_COUNT];
}overlap_sendto_ctx;
typedef struct sock_ctx
{
    uint16_t postsendev;
    uint16_t socktype;
    volatile atomic_t sending;
    struct buffer_ctx *recvbuf;
    struct buffer_ctx *sendbuf;
    struct chan_ctx *chan;//EV_CLOSE  EV_RECV  EV_SEND
    void *overlap;
}sock_ctx;

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
    int32_t ifree = 0;
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

    irtn = setsockopt(sock, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
        (char *)&pacceptol->listener->sock, sizeof(&pacceptol->listener->sock));
    if (irtn < ERR_OK)
    {
        irtn = ERRNO;
        LOG_ERROR("error code:%d message:%s", irtn, ERRORSTR(irtn));
        CLOSESOCKET(sock);
        return;
    }
    if (NULL == CreateIoCompletionPort((HANDLE)sock,
        piocpctx->iocp,
        0,
        piocpctx->thcnt))
    {
        irtn = ERRNO;
        LOG_ERROR("error code:%d message:%s", irtn, ERRORSTR(irtn));
        CLOSESOCKET(sock);
        return;
    }
    //投递EV_ACCEPT
    struct ev_sock_ctx *pev = ev_sock_accept(sock);
    if (ERR_OK != post_ev(pacceptol->listener->chan, 
        pacceptol->listener->chancnt, &pev->ev))
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
    poverlap->iovcnt = _recv_iov_application(poverlap->sockctx->recvbuf,
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
            _recv_iov_commit(poverlap->sockctx->recvbuf, 0, 
                poverlap->wsabuf, poverlap->iovcnt);
            return irtn;
        }
    }

    return ERR_OK;
}
static inline void _tcp_close(struct overlap_recv_ctx *precvol)
{
    struct ev_sock_ctx *pev = ev_sock_close(precvol->ol.sock, 
        precvol->sockctx, precvol->sockctx->socktype);
    if (ERR_OK != post_ev(precvol->sockctx->chan, 1, &pev->ev))
    {
        LOG_ERROR("%s", "post close event failed.");
        SAFE_FREE(pev);
    }
    closereset(precvol->ol.sock);
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
        _recv_iov_commit(precvol->sockctx->recvbuf, 0, 
            precvol->wsabuf, precvol->iovcnt);
        _tcp_close(precvol);
        return;
    }

    _recv_iov_commit(precvol->sockctx->recvbuf, (size_t)uibyte,
        precvol->wsabuf, precvol->iovcnt);
    //投递EV_RECV消息
    struct ev_sock_ctx *pev = ev_sock_recv(precvol->ol.sock, (int32_t)uibyte, 
        precvol->sockctx, precvol->sockctx->socktype);
    if (ERR_OK != post_ev(precvol->sockctx->chan, 1, &pev->ev))
    {
        LOG_ERROR("%s", "post recv event failed.");
        SAFE_FREE(pev);
    }
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
    poverlap->iovcnt = _recv_iov_application(poverlap->sockctx->recvbuf,
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
            _recv_iov_commit(poverlap->sockctx->recvbuf, 0, 
                poverlap->wsabuf, poverlap->iovcnt);
            return irtn;
        }
    }

    return ERR_OK;
}
static inline void _udp_close(struct overlap_recv_from_ctx *precvfol)
{
    struct ev_sock_ctx *pev = ev_sock_close(precvfol->ol.sock,
        precvfol->sockctx, precvfol->sockctx->socktype);
    if (ERR_OK != post_ev(precvfol->sockctx->chan, 1, &pev->ev))
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
        _recv_iov_commit(precvfol->sockctx->recvbuf, 0, 
            precvfol->wsabuf, precvfol->iovcnt);
        _udp_close(precvfol);
        return;
    }
    
    _recv_iov_commit(precvfol->sockctx->recvbuf, (size_t)uibyte, 
        precvfol->wsabuf, precvfol->iovcnt);
    //投递EV_RECV消息
    struct ev_sock_ctx *pev = ev_sock_recv(precvfol->ol.sock, (int32_t)uibyte,
        precvfol->sockctx, precvfol->sockctx->socktype);
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
    if (ERR_OK != post_ev(precvfol->sockctx->chan, 1, &pev->ev))
    {
        LOG_ERROR("%s", "post recv event failed.");
        SAFE_FREE(pev);
    }
    irtn = _post_recv_from(precvfol);
    if (ERR_OK != irtn)
    {
        LOG_WARN("error code:%d message:%s", irtn, ERRORSTR(irtn));
        _udp_close(precvfol);
    }
}
static inline uint32_t _tcp_send_iov_application(struct overlap_send_ctx *poverlap)
{
    struct buffer_ctx *pbuf = poverlap->sockctx->sendbuf;
    buffer_lock(pbuf);
    ASSERTAB(0 == pbuf->freeze_read, "buffer head already freezed");
    uint32_t uicnt = _buffer_get_iov(pbuf, MAX_SEND_IOV_SIZE, 
        poverlap->wsabuf, MAX_SEND_IOV_COUNT);
    if (uicnt > 0)
    {
        pbuf->freeze_read = 1;
    }
    buffer_unlock(pbuf);
    return uicnt;
}
static inline void _tcp_send_iov_commit(struct overlap_send_ctx *poverlap, size_t uilens)
{
    struct buffer_ctx *pbuf = poverlap->sockctx->sendbuf;
    buffer_lock(pbuf);
    ASSERTAB(1 == pbuf->freeze_read, "buffer head already unfreezed.");
    if (uilens > 0)
    {
        ASSERTAB(uilens == _buffer_drain(pbuf, uilens), "drain size not equ commit size.");
    }
    pbuf->freeze_read = 0;
    buffer_unlock(pbuf);
}
static inline int32_t _post_send(struct overlap_send_ctx *poverlap)
{
    uint32_t uicnt = _tcp_send_iov_application(poverlap);
    if (0 == uicnt)
    {
        return ERR_FAILED;
    }

    ZERO(&poverlap->ol.overlapped, sizeof(poverlap->ol.overlapped));
    poverlap->iovcnt = uicnt;
    int32_t irtn = WSASend(poverlap->ol.sock,
        poverlap->wsabuf,
        (DWORD)uicnt,
        &poverlap->bytes,
        0,
        &poverlap->ol.overlapped,
        NULL);
    if (ERR_OK != irtn)
    {
        irtn = ERRNO;
        if (WSA_IO_PENDING != irtn)
        {
            _tcp_send_iov_commit(poverlap, 0);
            return irtn;
        }
    }
    return ERR_OK;
}
static inline void _on_send(struct netev_ctx *piocpctx,
    struct overlap_ctx *polctx, const uint32_t uibyte, const int32_t ierr)
{
    struct overlap_send_ctx *psendol = UPCAST(polctx, struct overlap_send_ctx, ol);
    if (0 == uibyte
        || ERR_OK != ierr)
    {
        _tcp_send_iov_commit(psendol, 0);
        ATOMIC_SET(&psendol->sockctx->sending, 0);
        return;
    }

    _tcp_send_iov_commit(psendol, uibyte);
    if (0 != psendol->sockctx->postsendev)
    {
        //投递EV_SEND消息
        struct ev_sock_ctx *pev = ev_sock_send(psendol->ol.sock, (int32_t)uibyte,
            psendol->sockctx, psendol->sockctx->socktype);  
        if (ERR_OK != post_ev(psendol->sockctx->chan, 1, &pev->ev))
        {
            LOG_ERROR("%s", "post send event failed.");
            SAFE_FREE(pev);
        }
    }
    if (ERR_OK != _post_send(psendol))
    {
        ATOMIC_SET(&psendol->sockctx->sending, 0);
    }
}
static inline uint32_t _udp_sendto_iov_application(struct overlap_sendto_ctx *poverlap, struct netaddr_ctx **paddr)
{
    uint32_t uicnt = 0;
    struct buffer_ctx *pbuffer = poverlap->sockctx->sendbuf;
    buffer_lock(pbuffer);
    ASSERTAB(0 == pbuffer->freeze_read, "buffer head already freezed");
    if (NULL != poverlap->head)
    {
        *paddr = &poverlap->head->addr;
        uicnt = _buffer_get_iov(pbuffer, poverlap->head->size, poverlap->wsabuf, MAX_SENDTO_IOV_COUNT);
        if (uicnt > 0)
        {
            pbuffer->freeze_read = 1;
        }
    }
    buffer_unlock(pbuffer);
    return uicnt;
}
static inline void _udp_sendto_iov_commit(struct overlap_sendto_ctx *poverlap, size_t uilens)
{
    struct buffer_ctx *pbuffer = poverlap->sockctx->sendbuf;
    buffer_lock(pbuffer);
    ASSERTAB(1 == pbuffer->freeze_read, "buffer head already unfreezed.");
    if (uilens > 0)
    {
        struct udp_send_node_ctx *pnode = poverlap->head;
        ASSERTAB(NULL != pnode && uilens == pnode->size, "buffer head is NULL or commit size not equ node size.");
        ASSERTAB(uilens == _buffer_drain(pbuffer, uilens), "drain size not equ commit size.");
        if (NULL == pnode->next)
        {
            poverlap->head = poverlap->tail = NULL;
        }
        else
        {
            poverlap->head = pnode->next;
        }
        SAFE_FREE(pnode);
    }
    pbuffer->freeze_read = 0;
    buffer_unlock(pbuffer);
}
static inline int32_t _post_sendto(struct overlap_sendto_ctx *poverlap)
{
    struct netaddr_ctx *paddr = NULL;
    uint32_t uicnt = _udp_sendto_iov_application(poverlap, &paddr);
    if (0 == uicnt)
    {
        return ERR_FAILED;
    }

    ZERO(&poverlap->ol.overlapped, sizeof(poverlap->ol.overlapped));
    poverlap->bytes = 0;
    poverlap->iovcnt = uicnt;

    int32_t irtn = WSASendTo(poverlap->ol.sock,
        poverlap->wsabuf,
        (DWORD)poverlap->iovcnt,
        &poverlap->bytes,
        0,
        netaddr_addr(paddr),
        netaddr_size(paddr),
        &poverlap->ol.overlapped,
        NULL);
    if (ERR_OK != irtn)
    {
        irtn = ERRNO;
        if (ERROR_IO_PENDING != irtn)
        {
            _udp_sendto_iov_commit(poverlap, 0);
            return irtn;
        }
    }

    return ERR_OK;
}
static inline void _on_sendto(struct netev_ctx *piocpctx,
    struct overlap_ctx *polctx, const uint32_t uibyte, const int32_t ierr)
{
    struct overlap_sendto_ctx *psendtool = UPCAST(polctx, struct overlap_sendto_ctx, ol);
    if (0 == uibyte
        || ERR_OK != ierr)
    {
        _udp_sendto_iov_commit(psendtool, 0);
        ATOMIC_SET(&psendtool->sockctx->sending, 0);
        return;
    }
   
    _udp_sendto_iov_commit(psendtool, uibyte);
    if (0 != psendtool->sockctx->postsendev)
    {
        //投递EV_SEND消息
        struct ev_sock_ctx *pev = ev_sock_send(psendtool->ol.sock, (int32_t)uibyte,
            psendtool->sockctx, psendtool->sockctx->socktype);
        if (ERR_OK != post_ev(psendtool->sockctx->chan, 1, &pev->ev))
        {
            LOG_ERROR("%s", "post send event failed.");
            SAFE_FREE(pev);
        }
    }
    if (ERR_OK != _post_sendto(psendtool))
    {
        ATOMIC_SET(&psendtool->sockctx->sending, 0);
    }
}
static int32_t _accptex(struct netev_ctx *piocpctx, 
    SOCKET sock, const uint16_t ufamily,
    struct chan_ctx *pchan, const uint16_t uchancnt)
{
    int32_t irtn;
    if (NULL == CreateIoCompletionPort((HANDLE)sock,
        piocpctx->iocp,
        0,
        piocpctx->thcnt))
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
    const uint16_t uchancnt, const char *phost, const uint16_t usport)
{
    struct netaddr_ctx addr;
    int32_t irtn = netaddr_sethost(&addr, phost, usport);
    if (ERR_OK != irtn)
    {
        LOG_ERROR("error code:%d message:%s", irtn, gai_strerror(irtn));
        return INVALID_SOCK;
    }
    uint16_t ufamily = netaddr_family(&addr);

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
static inline int32_t _trybind(SOCKET sock, const uint16_t ufamily)
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
        piocpctx->thcnt))
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
    uint16_t ufamily = netaddr_family(&addr);

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
    struct netaddr_ctx addr;
    int32_t irtn = netaddr_localaddr(&addr, fd);
    if (ERR_OK != irtn)
    {
        irtn = _trybind(fd, netaddr_family(&addr));
        if (ERR_OK != irtn)
        {
            CLOSESOCKET(fd);
            return irtn;
        }
    }

    socknbio(fd);
    if (NULL == CreateIoCompletionPort((HANDLE)fd,
        piocpctx->iocp,
        0,
        piocpctx->thcnt))
    {
        irtn = ERRNO;
        LOG_ERROR("error code:%d message:%s", irtn, ERRORSTR(irtn));
        CLOSESOCKET(fd);
        return irtn;
    }

    return ERR_OK;
}
int32_t _sock_can_free(struct sock_ctx *psockctx)
{
    return 0 == ATOMIC_GET(&psockctx->sending) ? ERR_OK : ERR_FAILED;
}
struct buffer_ctx *sock_recvbuf(struct sock_ctx *psockctx)
{
    return psockctx->recvbuf;
}
static inline struct sock_ctx *_sockctx_new()
{
    struct sock_ctx *psockctx = (struct sock_ctx *)MALLOC(sizeof(struct sock_ctx));
    if (NULL == psockctx)
    {
        LOG_ERROR("%s", ERRSTR_MEMORY);
        return NULL;
    }
    psockctx->recvbuf = (struct buffer_ctx *)MALLOC(sizeof(struct buffer_ctx));
    if (NULL == psockctx->recvbuf)
    {
        LOG_ERROR("%s", ERRSTR_MEMORY);
        SAFE_FREE(psockctx);
        return NULL;
    }
    psockctx->sendbuf = (struct buffer_ctx *)MALLOC(sizeof(struct buffer_ctx));
    if (NULL == psockctx->sendbuf)
    {
        LOG_ERROR("%s", ERRSTR_MEMORY);
        SAFE_FREE(psockctx->recvbuf);
        SAFE_FREE(psockctx);
        return NULL;
    }

    return psockctx;
}
static inline void _sockctx_freeptr(struct sock_ctx *psockctx)
{
    SAFE_FREE(psockctx->sendbuf);
    SAFE_FREE(psockctx->recvbuf);
    SAFE_FREE(psockctx);
}
static inline struct sock_ctx *_sockctx_init(SOCKET sock, int32_t isocktyep)
{
    struct sock_ctx *psockctx = _sockctx_new();
    if (NULL == psockctx)
    {
        return NULL;
    }
    if (SOCK_DGRAM == isocktyep)
    {
        struct overlap_sendto_ctx *psendtool = 
            (struct overlap_sendto_ctx *)MALLOC(sizeof(struct overlap_sendto_ctx));
        if (NULL == psendtool)
        {
            _sockctx_freeptr(psockctx);
            return NULL;
        }
        psendtool->head = NULL;
        psendtool->tail = NULL;
        psendtool->ol.sock = sock;
        psendtool->ol.overlap_cb = _on_sendto;
        psendtool->sockctx = psockctx;
        psockctx->overlap = psendtool;
    }
    else
    {
        struct overlap_send_ctx *psendol = 
            (struct overlap_send_ctx *)MALLOC(sizeof(struct overlap_send_ctx));
        if (NULL == psendol)
        {
            _sockctx_freeptr(psockctx);
            return NULL;
        }
        psendol->ol.sock = sock;
        psendol->ol.overlap_cb = _on_send;
        psendol->sockctx = psockctx;
        psockctx->overlap = psendol;
    }

    psockctx->socktype = (uint16_t)isocktyep;
    psockctx->sending = 0;
    buffer_init(psockctx->recvbuf);
    buffer_init(psockctx->sendbuf);

    return psockctx;
}
static void _sockctx_udpnode_free(struct udp_send_node_ctx *pnode)
{
    struct udp_send_node_ctx *pnext;
    while (NULL != pnode)
    {
        pnext = pnode->next;
        SAFE_FREE(pnode);
        pnode = pnext;
    }
}
void _sock_free(struct sock_ctx *psockctx)
{
    if (SOCK_DGRAM == psockctx->socktype)
    {
        struct overlap_sendto_ctx *psendtool = (struct overlap_sendto_ctx *)psockctx->overlap;
        _sockctx_udpnode_free(psendtool->head);
    }

    SAFE_FREE(psockctx->overlap);
    buffer_free(psockctx->recvbuf);
    buffer_free(psockctx->sendbuf);
    _sockctx_freeptr(psockctx);
}
static inline int32_t _udp_enable_recv(SOCKET fd, struct sock_ctx *psockctx)
{
    struct overlap_recv_from_ctx *poverlap =
        (struct overlap_recv_from_ctx *)MALLOC(sizeof(struct overlap_recv_from_ctx));
    if (NULL == poverlap)
    {
        LOG_ERROR("%s", ERRSTR_MEMORY);
        return ERR_FAILED;
    }

    BOOL bbehavior = FALSE;
    DWORD drtn = 0;
    //10054 错误
    int32_t irtn = WSAIoctl(fd,
        SIO_UDP_CONNRESET,
        &bbehavior, sizeof(bbehavior),
        NULL, 0, &drtn, NULL, NULL);
    if (SOCKET_ERROR == irtn)
    {
        LOG_WARN("WSAIoctl(%d, SIO_UDP_CONNRESET...) failed. %s", (int32_t)fd, ERRORSTR(ERRNO));
    }

    poverlap->sockctx = psockctx;
    poverlap->ol.sock = fd;
    poverlap->ol.overlap_cb = _on_recv_from;
    irtn = _post_recv_from(poverlap);
    if (ERR_OK != irtn)
    {
        LOG_ERROR("error code:%d message:%s", irtn, ERRORSTR(irtn));
        SAFE_FREE(poverlap);
    }
    return irtn;
}
static inline int32_t _tcp_enable_recv(SOCKET fd, struct sock_ctx *psockctx)
{
    struct overlap_recv_ctx *poverlap =
        (struct overlap_recv_ctx *)MALLOC(sizeof(struct overlap_recv_ctx));
    if (NULL == poverlap)
    {
        LOG_ERROR("%s", ERRSTR_MEMORY);
        return ERR_FAILED;
    }

    socknodelay(fd);
    sockkpa(fd, SOCKKPA_DELAY, SOCKKPA_INTVL);

    poverlap->sockctx = psockctx;
    poverlap->ol.sock = fd;
    poverlap->ol.overlap_cb = _on_recv;
    int32_t irtn = _post_recv(poverlap);
    if (ERR_OK != irtn)
    {
        LOG_ERROR("error code:%d message:%s", irtn, ERRORSTR(irtn));
        SAFE_FREE(poverlap);
    }
    return irtn;
}
void sock_change_chan(struct sock_ctx *psockctx, struct chan_ctx *pchan)
{
    struct chan_ctx *plock = psockctx->chan;
    chan_lock(plock);
    psockctx->chan = pchan;
    chan_unlock(plock);
}
struct sock_ctx *netev_enable_rw(SOCKET fd, struct chan_ctx *pchan, const uint16_t upostsendev)
{
    int32_t isocktyep = socktype(fd);
    if (ERR_FAILED == isocktyep)
    {
        LOG_ERROR("%s", "get sock type error.");
        CLOSESOCKET(fd);
        return NULL;
    }

    struct sock_ctx *psender = _sockctx_init(fd, isocktyep);
    if (NULL == psender)
    {
        CLOSESOCKET(fd);
        return NULL;
    }
    psender->chan = pchan;
    psender->postsendev = upostsendev;

    int32_t irtn;
    if (SOCK_DGRAM == isocktyep)
    {
        irtn = _udp_enable_recv(fd, psender);
    }
    else
    {
        irtn = _tcp_enable_recv(fd, psender);
    }
    if (ERR_OK != irtn)
    {
        CLOSESOCKET(fd);
        _sock_free(psender);
        return NULL;
    }

    return psender;
}
int32_t tcp_send(struct sock_ctx *psockctx, void *pdata, const size_t uilens)
{
    ASSERTAB(SOCK_STREAM == psockctx->socktype, "only support tcp.");
    ASSERTAB(ERR_OK == buffer_append(psockctx->sendbuf, pdata, uilens), "buffer_append error.");
    return tcp_send_buf(psockctx);
}
int32_t tcp_send_buf(struct sock_ctx *psockctx)
{
    if (!ATOMIC_CAS(&psockctx->sending, 0, 1))
    {
        return ERR_OK;
    }

    int32_t irtn = _post_send((struct overlap_send_ctx *)psockctx->overlap);
    if (ERR_OK != irtn)
    {
        ATOMIC_SET(&psockctx->sending, 0);
        return irtn;
    }

    return ERR_OK;
}
static inline void _insert_udp_node(struct overlap_sendto_ctx *poverlap, struct udp_send_node_ctx *pnode,
    void *pdata, const size_t uilens)
{
    struct buffer_ctx *pbuffer = poverlap->sockctx->sendbuf;
    buffer_lock(pbuffer);
    if (NULL == poverlap->head)
    {
        poverlap->head = poverlap->tail = pnode;
    }
    else
    {
        poverlap->tail->next = pnode;
        poverlap->tail = pnode;
    }
    ASSERTAB(ERR_OK == _buffer_append(pbuffer, pdata, uilens), "buffer_append error.");
    buffer_unlock(pbuffer);
}
int32_t udp_send(struct sock_ctx *psockctx, void *pdata, const size_t uilens,
    const char *pip, const uint16_t uport)
{
    ASSERTAB(SOCK_DGRAM == psockctx->socktype, "only support udp.");  
    struct udp_send_node_ctx *pnode = MALLOC(sizeof(struct udp_send_node_ctx));
    if (NULL == pnode)
    {
        LOG_ERROR("%s", ERRSTR_MEMORY);
        return ERR_FAILED;
    }
    int32_t irtn = netaddr_sethost(&pnode->addr, pip, uport);
    if (ERR_OK != irtn)
    {
        LOG_ERROR("error code:%d message:%s", irtn, gai_strerror(irtn));
        SAFE_FREE(pnode);
        return irtn;
    }

    pnode->next = NULL;
    pnode->size = uilens;
    _insert_udp_node((struct overlap_sendto_ctx *)psockctx->overlap, pnode, pdata, uilens);
    if (!ATOMIC_CAS(&psockctx->sending, 0, 1))
    {
        return ERR_OK;
    }
    
    irtn = _post_sendto((struct overlap_sendto_ctx *)psockctx->overlap);
    if (ERR_OK != irtn)
    {
        ATOMIC_SET(&psockctx->sending, 0);
        return irtn;
    }

    return ERR_OK;
}

#endif
