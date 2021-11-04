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
typedef struct overlap_disconn_ctx
{
    struct overlap_ctx ol;//overlapped
    struct sock_ctx *sockctx;
}overlap_disconn_ctx;
typedef struct overlap_recv_ctx
{
    struct overlap_ctx ol;//overlapped
    struct sock_ctx *sockctx;
    uint32_t iovcnt;
    DWORD bytes;
    DWORD flag;
    IOV_TYPE wsabuf[MAX_RECV_IOV_COUNT];
}overlap_recv_ctx;
typedef struct overlap_send_ctx
{
    struct overlap_ctx ol;//overlapped
    struct sock_ctx *sockctx;
    uint32_t iovcnt;
    DWORD bytes;
    IOV_TYPE wsabuf[MAX_SEND_IOV_COUNT];
}overlap_send_ctx;
typedef struct sock_ctx
{
    int32_t freecnt;
    int32_t postsendev;
    volatile atomic_t closed;
    volatile atomic_t recvref;
    volatile atomic_t sendref;
    struct chan_ctx *chan;//EV_CLOSE  EV_RECV  EV_SEND
    struct netev_ctx *netev;
    SOCKET sock;
    struct buffer_ctx recvbuf;
    struct buffer_ctx sendbuf;    
    struct overlap_recv_ctx recvol;
    struct overlap_send_ctx sendol;
    mutex_ctx lock_changech;//更改chan时
    mutex_ctx lock_postsc;//EV_CLOSE投递后不再投递EV_SEND    
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
            SAFE_CLOSE_SOCK(pacpolctx->sock);
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
    struct overlap_disconn_ctx *pdisconol = UPCAST(polctx, struct overlap_disconn_ctx, ol);
    SAFE_CLOSE_SOCK(pdisconol->sockctx->sock);
    SAFE_FREE(pdisconol);
}
void sock_close(struct sock_ctx *psockctx)
{
    if (0 != ATOMIC_GET(&psockctx->closed))
    {
        return;
    }
    struct overlap_disconn_ctx *pdisconol =
        (struct overlap_disconn_ctx *)MALLOC(sizeof(struct overlap_disconn_ctx));
    if (NULL == pdisconol)
    {
        LOG_ERROR("%s", ERRSTR_MEMORY);
        SAFE_CLOSE_SOCK(psockctx->sock);
        return;
    }
    ZERO(&pdisconol->ol.overlapped, sizeof(pdisconol->ol.overlapped));
    pdisconol->ol.overlap_cb = _on_disconnectex;
    pdisconol->sockctx = psockctx;
    //对端会收到0字节
    if (!psockctx->netev->disconnectex(psockctx->sock, &pdisconol->ol.overlapped, 0, 0))
    {
        int32_t irtn = ERRNO;
        if (ERROR_IO_PENDING != irtn)
        {
            NET_ERROR(irtn, "error code:%d message:%s", irtn, ERRORSTR(irtn));
            SAFE_FREE(pdisconol);
            SAFE_CLOSE_SOCK(psockctx->sock);
            return;
        }
    }
}
static inline int32_t _post_recv(struct overlap_recv_ctx *poverlap)
{
    ZERO(&poverlap->ol.overlapped, sizeof(poverlap->ol.overlapped));
    poverlap->flag = 0;
    poverlap->bytes = 0;
    poverlap->iovcnt = buffer_write_iov_application(&poverlap->sockctx->recvbuf,
        MAX_RECV_IOV_SIZE, poverlap->wsabuf, MAX_RECV_IOV_COUNT);

    ATOMIC_ADD(&poverlap->sockctx->recvref, 1);
    int32_t irtn = WSARecv(poverlap->sockctx->sock,
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
            buffer_write_iov_commit(&poverlap->sockctx->recvbuf, 0,
                poverlap->wsabuf, poverlap->iovcnt);
            ATOMIC_ADD(&poverlap->sockctx->recvref, -1);
            return irtn;
        }
    }
    return ERR_OK;
}
static inline void _on_sock_close(struct sock_ctx *psockctx)
{
    if (!ATOMIC_CAS(&psockctx->closed, 0, 1))
    {
        return;
    }
    struct ev_sock_ctx *pev = ev_sock_close(psockctx->sock, psockctx);
    mutex_lock(&psockctx->lock_postsc);
    int32_t irtn = chan_send(_get_rsc_chan(psockctx), &pev->ev);
    mutex_unlock(&psockctx->lock_postsc);
    if (ERR_OK != irtn)
    {
        LOG_ERROR("%s", "post close event failed.");
        SAFE_FREE(pev);
        _sock_free(psockctx);
    }
}
static inline void _on_recv(struct netev_ctx *piocpctx, 
    struct overlap_ctx *polctx, const uint32_t uibyte, const int32_t ierr)
{
    struct overlap_recv_ctx *precvol = UPCAST(polctx, struct overlap_recv_ctx, ol);
    if (0 == uibyte
        || ERR_OK != ierr)
    {
        buffer_write_iov_commit(&precvol->sockctx->recvbuf, 0,
            precvol->wsabuf, precvol->iovcnt);
        _on_sock_close(precvol->sockctx);
        ATOMIC_ADD(&precvol->sockctx->recvref, -1);
        return;
    }

    buffer_write_iov_commit(&precvol->sockctx->recvbuf, (size_t)uibyte,
        precvol->wsabuf, precvol->iovcnt);
    //投递EV_RECV消息
    struct ev_sock_ctx *pev = ev_sock_recv(precvol->sockctx->sock, 
        (int32_t)uibyte, precvol->sockctx);
    if (ERR_OK != chan_send(_get_rsc_chan(precvol->sockctx), &pev->ev))
    {
        LOG_ERROR("%s", "post recv event failed.");
        SAFE_FREE(pev);
    }
    int32_t irtn = _post_recv(precvol);
    if (ERR_OK != irtn)
    {
        NET_ERROR(irtn, "error code:%d message:%s", irtn, ERRORSTR(irtn));
        _on_sock_close(precvol->sockctx);
    }
    ATOMIC_ADD(&precvol->sockctx->recvref, -1);
}
static inline int32_t _post_send(struct overlap_send_ctx *poverlap)
{
    if (0 != ATOMIC_GET(&poverlap->sockctx->closed))
    {
        return ERR_FAILED;
    }
    uint32_t uicnt = buffer_read_iov_application(&poverlap->sockctx->sendbuf,
        MAX_SEND_IOV_SIZE, poverlap->wsabuf, MAX_SEND_IOV_COUNT);
    if (0 == uicnt)
    {
        return ERR_FAILED;
    }
    ZERO(&poverlap->ol.overlapped, sizeof(poverlap->ol.overlapped));
    poverlap->iovcnt = uicnt;

    ATOMIC_ADD(&poverlap->sockctx->sendref, 1);
    int32_t irtn = WSASend(poverlap->sockctx->sock,
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
            buffer_read_iov_commit(&poverlap->sockctx->sendbuf, 0);
            NET_ERROR(irtn, "error code:%d message:%s", irtn, ERRORSTR(irtn));
            ATOMIC_ADD(&poverlap->sockctx->sendref, -1);
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
        buffer_read_iov_commit(&psendol->sockctx->sendbuf, 0);
        ATOMIC_ADD(&psendol->sockctx->sendref, -1);
        return;
    }

    buffer_read_iov_commit(&psendol->sockctx->sendbuf, uibyte);
    if (0 != psendol->sockctx->postsendev)
    {
        //投递EV_SEND消息
        mutex_lock(&psendol->sockctx->lock_postsc);
        if (0 == ATOMIC_GET(&psendol->sockctx->closed))
        {
            struct ev_sock_ctx *pev = ev_sock_send(psendol->sockctx->sock, 
                (int32_t)uibyte, psendol->sockctx);
            if (ERR_OK != chan_send(_get_rsc_chan(psendol->sockctx), &pev->ev))
            {
                LOG_ERROR("%s", "post send event failed.");
                SAFE_FREE(pev);
            }
        }
        mutex_unlock(&psendol->sockctx->lock_postsc);
    }
    (void)_post_send(psendol);
    ATOMIC_ADD(&psendol->sockctx->sendref, -1);
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
                SAFE_CLOSE_SOCK(plistener->overlap_acpt[i].sock);
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
        LOG_WARN("free sock_ctx use long time. sock %d sending:%d recving:%d",
            (int32_t)sock_handle(psockctx), uisendref, uirecvref); 
        if (0 != uisendref)
        {
            if (ERR_OK != _check_olresult(&psockctx->sendol.ol, psockctx->sock))
            {
                return ERR_FAILED;
            }
        }
        if (0 != uirecvref)
        {
            if (ERR_OK != _check_olresult(&psockctx->recvol.ol, psockctx->sock))
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
static inline struct sock_ctx *_sockctx_init(struct netev_ctx *piocpctx, SOCKET fd,
    struct chan_ctx *pchan, const int32_t ipostsendev)
{
    struct sock_ctx *psockctx = (struct sock_ctx *)MALLOC(sizeof(struct sock_ctx));
    if (NULL == psockctx)
    {
        LOG_ERROR("%s", ERRSTR_MEMORY);
        return NULL;
    }
    psockctx->sendol.ol.overlap_cb = _on_send;
    psockctx->sendol.sockctx = psockctx;
    psockctx->recvol.ol.overlap_cb = _on_recv;
    psockctx->recvol.sockctx = psockctx;

    psockctx->sock = fd;
    psockctx->chan = pchan;
    psockctx->postsendev = ipostsendev;
    psockctx->netev = piocpctx;
    psockctx->sendref = 0;
    psockctx->recvref = 0;
    psockctx->closed = 0;
    psockctx->freecnt = 0;    

    buffer_init(&psockctx->recvbuf);
    buffer_init(&psockctx->sendbuf);
    mutex_init(&psockctx->lock_changech);
    mutex_init(&psockctx->lock_postsc);

    return psockctx;
}
void _sock_free(struct sock_ctx *psockctx)
{
    SAFE_CLOSE_SOCK(psockctx->sock);
    buffer_free(&psockctx->recvbuf);
    buffer_free(&psockctx->sendbuf);
    mutex_free(&psockctx->lock_changech);
    mutex_free(&psockctx->lock_postsc);
    SAFE_FREE(psockctx);
}
struct sock_ctx *netev_enable_rw(struct netev_ctx *piocpctx, SOCKET fd,
    struct chan_ctx *pchan, const int32_t ipostsendev)
{
    struct sock_ctx *psockctx = _sockctx_init(piocpctx, fd, pchan, ipostsendev);
    if (NULL == psockctx)
    {
        SOCK_CLOSE(fd);
        return NULL;
    }

    socknodelay(fd);
    closereset(fd);
    sockkpa(fd, SOCKKPA_DELAY, SOCKKPA_INTVL);
    int32_t irtn = _post_recv(&psockctx->recvol);
    if (ERR_OK != irtn)
    {
        NET_ERROR(irtn, "error code:%d message:%s", irtn, ERRORSTR(irtn));
        SOCK_CLOSE(fd);
        _sock_free(psockctx);
        return NULL;
    }

    return psockctx;
}
int32_t _tcp_trysend(struct sock_ctx *psockctx)
{
    if (!ATOMIC_CAS(&psockctx->sendref, 0, 1))
    {
        return ERR_OK;
    }
    int32_t irtn = _post_send(&psockctx->sendol);
    ATOMIC_ADD(&psockctx->sendref, -1);
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
    ASSERTAB(ERR_OK == buffer_append(&psockctx->sendbuf, pdata, uilens), "buffer_append error.");
    return _tcp_trysend(psockctx);
}
int32_t sock_send_buf(struct sock_ctx *psockctx)
{
    if (0 != ATOMIC_GET(&psockctx->closed))
    {
        return ERR_FAILED;
    }
    return _tcp_trysend(psockctx);
}

#endif
