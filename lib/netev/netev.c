#include "netev/netev.h"
#include "netutils.h"

#ifdef NETEV_IOCP
    #define NOTIFI_EXIT_KEY  ((ULONG_PTR)-1)
#else
    #define _ININT_EVENT 512
    #define _CMD_STOP    0x01
    #define _CMD_ADD     0x02
    #define _CMD_DEL     0x03
    #define _CMD_CLOSE   0x04
#endif
int32_t _netev_threadcnt(const uint32_t uthcnt)
{
    return uthcnt > 0 ? uthcnt : (int32_t)procscnt() * 2;
}
struct watcher_ctx *_netev_get_watcher(struct netev_ctx *pctx, SOCKET fd)
{
#if defined(NETEV_IOCP)
    return &pctx->watcher[0];
#else
    if (1 == pctx->thcnt)
    {
        return &pctx->watcher[0];
    }
    return &pctx->watcher[fnv1a_hash((const char *)&fd, sizeof(fd)) % pctx->thcnt];
#endif
}
static inline struct netev_ctx *_new_pub_ev(struct tw_ctx *ptw, const uint32_t uthcnt)
{
    struct netev_ctx *pev = MALLOC(sizeof(struct netev_ctx));
    ASSERTAB(NULL != pev, ERRSTR_MEMORY);
    pev->thcnt = _netev_threadcnt(uthcnt);
    pev->tw = ptw;
    pev->id = 1;
    pev->watcher = MALLOC(sizeof(struct watcher_ctx) * pev->thcnt);
    ASSERTAB(NULL != pev->watcher, ERRSTR_MEMORY);
    return pev;
}
static inline void _free_pub_ev(struct netev_ctx *pctx)
{
    FREE(pctx->watcher);
    FREE(pctx);
}
#ifdef NETEV_IOCP
static inline void *_ex_func(SOCKET fd, GUID  *pguid)
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
static inline void _iocp_init_watcher(struct watcher_ctx *pwatcher, HANDLE piocp,
    void *acpex, void *connex, void *disconnex)
{
    thread_init(&pwatcher->thev);
    pwatcher->iocp = piocp;
    pwatcher->acceptex = acpex;
    pwatcher->connectex = connex;
    pwatcher->disconnectex = disconnex;
}
#else
#ifdef NETEV_EPOLL
static inline void _set_cloexec(int32_t fd)
{
    int32_t iflag = fcntl(fd, F_GETFD);
    if (ERR_FAILED == iflag)
    {
        LOG_WARN("fcntl(%d, F_GETFD) failed.", fd);
        return;
    }
    if (!(iflag & FD_CLOEXEC))
    {
        if (ERR_FAILED == fcntl(fd, F_SETFD, iflag | FD_CLOEXEC))
        {
            LOG_WARN("fcntl(%d, F_SETFD, FD_CLOEXEC) failed.", fd);
        }
    }
}
#endif
static inline void _uev_init_watcher(struct watcher_ctx *pwatcher)
{
#if defined(NETEV_EPOLL)
    pwatcher->evfd = epoll_create(ONEK);
    ASSERTAB(ERR_FAILED != pwatcher->evfd, ERRORSTR(ERRNO));
    _set_cloexec(pwatcher->evfd);
#elif defined(NETEV_KQUEUE)
    pwatcher->evfd = kqueue();
    ASSERTAB(ERR_FAILED != pwatcher->evfd, ERRORSTR(ERRNO));
#elif defined(NETEV_EVPORT)
    pwatcher->evfd = port_create();
    ASSERTAB(ERR_FAILED != pwatcher->evfd, ERRORSTR(ERRNO));
#endif
    pwatcher->event_cnt = _ININT_EVENT;
    pwatcher->events = MALLOC(sizeof(events_t) * pwatcher->event_cnt);
    ASSERTAB(NULL != pwatcher->events, ERRSTR_MEMORY);
    ASSERTAB(ERR_OK == sockpair(pwatcher->socks), "init socket pair failed.");
    mutex_init(&pwatcher->lock_qucmd);
    thread_init(&pwatcher->thev);
    queue_init(&pwatcher->qu_close, ONEK);
    queue_init(&pwatcher->qu_cmd, ONEK);
}
static inline void _uev_free_watcher(struct watcher_ctx *pwatcher)
{
    SOCK_CLOSE(pwatcher->socks[0]);
    SOCK_CLOSE(pwatcher->socks[1]);
    mutex_free(&pwatcher->lock_qucmd);
    (void)close(pwatcher->evfd);
    queue_free(&pwatcher->qu_close);
    queue_free(&pwatcher->qu_cmd);
    FREE(pwatcher->events);
}
static inline void _uev_resize_events(struct watcher_ctx *pwatcher)
{
    events_t *pnew = MALLOC(sizeof(events_t) * (pwatcher->event_cnt * 2));
    if (NULL != pnew)
    {
        FREE(pwatcher->events);
        pwatcher->events = pnew;
        pwatcher->event_cnt *= 2;
    }
};
static char trigger[1] = { '1' };
static inline void _uev_cmd(struct watcher_ctx *pwatcher, int32_t icmd, int32_t ievents, struct sock_ctx *psock)
{
    struct message_ctx msg;
    msg.flags = icmd;
    msg.idata = ievents;
    msg.pdata = psock;
    mutex_lock(&pwatcher->lock_qucmd);
    queue_expand(&pwatcher->qu_cmd);
    (void)queue_push(&pwatcher->qu_cmd, &msg);
    (void)send(pwatcher->socks[1], trigger, (int32_t)sizeof(trigger), 0);
    mutex_unlock(&pwatcher->lock_qucmd);
}
int32_t _uev_add(struct watcher_ctx *pwatcher, struct sock_ctx *psock, int32_t iev)
{
#if defined(NETEV_EPOLL)
    struct epoll_event epev;
    ZERO(&epev, sizeof(epev));
    epev.data.ptr = psock;
    iev |= psock->events;
    if (iev & EV_READ)
    {
        epev.events |= EPOLLIN;
    }
    if (iev & EV_WRITE)
    {
        epev.events |= EPOLLOUT;
    }
    if (ERR_FAILED == epoll_ctl(pwatcher->evfd,
        (0 == psock->events ? EPOLL_CTL_ADD : EPOLL_CTL_MOD), psock->sock, &epev))
    {
        return ERR_FAILED;
    }
    psock->events = iev;
#elif defined(NETEV_KQUEUE)
    if ((iev & EV_READ)
        && !(psock->events & EV_READ))
    {
        psock->events |= EV_READ;
        struct kevent kev;
        EV_SET(&kev, psock->sock, EVFILT_READ, EV_ADD, 0, 0, psock);
        if (ERR_FAILED == kevent(pwatcher->evfd, &kev, 1, NULL, 0, NULL)
            || 0 != (kev.flags & EV_ERROR))
        {
            return ERR_FAILED;
        }
    }
    if ((iev & EV_WRITE)
        && !(psock->events & EV_WRITE))
    {
        psock->events |= EV_WRITE;
        struct kevent kev;
        EV_SET(&kev, psock->sock, EVFILT_WRITE, EV_ADD, 0, 0, psock);
        if (ERR_FAILED == kevent(pwatcher->evfd, &kev, 1, NULL, 0, NULL)
            || 0 != (kev.flags & EV_ERROR))
        {
            return ERR_FAILED;
        }
    }
#elif defined(NETEV_EVPORT)
    psock->events |= iev;
    iev = 0;
    if (psock->events & EV_READ)
    {
        iev |= POLLIN;
    }
    if (psock->events & EV_WRITE)
    {
        iev |= POLLOUT;
    }
    if (ERR_FAILED == port_associate(pwatcher->evfd, PORT_SOURCE_FD, psock->sock, iev, psock))
    {
        return ERR_FAILED;
    }
#endif
    return ERR_OK;
}
void _uev_del(struct watcher_ctx *pwatcher, struct sock_ctx *psock, int32_t iev)
{
    if (0 == iev)
    {
        return;
    }
#if defined(NETEV_EPOLL)
    struct epoll_event epev;
    ZERO(&epev, sizeof(epev));
    epev.data.ptr = psock;
    psock->events = psock->events & ~iev;
    if (0 == psock->events)
    {
        (void)epoll_ctl(pwatcher->evfd, EPOLL_CTL_DEL, psock->sock, &epev);
    }
    else
    {
        if (psock->events & EV_READ)
        {
            epev.events |= EPOLLIN;
        }
        if (psock->events & EV_WRITE)
        {
            epev.events |= EPOLLOUT;
        }
        (void)epoll_ctl(pwatcher->evfd, EPOLL_CTL_MOD, psock->sock, &epev);
    }
#elif defined(NETEV_KQUEUE)
    if ((iev & EV_READ)
        && (psock->events & EV_READ))
    {
        psock->events = psock->events & ~EV_READ;
        struct kevent kev;
        EV_SET(&kev, psock->sock, EVFILT_READ, EV_DELETE, 0, 0, psock);
        (void)kevent(pwatcher->evfd, &kev, 1, NULL, 0, NULL);
    }
    if ((iev & EV_WRITE)
        && (psock->events & EV_WRITE))
    {
        psock->events = psock->events & ~EV_WRITE;
        struct kevent kev;
        EV_SET(&kev, psock->sock, EVFILT_WRITE, EV_DELETE, 0, 0, psock);
        (void)kevent(pwatcher->evfd, &kev, 1, NULL, 0, NULL);
    }
#elif defined(NETEV_EVPORT)
    psock->events = psock->events & ~iev;
    if (0 == psock->events)
    {
        (void)port_dissociate(pwatcher->evfd, PORT_SOURCE_FD, psock->sock);
    }
    else
    {
        iev = 0;
        if (psock->events & EV_READ)
        {
            iev |= POLLIN;
        }
        if (psock->events & EV_WRITE)
        {
            iev |= POLLOUT;
        }
        (void)port_associate(pwatcher->evfd, PORT_SOURCE_FD, psock->sock, iev, psock);
    }
#endif
}
void _uev_cmd_close(struct watcher_ctx *pwatcher, struct sock_ctx *psock)
{
    _uev_cmd(pwatcher, _CMD_CLOSE, 0, psock);
}
void _add_close_qu(struct watcher_ctx *pwatcher, struct sock_ctx *psock)
{
    if (psock->flags & _FLAGS_CLOSE)
    {
        return;
    }

    psock->flags |= _FLAGS_CLOSE;
    struct message_ctx msg;
    msg.pdata = psock;
    _uev_del(pwatcher, psock, EV_READ | EV_WRITE);    
    queue_expand(&pwatcher->qu_close);
    (void)queue_push(&pwatcher->qu_close, &msg);
}
static inline void _cmd_cb(struct watcher_ctx *pwatcher, struct sock_ctx *psock, int32_t iev, int32_t *pstop)
{
    (void)read(psock->sock, pwatcher->trigger, sizeof(pwatcher->trigger));
    int32_t irtn;
    struct message_ctx msg;
    while (1)
    {
        mutex_lock(&pwatcher->lock_qucmd);
        irtn = queue_pop(&pwatcher->qu_cmd, &msg);
        mutex_unlock(&pwatcher->lock_qucmd);
        if (ERR_OK != irtn)
        {
            break;
        }
        switch (msg.flags)
        {
        case _CMD_STOP:
            *pstop = 1;
            break;
        case _CMD_ADD:
            if (ERR_OK != _uev_add(pwatcher, msg.pdata, msg.idata))
            {
                _add_close_qu(pwatcher, msg.pdata);
            }
            break;
        case _CMD_DEL:
            _uev_del(pwatcher, msg.pdata, msg.idata);
            break;
        case _CMD_CLOSE://�����ر�
            _add_close_qu(pwatcher, msg.pdata);
            break;
        }
    }
#ifdef NETEV_EVPORT
    if (0 == *pstop)
    {
        ASSERTAB(ERR_OK == _uev_add(pwatcher, psock, psock->events), "_uev_add failed.");
    }
#endif
}
#endif
struct netev_ctx *netev_new(struct tw_ctx *ptw, const uint32_t uthcnt)
{
    struct netev_ctx *pev = _new_pub_ev(ptw, uthcnt);
#ifdef NETEV_IOCP
    WSADATA wsdata;
    WORD ver = MAKEWORD(2, 2);
    ASSERTAB(ERR_OK == WSAStartup(ver, &wsdata), ERRORSTR(ERRNO));

    HANDLE piocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, pev->thcnt);
    ASSERTAB(NULL != piocp, ERRORSTR(ERRNO));
    GUID accept_uid = WSAID_ACCEPTEX;
    GUID connect_uid = WSAID_CONNECTEX;
    GUID disconnect_uid = WSAID_DISCONNECTEX;
    SOCKET sock = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, 0);
    ASSERTAB(INVALID_SOCK != sock, ERRORSTR(ERRNO));
    void *acpex = _ex_func(sock, &accept_uid);
    void *connex = _ex_func(sock, &connect_uid);
    void *disconnex = _ex_func(sock, &disconnect_uid);
    SOCK_CLOSE(sock);
#endif
    struct watcher_ctx *pwatcher;
    for (int32_t i = 0; i < pev->thcnt; i++)
    {
        pwatcher = &pev->watcher[i];
        pwatcher->netev = pev;
#ifdef NETEV_IOCP
        _iocp_init_watcher(pwatcher, piocp, acpex, connex, disconnex);
#else
        _uev_init_watcher(pwatcher);
#endif
    }
    return pev;
}
void netev_free(struct netev_ctx *pctx)
{
    int32_t i;
    for (i = 0; i < pctx->thcnt; i++)
    {
#ifdef NETEV_IOCP
        if (!PostQueuedCompletionStatus(pctx->watcher[i].iocp, 0, NOTIFI_EXIT_KEY, NULL))
        {
            LOG_ERROR("%s", ERRORSTR(ERRNO));
        }
#else
        _uev_cmd(&pctx->watcher[i], _CMD_STOP, 0, NULL);
#endif
    }
    for (i = 0; i < pctx->thcnt; i++)
    {
        thread_join(&pctx->watcher[i].thev);
#ifndef NETEV_IOCP
        _uev_free_watcher(&pctx->watcher[i]);
#endif
    }
#ifdef NETEV_IOCP
    (void)CloseHandle(pctx->watcher[0].iocp);
#endif
    _free_pub_ev(pctx);
#ifdef NETEV_IOCP
    (void)WSACleanup();
#endif
}
void _netev_add(struct watcher_ctx *pwatcher, struct sock_ctx *psock, int32_t iev)
{
#if defined(NETEV_IOCP)
    if (0 == psock->events
        && 0 != iev)
    {
        if (NULL == CreateIoCompletionPort((HANDLE)psock->sock, pwatcher->iocp, 0, pwatcher->netev->thcnt))
        {
            LOG_ERROR("%s", ERRORSTR(ERRNO));
        }
    }
    psock->events |= iev;
#else
    _uev_cmd(pwatcher, _CMD_ADD, iev, psock);
#endif
}
#ifndef NETEV_IOCP
static inline int32_t _trans_ev(events_t *pev, struct sock_ctx **psock)
{
    int32_t iev = 0;
#if defined(NETEV_EPOLL)
    if (pev->events & (EPOLLIN | EPOLLHUP | EPOLLERR))
    {
        iev |= EV_READ;
    }
    if (pev->events & (EPOLLOUT | EPOLLHUP | EPOLLERR))
    {
        iev |= EV_WRITE;
    }
    *psock = pev->data.ptr;
#elif defined(NETEV_KQUEUE)
    if (EVFILT_READ == pev->filter)
    {
        iev |= EV_READ;
    }
    if (EVFILT_WRITE == pev->filter)
    {
        iev |= EV_WRITE;
    }
    *psock = pev->udata;
#elif defined(NETEV_EVPORT)
    if (pev->portev_events & POLLIN)
    {
        iev |= EV_READ;
    }
    if (pev->portev_events & POLLOUT)
    {
        iev |= EV_WRITE;
    }
    *psock = pev->portev_user;
#endif
    return iev;
}
#endif
static inline void _netev_wait(struct watcher_ctx *pwatcher, int32_t *pstop)
{
#if defined(NETEV_IOCP)
    BOOL brtn;
    ULONG_PTR ulkey;
    OVERLAPPED *poverlap;
    brtn = GetQueuedCompletionStatus(pwatcher->iocp,
        &pwatcher->bytes,
        &ulkey,
        &poverlap,
        INFINITE);
    if (NOTIFI_EXIT_KEY == ulkey)
    {
        *pstop = 1;
        return;
    }
    if (NULL == poverlap)
    {
        return;
    }
    pwatcher->err = ERR_OK;
    if (!brtn)
    {
        pwatcher->err = ERRNO;
    }
    struct sock_ctx *psock = UPCAST(poverlap, struct sock_ctx, overlapped);
    psock->ev_cb(pwatcher, psock, psock->events, pstop);
#else
    int32_t icnt;
#if defined(NETEV_EPOLL)
    icnt = epoll_wait(pwatcher->evfd, pwatcher->events, pwatcher->event_cnt, -1);
#elif defined(NETEV_KQUEUE)
    icnt = kevent(pwatcher->evfd, NULL, 0, pwatcher->events, pwatcher->event_cnt, NULL);
#elif defined(NETEV_EVPORT)
    uint32_t uicnt = 1;
    (void)port_getn(pwatcher->evfd, pwatcher->events, pwatcher->event_cnt, &uicnt, NULL);
    icnt = (int32_t)uicnt;
#endif
    int32_t iev;
    struct sock_ctx *psock;
    for (int32_t i = 0; i < icnt; i++)
    {
        iev = _trans_ev(&pwatcher->events[i], &psock);
        psock->ev_cb(pwatcher, psock, iev, pstop);
    }
    struct message_ctx msg;
    while (ERR_OK == queue_pop(&pwatcher->qu_close, &msg))
    {
        _uev_sock_close(msg.pdata);
    }
    if (0 == *pstop
        && icnt >= pwatcher->event_cnt)
    {
        _uev_resize_events(pwatcher);
    }
#endif
}
static void _loop(void *param)
{
    int32_t istop = 0;
    struct watcher_ctx *pwatcher = param;
#ifndef NETEV_IOCP
    struct sock_ctx sock;
    sock.sock = pwatcher->socks[0];
    sock.ev_cb = _cmd_cb;
    sock.events = 0;
    sock.id = 0;
    ASSERTAB(ERR_OK == _uev_add(pwatcher, &sock, EV_READ), "_uev_add failed.");
#endif
    while (0 == istop)
    {
        _netev_wait(pwatcher, &istop);
    }
}
void netev_loop(struct netev_ctx *pctx)
{
    for (int32_t i = 0; i < pctx->thcnt; i++)
    {
        thread_creat(&pctx->watcher[i].thev, _loop, &pctx->watcher[i]);
        thread_waitstart(&pctx->watcher[i].thev);
    }
}
uint32_t sock_id(struct sock_ctx *psock)
{
    return psock->id;
}
SOCKET sock_handle(struct sock_ctx *psock)
{
    return psock->sock;
}
SOCKET sock_listen(struct netaddr_ctx *paddr)
{
    SOCKET sock = sock_create(netaddr_family(paddr), SOCK_STREAM);
    if (INVALID_SOCK == sock)
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return INVALID_SOCK;
    }
    socknbio(sock);
    sockraddr(sock);
    if (ERR_OK == checkrport())
    {
        sockrport(sock);
    }
    if (ERR_OK != bind(sock, netaddr_addr(paddr), netaddr_size(paddr)))
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        SOCK_CLOSE(sock);
        return INVALID_SOCK;
    }
    if (ERR_OK != listen(sock, SOCKK_BACKLOG))
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        SOCK_CLOSE(sock);
        return INVALID_SOCK;
    }

    return sock;
}
SOCKET sock_udp_bind(const char *phost, const uint16_t usport)
{
    struct netaddr_ctx addr;
    if (ERR_OK != netaddr_sethost(&addr, phost, usport))
    {
        return INVALID_SOCK;
    }
    SOCKET sock = sock_create(netaddr_family(&addr), SOCK_DGRAM);
    if (INVALID_SOCK == sock)
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return INVALID_SOCK;
    }
    sockraddr(sock);
    if (ERR_OK != bind(sock, netaddr_addr(&addr), netaddr_size(&addr)))
    {
        SOCK_CLOSE(sock);
        return INVALID_SOCK;
    }

    return sock;
}