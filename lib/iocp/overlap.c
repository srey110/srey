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
#define MAX_SEND_IOV_SIZE   4096
#define MAX_SEND_IOV_COUNT  16
#define MAX_RECV_FROM_IOV_COUNT  4
#define MAX_SENDTO_IOV_COUNT 16
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
    SOCKET sock;
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
    SOCKET sock;
}overlap_conn_ctx;

typedef struct overlap_recv_ctx
{
    struct overlap_ctx ol;//overlapped
    uint32_t iovcnt;
    DWORD bytes;
    DWORD flag;
    IOV_TYPE wsabuf[MAX_RECV_IOV_COUNT];
}overlap_recv_ctx;
typedef struct overlap_send_ctx
{
    struct overlap_ctx ol;//overlapped
    uint32_t iovcnt;
    DWORD bytes;
    IOV_TYPE wsabuf[MAX_SEND_IOV_COUNT];
}overlap_send_ctx;
typedef struct sock_ctx
{
    int32_t socktype;
    int32_t freecnt;
    int32_t postsendev;
    volatile atomic_t closed;
    volatile atomic_t recvref;
    volatile atomic_t sendref;
    struct chan_ctx *chan;//EV_CLOSE  EV_RECV  EV_SEND
    struct netev_ctx *netev;
    SOCKET sock;
    mutex_ctx lock_changech;//更改chan时   
    mutex_ctx lock_postsc;//EV_CLOSE投递后不再投递EV_SEND 
    struct buffer_ctx recvbuf;
    struct buffer_ctx sendbuf;
}sock_ctx;
typedef struct overlap_tcp_ctx
{
    struct sock_ctx sockctx;
    struct overlap_recv_ctx recvol;
    struct overlap_send_ctx sendol;
}overlap_tcp_ctx;

typedef struct overlap_recv_from_ctx
{
    struct overlap_ctx ol;//overlapped
    int32_t addrlen;
    uint32_t iovcnt;
    DWORD bytes;
    DWORD flag;
    IOV_TYPE wsabuf[MAX_RECV_FROM_IOV_COUNT];
    struct sockaddr_storage rmtaddr;//存储数据来源IP地址    
}overlap_recv_from_ctx;
typedef struct overlap_sendto_ctx
{
    struct overlap_ctx ol;//overlapped    
    uint32_t iovcnt;
    DWORD bytes;
    struct piece_iov_ctx piece;
    IOV_TYPE wsabuf[MAX_SENDTO_IOV_COUNT];
}overlap_sendto_ctx;
typedef struct overlap_udp_ctx
{
    struct sock_ctx sockctx;
    struct overlap_recv_from_ctx recvfol;
    struct overlap_sendto_ctx sendtool;
}overlap_udp_ctx;

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
    pacpolctx->sock = sock;

    if (!piocpctx->acceptex(pacpolctx->listener->sock,//Listen Socket
        pacpolctx->sock,                            //Accept Socket
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
            SOCK_CLOSE(pacpolctx->sock);
            return irtn;
        }
    }
    return ERR_OK;
}
static inline void _on_accept(struct netev_ctx *piocpctx, 
    struct overlap_ctx *polctx, const uint32_t uibyte, const int32_t ierr)
{
    struct overlap_acpt_ctx *pacceptol = UPCAST(polctx, struct overlap_acpt_ctx, ol);
    SOCKET sock = pacceptol->sock;  
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
    SOCKET sock = pconnol->sock;
    if (ERR_OK != ierr)
    {
        SAFE_CLOSE_SOCK(sock);
    }
    //投递EV_CONNECT
    struct ev_sock_ctx *pev = ev_sock_connect(pconnol->sock, ierr);
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
    SAFE_FREE(polctx);
}
void sock_close(struct sock_ctx *psockctx)
{
    if (0 != ATOMIC_GET(&psockctx->closed))
    {
        return;
    }
    //有问题
    if (SOCK_DGRAM == psockctx->socktype)
    {
        SOCK_CLOSE(psockctx->sock);
        return;
    }
    struct overlap_ctx *pdisconol =
        (struct overlap_ctx *)MALLOC(sizeof(struct overlap_ctx));
    if (NULL == pdisconol)
    {
        LOG_ERROR("%s", ERRSTR_MEMORY);
        SOCK_CLOSE(psockctx->sock);
        return;
    }
    ZERO(&pdisconol->overlapped, sizeof(pdisconol->overlapped));
    pdisconol->overlap_cb = _on_disconnectex;
    //对端会收到0字节
    if (!psockctx->netev->disconnectex(psockctx->sock, &pdisconol->overlapped, 0, 0))
    {
        int32_t irtn = ERRNO;
        if (ERROR_IO_PENDING != irtn)
        {
            NET_ERROR(irtn, "error code:%d message:%s", irtn, ERRORSTR(irtn));
            SAFE_FREE(pdisconol);
            SOCK_CLOSE(psockctx->sock);
            return;
        }
    }
}
static inline struct overlap_tcp_ctx *_sockctx_to_tcpol(struct sock_ctx *psockctx)
{
    return UPCAST(psockctx, struct overlap_tcp_ctx, sockctx);
}
static inline struct overlap_udp_ctx *_sockctx_to_udpol(struct sock_ctx *psockctx)
{
    return UPCAST(psockctx, struct overlap_udp_ctx, sockctx);
}
static inline int32_t _post_recv(struct overlap_tcp_ctx *ptcp)
{
    ZERO(&ptcp->recvol.ol.overlapped, sizeof(ptcp->recvol.ol.overlapped));
    ptcp->recvol.flag = 0;
    ptcp->recvol.bytes = 0;
    ptcp->recvol.iovcnt = buffer_write_iov_application(&ptcp->sockctx.recvbuf,
        MAX_RECV_IOV_SIZE, ptcp->recvol.wsabuf, MAX_RECV_IOV_COUNT);

    ATOMIC_ADD(&ptcp->sockctx.recvref, 1);
    int32_t irtn = WSARecv(ptcp->sockctx.sock,
        ptcp->recvol.wsabuf,
        (DWORD)ptcp->recvol.iovcnt,
        &ptcp->recvol.bytes,
        &ptcp->recvol.flag,
        &ptcp->recvol.ol.overlapped,
        NULL);
    if (ERR_OK != irtn)
    {
        irtn = ERRNO;
        if (WSA_IO_PENDING != irtn)
        {
            buffer_write_iov_commit(&ptcp->sockctx.recvbuf, 0,
                ptcp->recvol.wsabuf, ptcp->recvol.iovcnt);
            ATOMIC_ADD(&ptcp->sockctx.recvref, -1);
            return irtn;
        }
    }
    return ERR_OK;
}
static inline void _on_tcp_close(struct overlap_tcp_ctx *ptcp)
{
    if (!ATOMIC_CAS(&ptcp->sockctx.closed, 0, 1))
    {
        return;
    }
    struct ev_sock_ctx *pev = ev_sock_close(ptcp->sockctx.sock, &ptcp->sockctx);
    mutex_lock(&ptcp->sockctx.lock_postsc);
    int32_t irtn = chan_send(_get_rsc_chan(&ptcp->sockctx), &pev->ev);
    mutex_unlock(&ptcp->sockctx.lock_postsc);
    if (ERR_OK != irtn)
    {
        LOG_ERROR("%s", "post close event failed.");
        SAFE_FREE(pev);
        _sock_free(&ptcp->sockctx);
    }
}
static inline void _on_recv(struct netev_ctx *piocpctx, 
    struct overlap_ctx *polctx, const uint32_t uibyte, const int32_t ierr)
{
    struct overlap_recv_ctx *precvol = UPCAST(polctx, struct overlap_recv_ctx, ol);
    struct overlap_tcp_ctx *ptcp = UPCAST(precvol, struct overlap_tcp_ctx, recvol);
    if (0 == uibyte
        || ERR_OK != ierr)
    {
        buffer_write_iov_commit(&ptcp->sockctx.recvbuf, 0, precvol->wsabuf, precvol->iovcnt);
        _on_tcp_close(ptcp);
        ATOMIC_ADD(&ptcp->sockctx.recvref, -1);
        return;
    }

    buffer_write_iov_commit(&ptcp->sockctx.recvbuf, (size_t)uibyte, precvol->wsabuf, precvol->iovcnt);
    //投递EV_RECV消息
    struct ev_sock_ctx *pev = ev_sock_recv(ptcp->sockctx.sock, (int32_t)uibyte, &ptcp->sockctx);
    if (ERR_OK != chan_send(_get_rsc_chan(&ptcp->sockctx), &pev->ev))
    {
        LOG_ERROR("%s", "post recv event failed.");
        SAFE_FREE(pev);
    }
    int32_t irtn = _post_recv(ptcp);
    if (ERR_OK != irtn)
    {
        NET_ERROR(irtn, "error code:%d message:%s", irtn, ERRORSTR(irtn));
        _on_tcp_close(ptcp);
    }
    ATOMIC_ADD(&ptcp->sockctx.recvref, -1);
}
static inline int32_t _post_send(struct overlap_tcp_ctx *ptcp)
{
    if (0 != ATOMIC_GET(&ptcp->sockctx.closed))
    {
        return ERR_FAILED;
    }
    uint32_t uicnt = buffer_read_iov_application(&ptcp->sockctx.sendbuf,
        MAX_SEND_IOV_SIZE, ptcp->sendol.wsabuf, MAX_SEND_IOV_COUNT);
    if (0 == uicnt)
    {
        return ERR_FAILED;
    }
    ZERO(&ptcp->sendol.ol.overlapped, sizeof(ptcp->sendol.ol.overlapped));
    ptcp->sendol.iovcnt = uicnt;

    ATOMIC_ADD(&ptcp->sockctx.sendref, 1);
    int32_t irtn = WSASend(ptcp->sockctx.sock,
        ptcp->sendol.wsabuf,
        (DWORD)uicnt,
        &ptcp->sendol.bytes,
        0,
        &ptcp->sendol.ol.overlapped,
        NULL);
    if (ERR_OK != irtn)
    {
        irtn = ERRNO;
        if (WSA_IO_PENDING != irtn)
        {
            buffer_read_iov_commit(&ptcp->sockctx.sendbuf, 0);
            NET_ERROR(irtn, "error code:%d message:%s", irtn, ERRORSTR(irtn));
            ATOMIC_ADD(&ptcp->sockctx.sendref, -1);
            return irtn;
        }
    }
    return ERR_OK;
}
static inline void _on_send(struct netev_ctx *piocpctx,
    struct overlap_ctx *polctx, const uint32_t uibyte, const int32_t ierr)
{
    struct overlap_send_ctx *psendol = UPCAST(polctx, struct overlap_send_ctx, ol);
    struct overlap_tcp_ctx *ptcp = UPCAST(psendol, struct overlap_tcp_ctx, sendol);
    if (0 == uibyte
        || ERR_OK != ierr)
    {
        buffer_read_iov_commit(&ptcp->sockctx.sendbuf, 0);
        ATOMIC_ADD(&ptcp->sockctx.sendref, -1);
        return;
    }

    buffer_read_iov_commit(&ptcp->sockctx.sendbuf, uibyte);
    if (0 != ptcp->sockctx.postsendev)
    {
        //投递EV_SEND消息
        mutex_lock(&ptcp->sockctx.lock_postsc);
        if (0 == ATOMIC_GET(&ptcp->sockctx.closed))
        {
            struct ev_sock_ctx *pev = ev_sock_send(ptcp->sockctx.sock,
                (int32_t)uibyte, &ptcp->sockctx);
            if (ERR_OK != chan_send(_get_rsc_chan(&ptcp->sockctx), &pev->ev))
            {
                LOG_ERROR("%s", "post send event failed.");
                SAFE_FREE(pev);
            }
        }
        mutex_unlock(&ptcp->sockctx.lock_postsc);
    }
    (void)_post_send(ptcp);
    ATOMIC_ADD(&ptcp->sockctx.sendref, -1);
}
static inline int32_t _post_recv_from(struct overlap_udp_ctx *pudp)
{
    ZERO(&pudp->recvfol.ol.overlapped, sizeof(pudp->recvfol.ol.overlapped));
    pudp->recvfol.flag = 0;
    pudp->recvfol.bytes = 0;
    pudp->recvfol.addrlen = (int32_t)sizeof(pudp->recvfol.rmtaddr);
    pudp->recvfol.iovcnt = buffer_write_iov_application(&pudp->sockctx.recvbuf,
        MAX_RECV_FROM_IOV_SIZE, pudp->recvfol.wsabuf, MAX_RECV_FROM_IOV_COUNT);

    ATOMIC_ADD(&pudp->sockctx.recvref, 1);
    int32_t irtn = WSARecvFrom(pudp->sockctx.sock,
        pudp->recvfol.wsabuf,
        (DWORD)pudp->recvfol.iovcnt,
        &pudp->recvfol.bytes,
        &pudp->recvfol.flag,
        (struct sockaddr*)&pudp->recvfol.rmtaddr,
        &pudp->recvfol.addrlen,
        &pudp->recvfol.ol.overlapped,
        NULL);
    if (ERR_OK != irtn)
    {
        irtn = ERRNO;
        if (WSA_IO_PENDING != irtn)
        {
            buffer_write_iov_commit(&pudp->sockctx.recvbuf, 0,
                pudp->recvfol.wsabuf, pudp->recvfol.iovcnt);
            ATOMIC_ADD(&pudp->sockctx.recvref, -1);
            return irtn;
        }
    }
    return ERR_OK;
}
static inline void _on_udp_close(struct overlap_udp_ctx *pudp)
{
    if (!ATOMIC_CAS(&pudp->sockctx.closed, 0, 1))
    {
        return;
    }
    struct ev_sock_ctx *pev = ev_sock_close(pudp->sockctx.sock, &pudp->sockctx);
    mutex_lock(&pudp->sockctx.lock_postsc);
    int32_t irtn = chan_send(_get_rsc_chan(&pudp->sockctx), &pev->ev);
    mutex_unlock(&pudp->sockctx.lock_postsc);
    if (ERR_OK != irtn)
    {
        LOG_ERROR("%s", "post close event failed.");
        SAFE_FREE(pev);
        _sock_free(&pudp->sockctx);
    }
}
static inline void _on_recv_from(struct netev_ctx *piocpctx,
    struct overlap_ctx *polctx, const uint32_t uibyte, const int32_t ierr)
{
    struct overlap_recv_from_ctx *precvfol = UPCAST(polctx, struct overlap_recv_from_ctx, ol);
    struct overlap_udp_ctx *pudp = UPCAST(precvfol, struct overlap_udp_ctx, recvfol);
    if (0 == uibyte
        || ERR_OK != ierr)
    {
        PRINTF("%s", "111111111111111111111111");
        buffer_write_iov_commit(&pudp->sockctx.recvbuf, 0, precvfol->wsabuf, precvfol->iovcnt);
        _on_udp_close(pudp);
        ATOMIC_ADD(&pudp->sockctx.recvref, -1);
        return;
    }

    buffer_write_iov_commit(&pudp->sockctx.recvbuf, (size_t)uibyte, precvfol->wsabuf, precvfol->iovcnt);
    //投递EV_RECV消息
    struct ev_sock_ctx *pev = ev_sock_recv(pudp->sockctx.sock, (int32_t)uibyte, &pudp->sockctx);
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
    if (ERR_OK != chan_send(_get_rsc_chan(&pudp->sockctx), &pev->ev))
    {
        LOG_ERROR("%s", "post recv event failed.");
        SAFE_FREE(pev);
    }
    irtn = _post_recv_from(pudp);
    if (ERR_OK != irtn)
    {
        NET_ERROR(irtn, "error code:%d message:%s", irtn, ERRORSTR(irtn));
        _on_udp_close(pudp);
    }
    ATOMIC_ADD(&pudp->sockctx.recvref, -1);
}
static inline int32_t _post_sendto(struct overlap_udp_ctx *pudp)
{
    if (0 != ATOMIC_GET(&pudp->sockctx.closed))
    {
        return ERR_FAILED;
    }
    struct netaddr_ctx *paddr = NULL;
    uint32_t uicnt = buffer_piece_read_iov_application(&pudp->sockctx.sendbuf,
        &pudp->sendtool.piece, pudp->sendtool.wsabuf, MAX_SENDTO_IOV_COUNT, (void **)&paddr);
    if (0 == uicnt)
    {
        return ERR_FAILED;
    }
    ZERO(&pudp->sendtool.ol.overlapped, sizeof(pudp->sendtool.ol.overlapped));
    pudp->sendtool.bytes = 0;
    pudp->sendtool.iovcnt = uicnt;

    ATOMIC_ADD(&pudp->sockctx.sendref, 1);
    int32_t irtn = WSASendTo(pudp->sockctx.sock,
        pudp->sendtool.wsabuf,
        (DWORD)pudp->sendtool.iovcnt,
        &pudp->sendtool.bytes,
        0,
        netaddr_addr(paddr),
        netaddr_size(paddr),
        &pudp->sendtool.ol.overlapped,
        NULL);
    if (ERR_OK != irtn)
    {
        irtn = ERRNO;
        if (ERROR_IO_PENDING != irtn)
        {
            buffer_piece_read_iov_commit(&pudp->sockctx.sendbuf, &pudp->sendtool.piece, 0);
            NET_ERROR(irtn, "error code:%d message:%s", irtn, ERRORSTR(irtn));
            ATOMIC_ADD(&pudp->sockctx.sendref, -1);
            return irtn;
        }
    }
    return ERR_OK;
}
static inline void _on_sendto(struct netev_ctx *piocpctx,
    struct overlap_ctx *polctx, const uint32_t uibyte, const int32_t ierr)
{
    struct overlap_sendto_ctx *psendtool = UPCAST(polctx, struct overlap_sendto_ctx, ol);
    struct overlap_udp_ctx *pudp = UPCAST(psendtool, struct overlap_udp_ctx, sendtool);
    if (0 == uibyte
        || ERR_OK != ierr)
    {
        buffer_piece_read_iov_commit(&pudp->sockctx.sendbuf, &psendtool->piece, 0);
        ATOMIC_ADD(&pudp->sockctx.sendref, -1);
        return;
    }

    buffer_piece_read_iov_commit(&pudp->sockctx.sendbuf, &psendtool->piece, uibyte);
    if (0 != pudp->sockctx.postsendev)
    {
        //投递EV_SEND消息
        mutex_lock(&pudp->sockctx.lock_postsc);
        if (0 == ATOMIC_GET(&pudp->sockctx.closed))
        {
            struct ev_sock_ctx *pev = ev_sock_send(pudp->sockctx.sock, (int32_t)uibyte, &pudp->sockctx);
            if (ERR_OK != chan_send(_get_rsc_chan(&pudp->sockctx), &pev->ev))
            {
                LOG_ERROR("%s", "post send event failed.");
                SAFE_FREE(pev);
            }
        }
        mutex_unlock(&pudp->sockctx.lock_postsc);
    }
    (void)_post_sendto(pudp);
    ATOMIC_ADD(&pudp->sockctx.sendref, -1);
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
                SOCK_CLOSE(plistener->overlap_acpt[i].sock);
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
    poverlapped->sock = sock;
    poverlapped->bytes = 0;
    poverlapped->chan = pchan;

    if (!piocpctx->connectex(poverlapped->sock,
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
    socknbio(sock);
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
        NET_ERROR(irtn, "error code:%d message:%s", irtn, ERRORSTR(irtn));
        SOCK_CLOSE(fd);
        return irtn;
    }

    return ERR_OK;
}
SOCKET sock_handle(struct sock_ctx *psockctx)
{
    return psockctx->sock;
}
int32_t sock_type(struct sock_ctx *psockctx)
{
    return psockctx->socktype;
}
static inline int32_t _check_olresult(struct overlap_ctx *poverlap, SOCKET sock)
{
    DWORD dbytes;
    DWORD flags;
    BOOL brtn = WSAGetOverlappedResult(sock, &poverlap->overlapped, &dbytes, FALSE, &flags);
    if (!brtn)
    {
        if (WSA_IO_PENDING == ERRNO)
        {
            LOG_WARN("%s", "WSAGetOverlappedResult return WSA_IO_PENDING.");
            return ERR_FAILED;
        }
    }
    return ERR_OK;
}
int32_t _sock_can_free(struct sock_ctx *psockctx)
{
    atomic_t uisendref = ATOMIC_GET(&psockctx->sendref);
    atomic_t uirecvref = ATOMIC_GET(&psockctx->recvref);
    if (0 == uisendref
        && 0 == uirecvref)
    {
        return ERR_OK;
    }
    psockctx->freecnt++;
    if (psockctx->freecnt >= 5)
    {
        psockctx->freecnt = 0;
        LOG_WARN("free sock_ctx type: use long time. sock %d sending:%d recving:%d",
            psockctx->socktype, (int32_t)psockctx->sock, uisendref, uirecvref);
        if (0 != uisendref)
        {
            struct overlap_ctx *pol;
            if (SOCK_STREAM == psockctx->socktype)
            {
                pol = &_sockctx_to_tcpol(psockctx)->sendol.ol;
            }
            else
            {
                pol = &_sockctx_to_udpol(psockctx)->sendtool.ol;                
            }
            if (ERR_OK != _check_olresult(pol, psockctx->sock))
            {
                return ERR_FAILED;
            }
        }
        if (0 != uirecvref)
        {
            struct overlap_ctx *pol;
            if (SOCK_STREAM == psockctx->socktype)
            {
                pol = &_sockctx_to_tcpol(psockctx)->recvol.ol;
            }
            else
            {
                pol = &_sockctx_to_udpol(psockctx)->recvfol.ol;                
            }
            if (ERR_OK != _check_olresult(pol, psockctx->sock))
            {
                return ERR_FAILED;
            }
        }
        return ERR_OK;
    }
    return ERR_FAILED;
}
struct buffer_ctx *sock_recvbuf(struct sock_ctx *psockctx)
{
    return &psockctx->recvbuf;
}
struct buffer_ctx *sock_sendbuf(struct sock_ctx *psockctx)
{
    return &psockctx->sendbuf;
}
static inline struct overlap_tcp_ctx *_sockctx_tcp_init()
{
    struct overlap_tcp_ctx *ptcp = 
        (struct overlap_tcp_ctx *)MALLOC(sizeof(struct overlap_tcp_ctx));
    if (NULL == ptcp)
    {
        LOG_ERROR("%s", ERRSTR_MEMORY);
        return NULL;
    }
    ptcp->sendol.ol.overlap_cb = _on_send;
    ptcp->recvol.ol.overlap_cb = _on_recv;    
    return ptcp;
}
static inline struct overlap_udp_ctx *_sockctx_udp_init()
{
    struct overlap_udp_ctx *pudp =
        (struct overlap_udp_ctx *)MALLOC(sizeof(struct overlap_udp_ctx));
    if (NULL == pudp)
    {
        LOG_ERROR("%s", ERRSTR_MEMORY);
        return NULL;
    }
    pudp->sendtool.piece.free_data = FREE;
    pudp->sendtool.piece.head = NULL;
    pudp->sendtool.piece.tail = NULL;
    pudp->sendtool.ol.overlap_cb = _on_sendto;
    pudp->recvfol.ol.overlap_cb = _on_recv_from;
    return pudp;
}
static inline void _sockctx_init(struct sock_ctx *psockctx, 
    struct netev_ctx *piocpctx, SOCKET fd, struct chan_ctx *pchan, 
    const int32_t isocktype, const int32_t ipostsendev)
{
    psockctx->socktype = isocktype;
    psockctx->sock = fd;
    psockctx->chan = pchan;
    psockctx->netev = piocpctx;
    psockctx->postsendev = ipostsendev;
    psockctx->closed = 0;
    psockctx->freecnt = 0;
    psockctx->sendref = 0;
    psockctx->recvref = 0;    
    buffer_init(&psockctx->recvbuf);
    buffer_init(&psockctx->sendbuf);
    mutex_init(&psockctx->lock_postsc);
    mutex_init(&psockctx->lock_changech);
}
void _sock_free(struct sock_ctx *psockctx)
{
    SOCK_CLOSE(psockctx->sock);
    mutex_free(&psockctx->lock_changech);
    mutex_free(&psockctx->lock_postsc);
    buffer_free(&psockctx->recvbuf);
    buffer_free(&psockctx->sendbuf);
    if (SOCK_STREAM == psockctx->socktype)
    {
        FREE(_sockctx_to_tcpol(psockctx));
    }
    else
    {
        struct overlap_udp_ctx *pudp = _sockctx_to_udpol(psockctx);
        buffer_piece_node_free(&pudp->sendtool.piece);
        FREE(pudp);
    }
}
struct sock_ctx *netev_enable_rw(struct netev_ctx *piocpctx, SOCKET fd,
    struct chan_ctx *pchan, const int32_t ipostsendev)
{
    int32_t iscoktype = socktype(fd);
    if (ERR_FAILED == iscoktype)
    {
        SOCK_CLOSE(fd);
        return NULL;
    }
    int32_t irtn;
    struct sock_ctx *psockctx;
    if (SOCK_STREAM == iscoktype)
    {
        struct overlap_tcp_ctx *ptcp = _sockctx_tcp_init();
        if (NULL == ptcp)
        {
            SOCK_CLOSE(fd);
            return NULL;
        }
        socknodelay(fd);
        closereset(fd);
        sockkpa(fd, SOCKKPA_DELAY, SOCKKPA_INTVL);
        psockctx = &ptcp->sockctx;
        _sockctx_init(psockctx, piocpctx, fd, pchan, iscoktype, ipostsendev);
        irtn = _post_recv(ptcp);
    }
    else
    {
        struct overlap_udp_ctx *pudp = _sockctx_udp_init();
        if (NULL == pudp)
        {
            SOCK_CLOSE(fd);
            return NULL;
        }
        psockctx = &pudp->sockctx;
        _sockctx_init(psockctx, piocpctx, fd, pchan, iscoktype, ipostsendev);
        irtn = _post_recv_from(pudp);
    }
    if (ERR_OK != irtn)
    {
        NET_ERROR(irtn, "error code:%d message:%s", irtn, ERRORSTR(irtn));
        _sock_free(psockctx);
        return NULL;
    }

    return psockctx;
}
int32_t _tcp_trysend(struct overlap_tcp_ctx *ptcp)
{
    if (!ATOMIC_CAS(&ptcp->sockctx.sendref, 0, 1))
    {
        return ERR_OK;
    }
    int32_t irtn = _post_send(ptcp);
    ATOMIC_ADD(&ptcp->sockctx.sendref, -1);
    if (ERR_OK != irtn)
    {
        return irtn;
    }
    return ERR_OK;
}
int32_t sock_send(struct sock_ctx *psockctx, void *pdata, const size_t uilens)
{
    if (0 != ATOMIC_GET(&psockctx->closed))
    {
        return ERR_FAILED;
    }
    ASSERTAB(SOCK_STREAM == psockctx->socktype, "only support tcp.");
    struct overlap_tcp_ctx *ptcp = _sockctx_to_tcpol(psockctx);
    ASSERTAB(ERR_OK == buffer_append(&ptcp->sockctx.sendbuf, pdata, uilens), "buffer_append error.");
    return _tcp_trysend(ptcp);
}
int32_t sock_send_buf(struct sock_ctx *psockctx)
{
    if (0 != ATOMIC_GET(&psockctx->closed))
    {
        return ERR_FAILED;
    }
    ASSERTAB(SOCK_STREAM == psockctx->socktype, "only support tcp.");
    return _tcp_trysend(_sockctx_to_tcpol(psockctx));
}
int32_t sock_sendto(struct sock_ctx *psockctx, void *pdata, const size_t uilens,
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
    struct overlap_udp_ctx *pudp = _sockctx_to_udpol(psockctx);
    irtn = buffer_piece_inser(&pudp->sockctx.sendbuf, &pudp->sendtool.piece, paddr, pdata, uilens);
    if (ERR_OK != irtn)
    {
        SAFE_FREE(paddr);
        return irtn;
    }
    if (!ATOMIC_CAS(&pudp->sockctx.sendref, 0, 1))
    {
        return ERR_OK;
    }
    irtn = _post_sendto(pudp);
    ATOMIC_ADD(&pudp->sockctx.sendref, -1);
    if (ERR_OK != irtn)
    {
        return irtn;
    }

    return ERR_OK;
}

#endif
