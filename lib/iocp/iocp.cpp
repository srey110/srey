#include "iocp/iocp.h"
#include "chainbuffer.h"
#include "utils.h"
#include "evtype.h"
#include "netutils.h"

SREY_NS_BEGIN
#ifdef OS_WIN

#define NOTIFIEXIT_KEY ((ULONG_PTR)-1)
typedef BOOL(WINAPI *AcceptExPtr)(SOCKET, SOCKET, PVOID, DWORD, DWORD, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL(WINAPI *ConnectExPtr)(SOCKET, const struct sockaddr *, int, PVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef void (WINAPI *GetAcceptExSockaddrsPtr)(PVOID, DWORD, DWORD, DWORD, LPSOCKADDR *, LPINT, LPSOCKADDR *, LPINT);
struct ExPtr
{
    AcceptExPtr acceptEx;
    ConnectExPtr connectEx;
    GetAcceptExSockaddrsPtr getAcceptExSockaddrs;
};

struct sock_ol
{
    OVERLAPPED overlapped;
    EVTYPE evtype;
    struct sockctx *psockctx;
};
struct accept_ol : sock_ol
{
    DWORD bytes;
    char iobuffer[(sizeof(sockaddr_storage) + 16) * 2];
    SOCKET sock;
};
struct connect_ol : sock_ol
{
    DWORD bytes;
};
struct recv_ol : sock_ol
{
    WSABUF wsabuf;
    DWORD bytes;
    DWORD flag;
    char iobuffer[ONEK * 4];
};
struct recvfrom_ol : recv_ol
{
    sockaddr_storage  rmtaddr;  //存储数据来源IP地址
    int32_t addrlen;            //存储数据来源IP地址长度
};
struct send_ol : sock_ol
{
    char iobuffer[1];
    struct sockctx *psockctx;
};

class cworker : public ctask
{
public:
    cworker() {};
    ~cworker() {};
    void run()
    {
        BOOL bok;
        DWORD dbytes;
        int32_t ierr;
        ULONG_PTR ulkey;
        struct sock_ol *psockol;
        OVERLAPPED *poverlapped;
        while (!isstop())
        {
            ierr = ERR_OK;
            bok = GetQueuedCompletionStatus(m_ioport, &dbytes, &ulkey, &poverlapped, INFINITE);
            if (NOTIFIEXIT_KEY == ulkey)
            {
                PRINTF("%s", "NOTIFIEXIT_KEY");
                break;
            }
            if (!bok)
            {
                ierr = ERRNO;
                if (NULL == poverlapped)
                {
                    continue;
                }
            }

            psockol = CONTAINING_RECORD(poverlapped, sock_ol, overlapped);
            switch (psockol->evtype)
            {
                case EV_ACCEPT:
                {
                    _accept(ierr, dbytes, psockol);
                }
                break;
                case EV_CONNECT:
                {
                    _connect(ierr, dbytes, psockol);
                }
                break;
                case EV_READ:
                {//udp  tcp INIT_NUMBER == dbytes

                }
                break;
                case EV_WRITE:
                {//udp  tcp INIT_NUMBER == dbytes

                }
                break;
            }
        }
        PRINTF("%s", "222222222222222222222222");
    };
    void setparam(HANDLE ioport, class ciocp *piocp)
    {
        m_ioport = ioport;
        m_iocp = piocp;
    };
private:
    void _notifie(struct sockctx *pctx , const EVTYPE &emtype, const int32_t &ierr)
    {
        struct event *pev = new(std::nothrow) struct event();
        ASSERTAB(NULL != pev, ERRSTR_MEMORY);
        pev->code = ierr;
        pev->data = pctx;
        pev->evtype = emtype;

        if (!pctx->chansend((void *)pev))
        {
            SAFE_DEL(pev);
            PRINTF("%s", "chan send failed.");
        }
    };
    void _accept(const int32_t &ierr, const DWORD  &dbytes,struct sock_ol *psockol)
    {
        struct accept_ol *paccol = (struct accept_ol *)psockol;
        struct sockaddr *plocal = NULL, *premote = NULL;
        int32_t ilocal = INIT_NUMBER, iremote = INIT_NUMBER, irtn;
        SOCKET sock = paccol->sock;

        m_iocp->getexfunc()->getAcceptExSockaddrs(
            paccol->iobuffer, 0, 
            sizeof(paccol->iobuffer) / 2, sizeof(paccol->iobuffer) / 2,
            &plocal, &ilocal, &premote, &iremote); 
        //设置，让getsockname, getpeername, shutdown可用
        (void)setsockopt(sock, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
            (char *)&paccol->psockctx->sock, sizeof(&paccol->psockctx->sock)); 
        sockkpa(sock, SOCKKPA_DELAY, SOCKKPA_INTVL);
        //新加一个socket，继续accept
        irtn = m_iocp->_accptex(paccol->psockctx, paccol);
        if (ERR_OK != irtn)
        {
            SAFE_DEL(psockol);
            return;
        }

        struct sockctx *pctx = new(std::nothrow) struct sockctx();
        ASSERTAB(NULL != pctx, ERRSTR_MEMORY);
        pctx->addr.setaddr(premote);
        pctx->chan = paccol->psockctx->chan;
        pctx->ipproto = paccol->psockctx->ipproto;
        pctx->sock = sock;
        pctx->socktype = paccol->psockctx->socktype;
        struct recv_ol *poverlapped = new(std::nothrow) struct recv_ol();
        ASSERTAB(NULL != poverlapped, ERRSTR_MEMORY);
        irtn = m_iocp->_recvex(pctx, poverlapped);
        if (ERR_OK != irtn)
        {
            SAFE_DEL(pctx);
            SAFE_DEL(poverlapped);
            PRINTF("%s", ERRORSTR(irtn));
            return;
        }
    };
    void _connect(const int32_t &ierr, const DWORD  &dbytes, struct sock_ol *psockol)
    {
        if (ERR_OK != ierr)
        {
            _notifie(psockol->psockctx, EV_CONNECT, ierr);
            SAFE_DEL(psockol);
            return;
        }
        
        struct recv_ol *poverlapped = new(std::nothrow) struct recv_ol();
        ASSERTAB(NULL != poverlapped, ERRSTR_MEMORY);
        int32_t irtn = m_iocp->_recvex(psockol->psockctx, poverlapped);
        if (ERR_OK != irtn)
        {
            SAFE_DEL(poverlapped);
            PRINTF("%s", ERRORSTR(irtn));
            return;
        }
    };

private:
    HANDLE m_ioport;
    class ciocp *m_iocp; 
};

ciocp::ciocp()
{
    WSAData wsdata;
    WORD ver(MAKEWORD(2, 2));
    ASSERTAB(ERR_OK == WSAStartup(ver, &wsdata), ERRORSTR(ERRNO));

    m_threadnum = (int32_t)procsnum() * 2 + 2;
    m_exfunc = new(std::nothrow) struct ExPtr();
    ASSERTAB(NULL != m_exfunc, ERRSTR_MEMORY);
    m_thread = new(std::nothrow) class cthread[m_threadnum];
    ASSERTAB(NULL != m_thread, ERRSTR_MEMORY);
    m_worker = new(std::nothrow) class cworker[m_threadnum];
    ASSERTAB(NULL != m_worker, ERRSTR_MEMORY);
    _initexfunc();
}
ciocp::~ciocp()
{
    SAFE_DELARR(m_worker);
    SAFE_DELARR(m_thread);
    SAFE_DEL(m_exfunc);
    CloseHandle(m_ioport);
    (void)WSACleanup();
}
void ciocp::_initexfunc(void)
{
    const GUID acceptex = WSAID_ACCEPTEX;
    const GUID connectex = WSAID_CONNECTEX;
    const GUID getacceptexsockaddrs = WSAID_GETACCEPTEXSOCKADDRS;

    SOCKET sock = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, 0);
    ASSERTAB(INVALID_SOCK != sock, ERRORSTR(ERRNO));
    m_exfunc->acceptEx = (AcceptExPtr)_getexfunc(sock, &acceptex);
    m_exfunc->connectEx = (ConnectExPtr)_getexfunc(sock, &connectex);
    m_exfunc->getAcceptExSockaddrs = (GetAcceptExSockaddrsPtr)_getexfunc(sock, &getacceptexsockaddrs);

    SAFE_CLOSESOCK(sock);
}
void *ciocp::_getexfunc(const SOCKET &fd, const GUID  *guid)
{
    void *pfunc = NULL;
    DWORD dbytes = INIT_NUMBER;
    int32_t irtn = WSAIoctl(fd,
        SIO_GET_EXTENSION_FUNCTION_POINTER,
        (GUID*)guid, sizeof(*guid),
        &pfunc, sizeof(pfunc),
        &dbytes, NULL, NULL);
    ASSERTAB(irtn != SOCKET_ERROR, ERRORSTR(ERRNO));

    return pfunc;
}
bool ciocp::start()
{
    m_ioport = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, NULL, m_threadnum);
    if (NULL == m_ioport)
    {
        PRINTF("%s", ERRORSTR(ERRNO));
        return false;
    }

    class cworker *pworker;
    for (int32_t i = INIT_NUMBER; i < m_threadnum; i++)
    {
        pworker = &m_worker[i];
        pworker->setparam(m_ioport, this);
        m_thread[i].creat(pworker);
    }

    return true;
}
void ciocp::stop()
{
    int32_t i;
    for (i = INIT_NUMBER; i < m_threadnum; i++)
    {
        m_worker[i].stop();
        if (!PostQueuedCompletionStatus(m_ioport, 0, NOTIFIEXIT_KEY, NULL))
        {
            PRINTF("%s", ERRORSTR(ERRNO));
        }
    }
    for (i = INIT_NUMBER; i < m_threadnum; i++)
    {
        m_thread[i].join();
    }
}
struct sockctx *ciocp::_creatctx(class cchan *pchan,
    const char *phost, const uint16_t &usport, const bool &btcp)
{
    struct sockctx *pctx = new(std::nothrow) struct sockctx();
    ASSERTAB(NULL != pctx, ERRSTR_MEMORY);

    if (!pctx->addr.setaddr(phost, usport))
    {
        SAFE_DEL(pctx);
        return NULL;
    }
    pctx->chan = pchan;
    pctx->socktype = btcp ? SOCK_STREAM : SOCK_DGRAM;
    pctx->ipproto = btcp ? IPPROTO_TCP : IPPROTO_UDP;
    pctx->sock = WSASocket(pctx->addr.addrfamily(), pctx->socktype,
        pctx->ipproto, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (INVALID_SOCK == pctx->sock)
    {
        PRINTF("%s", ERRORSTR(ERRNO));
        SAFE_DEL(pctx);
        return NULL;
    }

    return pctx;
}
struct sockctx *ciocp::listener(class cchan *pchan,
    const char *phost, const uint16_t &usport, const bool &btcp)
{
    struct sockctx *pctx = _creatctx(pchan, phost, usport, btcp);
    if (NULL == pctx)
    {
        return NULL;
    }
    
    int32_t irtn = _listener(pctx);
    if (ERR_OK != irtn)
    {
        PRINTF("%s", ERRORSTR(irtn));
        SAFE_DEL(pctx);
        return NULL;
    }

    return pctx;
}
int32_t ciocp::_listener(struct sockctx *psock)
{
    sockraddr(psock->sock);
    if (ERR_OK != bind(psock->sock, psock->addr.getaddr(), psock->addr.getsize()))
    {
        return ERRNO;
    }
    //UDP
    if (SOCK_DGRAM == psock->socktype)
    {
        return _udpex(psock);
    }
    //TCP
    if (ERR_OK != listen(psock->sock, SOCKK_BACKLOG))
    {
        return ERRNO;
    }

    return _accptex(psock);
}
int32_t ciocp::_udpex(struct sockctx *psock)
{
    if (NULL == CreateIoCompletionPort((HANDLE)psock->sock, m_ioport,
        (ULONG_PTR)psock, m_threadnum))
    {
        return ERRNO;
    }

    struct recvfrom_ol *poverlapped = new(std::nothrow)struct recvfrom_ol();
    ASSERTAB(NULL != poverlapped, ERRSTR_MEMORY);
    ZERO(&(poverlapped->overlapped), sizeof(poverlapped->overlapped));
    poverlapped->evtype = EV_READ;
    poverlapped->psockctx = psock;
    poverlapped->wsabuf.len = psock->addr.getsize();
    poverlapped->wsabuf.buf = poverlapped->iobuffer;
    poverlapped->addrlen = sizeof(poverlapped->rmtaddr);
    poverlapped->flag = INIT_NUMBER;
    poverlapped->bytes = INIT_NUMBER;
    int32_t irtn = WSARecvFrom(
        psock->sock,
        &poverlapped->wsabuf,
        1,
        &poverlapped->bytes,
        &poverlapped->flag,
        (sockaddr*)&(poverlapped->rmtaddr),
        &(poverlapped->addrlen),
        &(poverlapped->overlapped),
        NULL);
    if (ERR_OK != irtn)
    {
        irtn = ERRNO;
        if (WSA_IO_PENDING != irtn)
        {
            SAFE_DEL(poverlapped);
            return irtn;
        }
    }

    return ERR_OK;
}
int32_t ciocp::_accptex(struct sockctx *psock)
{
    if (NULL == CreateIoCompletionPort((HANDLE)psock->sock, m_ioport,
        (ULONG_PTR)psock, m_threadnum))
    {
        return ERRNO;
    }

    int32_t irtn;
    struct accept_ol *poverlapped;
    std::vector<struct accept_ol *> vcsuss;
    for (int32_t i = INIT_NUMBER; i < SOCKK_BACKLOG; i++)
    {
        poverlapped = new(std::nothrow) struct accept_ol();
        ASSERTAB(NULL != poverlapped, ERRSTR_MEMORY);

        irtn = _accptex(psock, poverlapped);
        if (ERR_OK != irtn)
        {
            _freeaccptex(vcsuss);
            return irtn;
        }

        vcsuss.push_back(poverlapped);
    }

    return ERR_OK;
}
void ciocp::_freeaccptex(std::vector<struct accept_ol *> &vcsuss)
{
    for (std::vector<struct accept_ol *>::iterator it = vcsuss.begin(); it < vcsuss.end(); it++)
    {
        SAFE_CLOSESOCK((*it)->sock);
        SAFE_DEL(*it);
    }
}
int32_t ciocp::_accptex(struct sockctx *plistensock, struct accept_ol *poverlapped)
{
    SOCKET sock = WSASocket(plistensock->addr.addrfamily(), plistensock->socktype,
        plistensock->ipproto, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (INVALID_SOCK == sock)
    {
        return ERRNO;
    }

    ZERO(&poverlapped->overlapped, sizeof(poverlapped->overlapped));
    poverlapped->evtype = EV_ACCEPT;
    poverlapped->sock = sock;
    poverlapped->psockctx = plistensock;
    poverlapped->bytes = INIT_NUMBER;

    if (!m_exfunc->acceptEx(plistensock->sock, //ListenSocket
        poverlapped->sock,                     //AcceptSocket
        &poverlapped->iobuffer,
        0,
        sizeof(poverlapped->iobuffer) / 2,
        sizeof(poverlapped->iobuffer) / 2,
        &poverlapped->bytes,
        &poverlapped->overlapped))
    {
        int32_t irtn = ERRNO;
        if (ERROR_IO_PENDING != irtn)
        {
            return irtn;
        }
    }

    return ERR_OK;
}
struct sockctx *ciocp::connectter(class cchan *pchan,
    const char *phost, const uint16_t &usport, const bool &btcp)
{
    struct sockctx *pctx = _creatctx(pchan, phost, usport, btcp);
    if (NULL == pctx)
    {
        return NULL;
    }

    int32_t irtn = _connectter(pctx);
    if (ERR_OK != irtn)
    {
        PRINTF("%s", ERRORSTR(irtn));
        SAFE_DEL(pctx);
        return NULL;
    }

    return pctx;
}
int32_t ciocp::_connectter(struct sockctx *psock)
{
    int32_t irtn = _trybind(psock);
    if (ERR_OK != irtn)
    {
        return irtn;
    }

    //UDP
    if (SOCK_DGRAM == psock->socktype)
    {
        (void)connect(psock->sock, psock->addr.getaddr(), psock->addr.getsize());
        return _udpex(psock);
    }

    return _tcpconnect(psock);
}
int32_t ciocp::_trybind(struct sockctx *psock)
{
    cnetaddr addr;
    if (psock->addr.isipv4())
    {
        if (!addr.setaddr("0.0.0.0", 0))
        {
            return ERR_FAILED;
        }
    }
    else
    {
        if (!addr.setaddr("0:0:0:0:0:0:0:0", 0))
        {
            return ERR_FAILED;
        }
    }
    if (ERR_OK != bind(psock->sock, addr.getaddr(), addr.getsize()))
    {
        return ERRNO;
    }

    return ERR_OK;
}
int32_t ciocp::_tcpconnect(struct sockctx *psock)
{
    if (NULL == CreateIoCompletionPort((HANDLE)psock->sock, m_ioport,
        (ULONG_PTR)psock, m_threadnum))
    {
        return ERRNO;
    }

    struct connect_ol *poverlapped = new(std::nothrow) struct connect_ol();
    ASSERTAB(NULL != poverlapped, ERRSTR_MEMORY);
    ZERO(&poverlapped->overlapped, sizeof(poverlapped->overlapped));
    poverlapped->bytes = EV_CONNECT;
    poverlapped->evtype = EV_CONNECT;
    poverlapped->psockctx = psock;

    if (!m_exfunc->connectEx(psock->sock,
        psock->addr.getaddr(),
        psock->addr.getsize(),
        NULL,
        0,
        &poverlapped->bytes,
        &poverlapped->overlapped))
    {
        int32_t irtn = ERRNO;
        if (ERROR_IO_PENDING != irtn)
        {
            SAFE_DEL(poverlapped);
            return irtn;
        }
    }

    return ERR_OK;
}
struct sockctx *ciocp::addsock(class cchan *pchan, SOCKET &fd)
{
    if (INVALID_SOCK == fd)
    {
        return NULL;
    }

    socknbio(fd);
    WSAPROTOCOL_INFO info;
    int32_t ilens = (int32_t)sizeof(info);
    if (getsockopt(fd, SOL_SOCKET, SO_PROTOCOL_INFO, (char *)&info, &ilens) < ERR_OK)
    {
        PRINTF("getsockopt(%d, SOL_SOCKET, SO_PROTOCOL_INFO, ...) failed. %s", (int32_t)fd, ERRORSTR(ERRNO));
        return NULL;
    }

    struct sockctx *pctx = new(std::nothrow) struct sockctx();
    ASSERTAB(NULL != pctx, ERRSTR_MEMORY);
    pctx->chan = pchan;
    pctx->sock = fd;
    pctx->socktype = info.iSocketType;
    pctx->ipproto = info.iProtocol;
    if (SOCK_STREAM == pctx->socktype)
    {
        sockkpa(fd, SOCKKPA_DELAY, SOCKKPA_INTVL);
    }
    
    int32_t irtn = _addsock(pctx);
    if (ERR_OK != irtn)
    {
        PRINTF("%s", ERRORSTR(irtn));
        SAFE_DEL(pctx);
        return NULL;
    }

    return pctx;
}
int32_t ciocp::_addsock(struct sockctx *psock)
{
    //UDP
    if (SOCK_DGRAM == psock->socktype)
    {
        return _udpex(psock);
    }    
    
    if (NULL == CreateIoCompletionPort((HANDLE)psock->sock, m_ioport,
        (ULONG_PTR)psock, m_threadnum))
    {
        return ERRNO;
    }

    struct recv_ol *poverlapped = new(std::nothrow) struct recv_ol();
    ASSERTAB(NULL != poverlapped, ERRSTR_MEMORY);

    return _recvex(psock, poverlapped);
}
int32_t ciocp::_recvex(struct sockctx *psock, struct recv_ol *poverlapped)
{
    ZERO(&poverlapped->overlapped, sizeof(poverlapped->overlapped));
    poverlapped->bytes = INIT_NUMBER;
    poverlapped->flag = INIT_NUMBER;
    poverlapped->evtype = EV_READ;
    poverlapped->psockctx = psock;
    poverlapped->wsabuf.buf = poverlapped->iobuffer;
    poverlapped->wsabuf.len = sizeof(poverlapped->iobuffer);
    int32_t irtn = WSARecv(psock->sock,
        &poverlapped->wsabuf,
        1,
        &poverlapped->bytes,
        &poverlapped->flag,
        &poverlapped->overlapped,
        NULL);
    if (ERR_OK != irtn)
    {
        irtn = ERRNO;
        if (WSA_IO_PENDING != irtn)
        {
            SAFE_DEL(poverlapped);
            return irtn;
        }
    }

    return ERR_OK;
}

#endif
SREY_NS_END
