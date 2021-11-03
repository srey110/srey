#include "overlap.h"
#include "netapi.h"
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
#define NET_ERROR(err, format, ...) \
            if (ERR_FAILED != err\
            && WSAECONNRESET != err \
            && WSAENOTSOCK != err \
            && WSAENOTCONN != err \
            && WSAESHUTDOWN != err\
            && ERROR_OPERATION_ABORTED != err\
            && WSAECONNABORTED != err)\
            LOG_ERROR(format, ##__VA_ARGS__)

typedef struct overlap_acpt_ctx
{
    struct overlap_ctx ol;//overlapped
    struct listener_ctx *listener;
    DWORD bytes;
    char addrbuf[sizeof(struct sockaddr_storage)];
}overlap_acpt_ctx;
typedef struct listener_ctx
{
    uint32_t chancnt;
    int32_t family;
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
typedef struct overlap_sendto_ctx
{
    struct overlap_ctx ol;//overlapped    
    struct sock_ctx *sockctx;
    uint32_t iovcnt;
    DWORD bytes;
    struct piece_iov_ctx piece;
    IOV_TYPE wsabuf[MAX_SENDTO_IOV_COUNT];
}overlap_sendto_ctx;
typedef struct sock_ctx
{
    int32_t freecnt;
    int32_t postsendev;
    int32_t socktype;
    atomic_t closed;
    atomic_t sending;
    struct buffer_ctx *recvbuf;
    struct buffer_ctx *sendbuf;
    struct chan_ctx *chan;//EV_CLOSE  EV_RECV  EV_SEND
    void *overlap;
    mutex_ctx lock_changech;//更改chan时
    mutex_ctx lock_postsc;//EV_CLOSE投递后不再投递EV_SEND
    BOOL(WINAPI *disconnectex)(SOCKET, LPOVERLAPPED, DWORD, DWORD);
}sock_ctx;
static inline int32_t _post_acpcon_ev(struct chan_ctx *pchan, const uint32_t uicnt, struct ev_ctx *pev)
{
    if (1 == uicnt)
    {
        return chan_send(pchan, pev);
    }
    return chan_send(&pchan[rand() % uicnt], pev);
};
static inline chan_ctx *_get_rsc_chan(struct sock_ctx *psockctx)
{
    struct chan_ctx *pchan;
    mutex_lock(&psockctx->lock_changech);
    pchan = psockctx->chan;
    mutex_unlock(&psockctx->lock_changech);
    return pchan;
}
void sock_change_chan(struct sock_ctx *psockctx, struct chan_ctx *pchan)
{
    mutex_lock(&psockctx->lock_changech);
    psockctx->chan = pchan;
    mutex_unlock(&psockctx->lock_changech);
}
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
            SAFE_CLOSE_SOCK(pacpolctx->ol.sock);
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
        SOCK_CLOSE(plistener->sock);
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
        NET_ERROR(irtn, "error code:%d message:%s", ierr, ERRORSTR(ierr));
        _check_free_listener(pacceptol->listener);
    }
    if (ERR_OK != ierr)
    {
        NET_ERROR(ierr, "error code:%d message:%s", ierr, ERRORSTR(ierr));
        SOCK_CLOSE(sock);
        return;
    }
    irtn = setsockopt(sock, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
        (char *)&pacceptol->listener->sock, sizeof(&pacceptol->listener->sock));
    if (irtn < ERR_OK)
    {
        irtn = ERRNO;
        NET_ERROR(irtn, "error code:%d message:%s", irtn, ERRORSTR(irtn));
        SOCK_CLOSE(sock);
        return;
    }
    //投递EV_ACCEPT
    struct ev_sock_ctx *pev = ev_sock_accept(sock);
    if (ERR_OK != _post_acpcon_ev(pacceptol->listener->chan,
        pacceptol->listener->chancnt, &pev->ev))
    {
        LOG_ERROR("%s", "post accept event failed.");
        SAFE_FREE(pev);
        SOCK_CLOSE(sock);
    }
}
static inline void _on_connect(struct netev_ctx *piocpctx, 
    struct overlap_ctx *polctx, const uint32_t uibyte, const int32_t ierr)
{
    struct overlap_conn_ctx *pconnol = UPCAST(polctx, struct overlap_conn_ctx, ol);
    SOCKET sock = pconnol->ol.sock;
    if (ERR_OK != ierr)
    {
        SAFE_CLOSE_SOCK(sock);
    }
    //投递EV_CONNECT
    struct ev_sock_ctx *pev = ev_sock_connect(pconnol->ol.sock, ierr);
    if (ERR_OK != _post_acpcon_ev(pconnol->chan, 1, &pev->ev))
    {
        LOG_ERROR("%s", "post accept event failed.");
        SAFE_FREE(pev);
        SAFE_CLOSE_SOCK(sock);
    }
    SAFE_FREE(pconnol);
}
static inline void _on_disconnectex(struct netev_ctx *piocpctx,
    struct overlap_ctx *polctx, const uint32_t uibyte, const int32_t ierr)
{
    SOCK_CLOSE(polctx->sock);
    SAFE_FREE(polctx);
}
static inline int32_t _post_disconnectex(struct sock_ctx *psockctx)
{
    struct overlap_ctx *pdisconol = (struct overlap_ctx *)MALLOC(sizeof(struct overlap_ctx));
    if (NULL == pdisconol)
    {
        LOG_ERROR("%s", ERRSTR_MEMORY);
        return ERR_FAILED;
    }
    ZERO(&pdisconol->overlapped, sizeof(pdisconol->overlapped));
    pdisconol->overlap_cb = _on_disconnectex;
    pdisconol->sock = ((struct overlap_send_ctx *)psockctx->overlap)->ol.sock;
    //对端会收到0字节
    if (!psockctx->disconnectex(pdisconol->sock, &pdisconol->overlapped, 0, 0))
    {
        int32_t irtn = ERRNO;
        if (ERROR_IO_PENDING != irtn)
        {
            NET_ERROR(irtn, "error code:%d message:%s", irtn, ERRORSTR(irtn));
            SAFE_FREE(pdisconol);
            return irtn;
        }
    }
    return ERR_OK;
}
void sock_close(struct sock_ctx *psockctx)
{
    if (0 != psockctx->closed)
    {
        return;
    }

    if (SOCK_DGRAM == psockctx->socktype)
    {
        SOCK_CLOSE(((struct overlap_sendto_ctx *)psockctx->overlap)->ol.sock);
    }
    else
    {
        if (ERR_OK != _post_disconnectex(psockctx))
        {
            SOCK_CLOSE(((struct overlap_send_ctx *)psockctx->overlap)->ol.sock);
        }
    }
}
static inline int32_t _post_recv(struct overlap_recv_ctx *poverlap)
{
    ZERO(&poverlap->ol.overlapped, sizeof(poverlap->ol.overlapped));
    poverlap->flag = 0;
    poverlap->bytes = 0;
    poverlap->iovcnt = buffer_write_iov_application(poverlap->sockctx->recvbuf,
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
            buffer_write_iov_commit(poverlap->sockctx->recvbuf, 0,
                poverlap->wsabuf, poverlap->iovcnt);
            return irtn;
        }
    }
    return ERR_OK;
}
static inline void _tcp_close(struct overlap_recv_ctx *precvol)
{
    mutex_lock(&precvol->sockctx->lock_postsc);
    if (ATOMIC_CAS(&precvol->sockctx->closed, 0, 1))
    {
        struct ev_sock_ctx *pev = ev_sock_close(precvol->ol.sock,
            precvol->sockctx, precvol->sockctx->socktype);
        int32_t irtn = chan_send(_get_rsc_chan(precvol->sockctx), &pev->ev);
        mutex_unlock(&precvol->sockctx->lock_postsc);
        if (ERR_OK != irtn)
        {
            LOG_ERROR("%s", "post close event failed.");
            SAFE_FREE(pev);
            _sock_free(precvol->sockctx);
        }
        SOCK_CLOSE(precvol->ol.sock);
        SAFE_FREE(precvol);
    }
    else
    {
        mutex_unlock(&precvol->sockctx->lock_postsc);
    }
}
static inline void _on_recv(struct netev_ctx *piocpctx, 
    struct overlap_ctx *polctx, const uint32_t uibyte, const int32_t ierr)
{
    struct overlap_recv_ctx *precvol = UPCAST(polctx, struct overlap_recv_ctx, ol);
    if (0 == uibyte
        || ERR_OK != ierr)
    {
        buffer_write_iov_commit(precvol->sockctx->recvbuf, 0,
            precvol->wsabuf, precvol->iovcnt);
        _tcp_close(precvol);
        return;
    }

    buffer_write_iov_commit(precvol->sockctx->recvbuf, (size_t)uibyte,
        precvol->wsabuf, precvol->iovcnt);
    //投递EV_RECV消息
    struct ev_sock_ctx *pev = ev_sock_recv(precvol->ol.sock, (int32_t)uibyte, 
        precvol->sockctx, precvol->sockctx->socktype);
    struct chan_ctx *pchan = _get_rsc_chan(precvol->sockctx);
    if (ERR_OK != chan_send(pchan, &pev->ev))
    {
        LOG_ERROR("%s", "post recv event failed.");
        SAFE_FREE(pev);
    }
    int32_t irtn = _post_recv(precvol);
    if (ERR_OK != irtn)
    {
        NET_ERROR(irtn, "error code:%d message:%s", irtn, ERRORSTR(irtn));
        _tcp_close(precvol);
    }
}
static inline int32_t _post_recv_from(struct overlap_recv_from_ctx *poverlap)
{
    ZERO(&poverlap->ol.overlapped, sizeof(poverlap->ol.overlapped));
    poverlap->flag = 0;
    poverlap->bytes = 0;
    poverlap->addrlen = (int32_t)sizeof(poverlap->rmtaddr);
    poverlap->iovcnt = buffer_write_iov_application(poverlap->sockctx->recvbuf,
        MAX_RECV_FROM_IOV_SIZE, poverlap->wsabuf, MAX_RECV_FROM_IOV_COUNT);

    int32_t irtn = WSARecvFrom(poverlap->ol.sock,
        poverlap->wsabuf,
        (DWORD)poverlap->iovcnt,
        &poverlap->bytes,
        &poverlap->flag,
        (struct sockaddr*)&poverlap->rmtaddr,
        &poverlap->addrlen,
        &poverlap->ol.overlapped,
        NULL);
    if (ERR_OK != irtn)
    {
        irtn = ERRNO;
        if (WSA_IO_PENDING != irtn)
        {
            buffer_write_iov_commit(poverlap->sockctx->recvbuf, 0, 
                poverlap->wsabuf, poverlap->iovcnt);
            return irtn;
        }
    }
    return ERR_OK;
}
static inline void _udp_close(struct overlap_recv_from_ctx *precvfol)
{
    mutex_lock(&precvfol->sockctx->lock_postsc);
    if (ATOMIC_CAS(&precvfol->sockctx->closed, 0, 1))
    {
        struct ev_sock_ctx *pev = ev_sock_close(precvfol->ol.sock,
            precvfol->sockctx, precvfol->sockctx->socktype);
        int32_t irtn = chan_send(_get_rsc_chan(precvfol->sockctx), &pev->ev);
        mutex_unlock(&precvfol->sockctx->lock_postsc);
        if (ERR_OK != irtn)
        {
            LOG_ERROR("%s", "post close event failed.");
            SAFE_FREE(pev);
            _sock_free(precvfol->sockctx);
        }
        SOCK_CLOSE(precvfol->ol.sock);
        SAFE_FREE(precvfol);
    }
    else
    {
        mutex_unlock(&precvfol->sockctx->lock_postsc);
    }
}
static inline void _on_recv_from(struct netev_ctx *piocpctx, 
    struct overlap_ctx *polctx, const uint32_t uibyte, const int32_t ierr)
{
    struct overlap_recv_from_ctx *precvfol = UPCAST(polctx, struct overlap_recv_from_ctx, ol);
    if (0 == uibyte
        || ERR_OK != ierr)
    {
        buffer_write_iov_commit(precvfol->sockctx->recvbuf, 0, 
            precvfol->wsabuf, precvfol->iovcnt);
        _udp_close(precvfol);
        return;
    }
    
    buffer_write_iov_commit(precvfol->sockctx->recvbuf, (size_t)uibyte, 
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
    if (ERR_OK != chan_send(_get_rsc_chan(precvfol->sockctx), &pev->ev))
    {
        LOG_ERROR("%s", "post recv event failed.");
        SAFE_FREE(pev);
    }
    irtn = _post_recv_from(precvfol);
    if (ERR_OK != irtn)
    {
        NET_ERROR(irtn, "error code:%d message:%s", irtn, ERRORSTR(irtn));
        _udp_close(precvfol);
    }
}
static inline int32_t _post_send(struct overlap_send_ctx *poverlap)
{
    if (0 != poverlap->sockctx->closed)
    {
        return ERR_FAILED;
    }
    uint32_t uicnt = buffer_read_iov_application(poverlap->sockctx->sendbuf,
        MAX_SEND_IOV_SIZE, poverlap->wsabuf, MAX_SEND_IOV_COUNT);
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
            buffer_read_iov_commit(poverlap->sockctx->sendbuf, 0);
            NET_ERROR(irtn, "error code:%d message:%s", irtn, ERRORSTR(irtn));
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
        buffer_read_iov_commit(psendol->sockctx->sendbuf, 0);
        psendol->sockctx->sending = 0;
        return;
    }

    buffer_read_iov_commit(psendol->sockctx->sendbuf, uibyte);
    if (0 != psendol->sockctx->postsendev)
    {
        //投递EV_SEND消息
        mutex_lock(&psendol->sockctx->lock_postsc);
        if (0 == psendol->sockctx->closed)
        {
            struct ev_sock_ctx *pev = ev_sock_send(psendol->ol.sock, (int32_t)uibyte,
                psendol->sockctx, psendol->sockctx->socktype);
            if (ERR_OK != chan_send(_get_rsc_chan(psendol->sockctx), &pev->ev))
            {
                LOG_ERROR("%s", "post send event failed.");
                SAFE_FREE(pev);
            }
        }
        mutex_unlock(&psendol->sockctx->lock_postsc);
    }
    if (ERR_OK != _post_send(psendol))
    {
        psendol->sockctx->sending = 0;
    }
}
static inline int32_t _post_sendto(struct overlap_sendto_ctx *poverlap)
{
    if (0 != poverlap->sockctx->closed)
    {
        return ERR_FAILED;
    }
    struct netaddr_ctx *paddr = NULL;
    uint32_t uicnt = buffer_piece_read_iov_application(poverlap->sockctx->sendbuf,
        &poverlap->piece, poverlap->wsabuf, MAX_SENDTO_IOV_COUNT, (void **)&paddr);
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
            buffer_piece_read_iov_commit(poverlap->sockctx->sendbuf, &poverlap->piece, 0);
            NET_ERROR(irtn, "error code:%d message:%s", irtn, ERRORSTR(irtn));
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
        buffer_piece_read_iov_commit(psendtool->sockctx->sendbuf, &psendtool->piece, 0);
        psendtool->sockctx->sending = 0;
        return;
    }
   
    buffer_piece_read_iov_commit(psendtool->sockctx->sendbuf, &psendtool->piece, uibyte);
    if (0 != psendtool->sockctx->postsendev)
    {
        //投递EV_SEND消息
        mutex_lock(&psendtool->sockctx->lock_postsc);
        if (0 == psendtool->sockctx->closed)
        {
            struct ev_sock_ctx *pev = ev_sock_send(psendtool->ol.sock, (int32_t)uibyte,
                psendtool->sockctx, psendtool->sockctx->socktype);
            if (ERR_OK != chan_send(_get_rsc_chan(psendtool->sockctx), &pev->ev))
            {
                LOG_ERROR("%s", "post send event failed.");
                SAFE_FREE(pev);
            }
        }
        mutex_unlock(&psendtool->sockctx->lock_postsc);
    }
    if (ERR_OK != _post_sendto(psendtool))
    {
        psendtool->sockctx->sending = 0;
    }
}
static int32_t _accptex(struct netev_ctx *piocpctx, 
    SOCKET sock, const int32_t ifamily,
    struct chan_ctx *pchan, const uint32_t uichancnt)
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
    plistener->chancnt = uichancnt;
    plistener->family = ifamily;
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
                SAFE_CLOSE_SOCK(plistener->overlap_acpt[i].ol.sock);
            }
            mutex_free(&plistener->mutex);
            SAFE_FREE(plistener);
            return irtn;
        }
    }

    return ERR_OK;
}
SOCKET netev_listener(struct netev_ctx *piocpctx, struct chan_ctx *pchan,
    const uint32_t uichancnt, const char *phost, const uint16_t usport)
{
    struct netaddr_ctx addr;
    int32_t irtn = netaddr_sethost(&addr, phost, usport);
    if (ERR_OK != irtn)
    {
        LOG_ERROR("error code:%d message:%s", irtn, gai_strerror(irtn));
        return INVALID_SOCK;
    }

    int32_t ifamily = netaddr_family(&addr);
    SOCKET sock = WSASocket(ifamily,
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
        SOCK_CLOSE(sock);
        return INVALID_SOCK;
    }
    if (ERR_OK != listen(sock, SOCKK_BACKLOG))
    {
        irtn = ERRNO;
        LOG_ERROR("error code:%d message:%s", irtn, ERRORSTR(irtn));
        SOCK_CLOSE(sock);
        return INVALID_SOCK;
    }
    if (ERR_OK != _accptex(piocpctx, sock, ifamily, pchan, uichancnt))
    {
        SOCK_CLOSE(sock);
        return INVALID_SOCK;
    }

    return sock;
}
static inline int32_t _trybind(SOCKET sock, const int32_t ifamily)
{
    int32_t irtn;
    struct netaddr_ctx addr;
    if (AF_INET == ifamily)
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
            NET_ERROR(irtn, "error code:%d message:%s", irtn, ERRORSTR(irtn));
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

    int32_t ifamily = netaddr_family(&addr);
    SOCKET sock = WSASocket(ifamily,
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
    if (ERR_OK != _trybind(sock, ifamily))
    {
        SOCK_CLOSE(sock);
        return INVALID_SOCK;
    }
    if (ERR_OK != _connectex(piocpctx, &addr, sock, pchan))
    {
        SOCK_CLOSE(sock);
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
            SOCK_CLOSE(fd);
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
        SOCK_CLOSE(fd);
        return irtn;
    }

    return ERR_OK;
}
SOCKET sock_handle(struct sock_ctx *psockctx)
{
    if (SOCK_DGRAM == psockctx->socktype)
    {
        return ((struct overlap_sendto_ctx *)psockctx->overlap)->ol.sock;
    }
    else
    {
        return ((struct overlap_send_ctx *)psockctx->overlap)->ol.sock;
    }
}
int32_t _sock_can_free(struct sock_ctx *psockctx)
{
    if (0 == psockctx->sending)
    {
        return ERR_OK;
    }

    psockctx->freecnt++;
    if (psockctx->freecnt >= 5)
    {
        LOG_WARN("free sock_ctx use long time. force free it. sock %d type %d sending:%d", 
            (int32_t)sock_handle(psockctx), psockctx->socktype, (int32_t)psockctx->sending);
        return ERR_OK;
    }
    return ERR_FAILED;
}
struct buffer_ctx *sock_recvbuf(struct sock_ctx *psockctx)
{
    return psockctx->recvbuf;
}
struct buffer_ctx *sock_sendbuf(struct sock_ctx *psockctx)
{
    return psockctx->sendbuf;
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
        psendtool->piece.head = NULL;
        psendtool->piece.tail = NULL;
        psendtool->piece.free_data = FREE;
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
    psockctx->socktype = isocktyep;
    psockctx->sending = 0;
    psockctx->closed = 0;
    psockctx->freecnt = 0;
    buffer_init(psockctx->recvbuf);
    buffer_init(psockctx->sendbuf);
    mutex_init(&psockctx->lock_changech);
    mutex_init(&psockctx->lock_postsc);

    return psockctx;
}
void _sock_free(struct sock_ctx *psockctx)
{    
    if (SOCK_DGRAM == psockctx->socktype)
    {
        struct overlap_sendto_ctx *psendtool = (struct overlap_sendto_ctx *)psockctx->overlap;
        buffer_piece_node_free(&psendtool->piece);
    }
    SAFE_FREE(psockctx->overlap);
    buffer_free(psockctx->recvbuf);
    buffer_free(psockctx->sendbuf);
    mutex_free(&psockctx->lock_changech);
    mutex_free(&psockctx->lock_postsc);
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
        NET_ERROR(irtn, "error code:%d message:%s", irtn, ERRORSTR(irtn));
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
    closereset(fd);
    sockkpa(fd, SOCKKPA_DELAY, SOCKKPA_INTVL);
    poverlap->sockctx = psockctx;
    poverlap->ol.sock = fd;
    poverlap->ol.overlap_cb = _on_recv;
    int32_t irtn = _post_recv(poverlap);
    if (ERR_OK != irtn)
    {
        NET_ERROR(irtn, "error code:%d message:%s", irtn, ERRORSTR(irtn));
        SAFE_FREE(poverlap);
    }
    return irtn;
}
struct sock_ctx *netev_enable_rw(struct netev_ctx *piocpctx, SOCKET fd,
    struct chan_ctx *pchan, const int32_t ipostsendev)
{
    int32_t isocktyep = socktype(fd);
    if (ERR_FAILED == isocktyep)
    {
        LOG_ERROR("%s", "get sock type error.");
        SOCK_CLOSE(fd);
        return NULL;
    }

    struct sock_ctx *psender = _sockctx_init(fd, isocktyep);
    if (NULL == psender)
    {
        SOCK_CLOSE(fd);
        return NULL;
    }
    psender->chan = pchan;
    psender->postsendev = ipostsendev;
    psender->disconnectex = piocpctx->disconnectex;

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
        SOCK_CLOSE(fd);
        _sock_free(psender);
        return NULL;
    }

    return psender;
}
int32_t _tcp_trysend(struct sock_ctx *psockctx)
{
    if (!ATOMIC_CAS(&psockctx->sending, 0, 1))
    {
        return ERR_OK;
    }
    int32_t irtn = _post_send((struct overlap_send_ctx *)psockctx->overlap);
    if (ERR_OK != irtn)
    {
        psockctx->sending = 0;
        return irtn;
    }

    return ERR_OK;
}
int32_t tcp_send(struct sock_ctx *psockctx, void *pdata, const size_t uilens)
{
    if (0 != psockctx->closed)
    {
        return ERR_FAILED;
    }

    ASSERTAB(SOCK_STREAM == psockctx->socktype, "only support tcp.");
    ASSERTAB(ERR_OK == buffer_append(psockctx->sendbuf, pdata, uilens), "buffer_append error.");
    return _tcp_trysend(psockctx);
}
int32_t tcp_send_buf(struct sock_ctx *psockctx)
{
    if (0 != psockctx->closed)
    {
        return ERR_FAILED;
    }
    return _tcp_trysend(psockctx);
}
int32_t udp_send(struct sock_ctx *psockctx, void *pdata, const size_t uilens,
    const char *pip, const uint16_t uport)
{
    if (0 != psockctx->closed)
    {
        return ERR_FAILED;
    }
    ASSERTAB(SOCK_DGRAM == psockctx->socktype, "only support udp.");  
    struct netaddr_ctx *paddr = MALLOC(sizeof(struct netaddr_ctx));
    if (NULL == paddr)
    {
        LOG_ERROR("%s", ERRSTR_MEMORY);
        return ERR_FAILED;
    }
    int32_t irtn = netaddr_sethost(paddr, pip, uport);
    if (ERR_OK != irtn)
    {
        LOG_ERROR("error code:%d message:%s", irtn, gai_strerror(irtn));
        SAFE_FREE(paddr);
        return irtn;
    }
    struct overlap_sendto_ctx *poverlap = (struct overlap_sendto_ctx *)psockctx->overlap;
    irtn = buffer_piece_inser(psockctx->sendbuf, &poverlap->piece, paddr, pdata, uilens);
    if (ERR_OK != irtn)
    {
        SAFE_FREE(paddr);
        return irtn;
    }
    if (!ATOMIC_CAS(&psockctx->sending, 0, 1))
    {
        return ERR_OK;
    }
    irtn = _post_sendto(poverlap);
    if (ERR_OK != irtn)
    {
        psockctx->sending = 0;
        return irtn;
    }

    return ERR_OK;
}

#endif
