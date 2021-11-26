#include "netev/netev.h"
#include "netutils.h"

#ifdef NETEV_IOCP
    #define NOTIFI_EXIT_KEY  ((ULONG_PTR)-1)
#else
    #define _ININT_EVENT      512
    #define _CMD_STOP         0x01
    #define _CMD_ADD          0x02
    #define _CMD_DEL          0x03
    #define _CMD_CLOSE        0x04
    #define _CMD_CONN         0x05
    #define _CMD_CONN_TIMEOUT 0x06
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
static inline struct netev_ctx *_new_pub_ev(struct tw_ctx *ptw, const uint32_t uthcnt, sid_t(*id_creater)(void *), void *pid)
{
    struct netev_ctx *pev = MALLOC(sizeof(struct netev_ctx));
    ASSERTAB(NULL != pev, ERRSTR_MEMORY);
    pev->thcnt = _netev_threadcnt(uthcnt);
    pev->tw = ptw;
    pev->id_creater = id_creater;
    pev->id_data = pid;
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
static inline uint64_t _hash_conn_timeout(void *pval)
{
    struct ud_ctx *pud = pval;
    return fnv1a_hash((const char *)&pud->id, sizeof(pud->id));
}
static inline int32_t _compare_conn_timeout(void *pval1, void *pval2, void *pudata)
{
    return ((struct ud_ctx *)pval1)->id == ((struct ud_ctx *)pval2)->id ? ERR_OK : ERR_FAILED;
}
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
    ASSERTAB(ERR_OK == pipe(pwatcher->pipes), ERRORSTR(ERRNO));
    pwatcher->map = map_new(sizeof(struct ud_ctx), _hash_conn_timeout, _compare_conn_timeout, NULL);
    mutex_init(&pwatcher->lock_qucmd);
    thread_init(&pwatcher->thev);
    queue_init(&pwatcher->qu_close, ONEK * 2);
    queue_init(&pwatcher->qu_cmd, ONEK * 4);
}
static inline void _uev_free_watcher(struct watcher_ctx *pwatcher)
{
    close(pwatcher->pipes[0]);
    close(pwatcher->pipes[1]);
    map_free(pwatcher->map);
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
static inline void _uev_cmd(struct watcher_ctx *pwatcher, uint32_t uicmd, uint32_t uiev, struct sock_ctx *psock, sid_t uid)
{
    if (ERR_OK != _uev_add_ref_cmd(psock))
    {
        return;
    }

    struct message_ctx msg;
    msg.flags = uicmd;
    msg.session = uiev;
    msg.data = psock;
    msg.id = uid;
    mutex_lock(&pwatcher->lock_qucmd);
    queue_expand(&pwatcher->qu_cmd);
    (void)queue_push(&pwatcher->qu_cmd, &msg);
    mutex_unlock(&pwatcher->lock_qucmd);

    static char trigger[1] = { 's' };
    ASSERTAB(sizeof(trigger) == write(pwatcher->pipes[1], trigger, sizeof(trigger)), "cmd pipe write error.");
}
int32_t _uev_add(struct watcher_ctx *pwatcher, struct sock_ctx *psock, uint32_t uiev)
{
    if (psock->flags & _FLAGS_CLOSE)
    {
        return ERR_FAILED;
    }
#if defined(NETEV_EPOLL)
    struct epoll_event epev;
    ZERO(&epev, sizeof(epev));
    epev.data.ptr = psock;
    uiev |= psock->events;
    if (uiev & EV_READ)
    {
        epev.events |= EPOLLIN;
    }
    if (uiev & EV_WRITE)
    {
        epev.events |= EPOLLOUT;
    }
    if (ERR_FAILED == epoll_ctl(pwatcher->evfd,
        (0 == psock->events ? EPOLL_CTL_ADD : EPOLL_CTL_MOD), psock->sock, &epev))
    {
        return ERR_FAILED;
    }
    psock->events = uiev;
#elif defined(NETEV_KQUEUE)
    if ((uiev & EV_READ)
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
    if ((uiev & EV_WRITE)
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
    psock->events |= uiev;
    uiev = 0;
    if (psock->events & EV_READ)
    {
        uiev |= POLLIN;
    }
    if (psock->events & EV_WRITE)
    {
        uiev |= POLLOUT;
    }
    if (ERR_FAILED == port_associate(pwatcher->evfd, PORT_SOURCE_FD, psock->sock, uiev, psock))
    {
        return ERR_FAILED;
    }
#endif
    return ERR_OK;
}
void _uev_del(struct watcher_ctx *pwatcher, struct sock_ctx *psock, uint32_t uiev)
{
    if (0 == uiev
        || (psock->flags & _FLAGS_CLOSE))
    {
        return;
    }
#if defined(NETEV_EPOLL)
    struct epoll_event epev;
    ZERO(&epev, sizeof(epev));
    epev.data.ptr = psock;
    psock->events = psock->events & ~uiev;
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
    if ((uiev & EV_READ)
        && (psock->events & EV_READ))
    {
        psock->events = psock->events & ~EV_READ;
        struct kevent kev;
        EV_SET(&kev, psock->sock, EVFILT_READ, EV_DELETE, 0, 0, psock);
        (void)kevent(pwatcher->evfd, &kev, 1, NULL, 0, NULL);
    }
    if ((uiev & EV_WRITE)
        && (psock->events & EV_WRITE))
    {
        psock->events = psock->events & ~EV_WRITE;
        struct kevent kev;
        EV_SET(&kev, psock->sock, EVFILT_WRITE, EV_DELETE, 0, 0, psock);
        (void)kevent(pwatcher->evfd, &kev, 1, NULL, 0, NULL);
    }
#elif defined(NETEV_EVPORT)
    psock->events = psock->events & ~uiev;
    if (0 == psock->events)
    {
        (void)port_dissociate(pwatcher->evfd, PORT_SOURCE_FD, psock->sock);
    }
    else
    {
        uiev = 0;
        if (psock->events & EV_READ)
        {
            uiev |= POLLIN;
        }
        if (psock->events & EV_WRITE)
        {
            uiev |= POLLOUT;
        }
        (void)port_associate(pwatcher->evfd, PORT_SOURCE_FD, psock->sock, uiev, psock);
    }
#endif
}
void _uev_cmd_close(struct watcher_ctx *pwatcher, struct sock_ctx *psock)
{
    _uev_cmd(pwatcher, _CMD_CLOSE, 0, psock, 0);
}
void _uev_cmd_conn(struct watcher_ctx *pwatcher, struct sock_ctx *psock)
{
    _uev_cmd(pwatcher, _CMD_CONN, EV_WRITE, psock, 0);
}
void _uev_cmd_timeout(struct watcher_ctx *pwatcher, sid_t uid)
{
    _uev_cmd(pwatcher, _CMD_CONN_TIMEOUT, 0, NULL, uid);
}
void _add_close_qu(struct watcher_ctx *pwatcher, struct sock_ctx *psock)
{
    if (psock->flags & _FLAGS_CLOSE)
    {
        return;
    }
    psock->flags |= _FLAGS_CLOSE;
    struct message_ctx msg;
    msg.data = psock;
    _uev_del(pwatcher, psock, EV_READ | EV_WRITE);    
    queue_expand(&pwatcher->qu_close);
    (void)queue_push(&pwatcher->qu_close, &msg);
}
void _conn_timeout_add(struct watcher_ctx *pwatcher, struct sock_ctx *psock)
{
    struct ud_ctx ud;
    ud.id = psock->id;
    ud.handle = (uintptr_t)psock;
    _map_set(pwatcher->map, &ud);
}
struct sock_ctx * _conn_timeout_remove(struct watcher_ctx *pwatcher, sid_t uid)
{
    struct ud_ctx key;
    key.id = uid;
    struct ud_ctx val;
    int32_t irtn = _map_remove(pwatcher->map, &key, &val);
    return ERR_OK == irtn ? (struct sock_ctx *)val.handle : NULL;
}
static inline void _cmd_cb(struct watcher_ctx *pwatcher, struct sock_ctx *psock, uint32_t uiev, int32_t *pstop)
{
    struct message_ctx msg;
    struct sock_ctx *pusock;
    ssize_t iread = read(psock->sock, pwatcher->trigger, sizeof(pwatcher->trigger));
    for (ssize_t i = 0; i < iread; i++)
    {
        mutex_lock(&pwatcher->lock_qucmd);
        if (ERR_OK != queue_pop(&pwatcher->qu_cmd, &msg))
        {
            mutex_unlock(&pwatcher->lock_qucmd);
            break;
        }
        mutex_unlock(&pwatcher->lock_qucmd);

        pusock = msg.data;
        switch (msg.flags)
        {
        case _CMD_STOP:
            *pstop = 1;
            return;
        case _CMD_ADD:
            if (ERR_OK != _uev_add(pwatcher, pusock, msg.session))
            {
                _add_close_qu(pwatcher, pusock);
            }
            break;
        case _CMD_DEL:
            _uev_del(pwatcher, pusock, msg.session);
            break;
        case _CMD_CLOSE://Ö÷¶¯¹Ø±Õ
            _add_close_qu(pwatcher, pusock);
            break;
        case _CMD_CONN:
            if (ERR_OK != _uev_add(pwatcher, pusock, msg.session))
            {
                _add_close_qu(pwatcher, pusock);
            }
            else
            {
                _conn_timeout_add(pwatcher, pusock);
            }
            break;
        case _CMD_CONN_TIMEOUT:
            pusock = _conn_timeout_remove(pwatcher, msg.id);
            if (NULL != pusock)
            {
                LOG_WARN("sock %d connect timeout.", msg.id);
                _add_close_qu(pwatcher, pusock);
            }
            pusock = NULL;
            break;
        }
        _uev_sub_ref_cmd(pusock);
    }
#ifdef NETEV_EVPORT
    if (0 == *pstop)
    {
        ASSERTAB(ERR_OK == _uev_add(pwatcher, psock, psock->events), "_uev_add failed.");
    }
#endif
}
#endif
struct netev_ctx *netev_new(struct tw_ctx *ptw, const uint32_t uthcnt, sid_t(*id_creater)(void *), void *pid)
{
    struct netev_ctx *pev = _new_pub_ev(ptw, uthcnt, id_creater, pid);
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
        _uev_cmd(&pctx->watcher[i], _CMD_STOP, 0, NULL, 0);
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
void _netev_add(struct watcher_ctx *pwatcher, struct sock_ctx *psock, uint32_t uiev)
{
#if defined(NETEV_IOCP)
    if (0 == psock->events
        && 0 != uiev)
    {
        if (NULL == CreateIoCompletionPort((HANDLE)psock->sock, pwatcher->iocp, 0, pwatcher->netev->thcnt))
        {
            LOG_ERROR("%s", ERRORSTR(ERRNO));
        }
    }
    psock->events |= uiev;
#else
    _uev_cmd(pwatcher, _CMD_ADD, uiev, psock, 0);
#endif
}
#ifndef NETEV_IOCP
static inline int32_t _trans_ev(events_t *pev, struct sock_ctx **psock)
{
    uint32_t uiev = 0;
#if defined(NETEV_EPOLL)
    if (pev->events & (EPOLLIN | EPOLLHUP | EPOLLERR))
    {
        uiev |= EV_READ;
    }
    if (pev->events & (EPOLLOUT | EPOLLHUP | EPOLLERR))
    {
        uiev |= EV_WRITE;
    }
    *psock = pev->data.ptr;
#elif defined(NETEV_KQUEUE)
    if (EVFILT_READ == pev->filter)
    {
        uiev |= EV_READ;
    }
    if (EVFILT_WRITE == pev->filter)
    {
        uiev |= EV_WRITE;
    }
    *psock = pev->udata;
#elif defined(NETEV_EVPORT)
    if (pev->portev_events & POLLIN)
    {
        uiev |= EV_READ;
    }
    if (pev->portev_events & POLLOUT)
    {
        uiev |= EV_WRITE;
    }
    *psock = pev->portev_user;
#endif
    return uiev;
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
    uint32_t uiev;
    struct sock_ctx *psock;
    for (int32_t i = 0; i < icnt; i++)
    {
        uiev = _trans_ev(&pwatcher->events[i], &psock);
        psock->ev_cb(pwatcher, psock, uiev, pstop);
    }
    struct message_ctx msg;
    while (ERR_OK == queue_pop(&pwatcher->qu_close, &msg))
    {
        _uev_sock_close(msg.data);
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
    sock.sock = pwatcher->pipes[0];
    sock.ev_cb = _cmd_cb;
    sock.events = sock.id = 0;
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
sid_t sock_id(struct sock_ctx *psock)
{
    return psock->id;
}
SOCKET sock_handle(struct sock_ctx *psock)
{
    return psock->sock;
}
SOCKET sock_listen(union netaddr_ctx *paddr)
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
    union netaddr_ctx addr;
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
