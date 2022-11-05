#include "event/event.h"

#ifndef EV_IOCP

#define ININT_EVENT_CNT  512
#define CMD_STOP         0x01
#define CMD_ADD          0x02
#define CMD_CLOSE        0x03
#define CMD_CONN         0x04
#define CMD_CONN_TIMEOUT 0x05

#define FLAGS_LSN   0x01
#define FLAGS_CONN  0x02
#define FLAGS_NORM  0x04
#define FLAGS_CLOSE 0x08

#define EV_READ    0x01
#define EV_WRITE   0x02

static void _cloexec(int32_t evfd)
{
#ifdef EV_EPOLL
    int32_t flag = fcntl(evfd, F_GETFD);
    if (ERR_FAILED == flag)
    {
        LOG_WARN("fcntl(%d, F_GETFD) failed.", evfd);
        return;
    }
    if (!(flag & FD_CLOEXEC))
    {
        if (ERR_FAILED == fcntl(evfd, F_SETFD, flag | FD_CLOEXEC))
        {
            LOG_WARN("fcntl(%d, F_SETFD, FD_CLOEXEC) failed.", evfd);
        }
    }
#endif
}
static int32_t _init_ev()
{
    int32_t evfd = INVALID_FD;
#if defined(EV_EPOLL)
    evfd = epoll_create(ONEK);
#elif defined(EV_KQUEUE)
    evfd = kqueue();
#elif defined(EV_EVPORT)
    evfd = port_create();
#endif
    ASSERTAB(INVALID_FD != evfd, ERRORSTR(ERRNO));
    _cloexec(evfd);
    return evfd;
}
static void _resize_events(watcher_ctx *watcher)
{
    FREE(watcher->events);
    watcher->nevent *= 2;
    MALLOC(watcher->events, sizeof(events_t) * watcher->nevent);
}
static void _cmd(watcher_ctx *watcher, int32_t cmd, sock_ctx *sock)
{
    static char trigger[1] = { '1' };
    cmd_ctx stcmd;
    stcmd.cmd = cmd;
    stcmd.sock = sock;
    mutex_lock(&watcher->qucmdlck);
    qu_cmd_push(&watcher->qucmd, &stcmd);
    ASSERTAB(sizeof(trigger) == write(watcher->pipes[1], trigger, sizeof(trigger)), "pipe write error.");
    mutex_unlock(&watcher->qucmdlck);
}
static int32_t _add_event(watcher_ctx *watcher, sock_ctx *sock, uint32_t ev)
{
    if (sock->flags & FLAGS_CLOSE)
    {
        return ERR_FAILED;
    }
#if defined(EV_EPOLL)
    struct epoll_event epev;
    ZERO(&epev, sizeof(epev));
    epev.data.ptr = sock;
    ev |= sock->events;
    if (ev & EV_READ)
    {
        epev.events |= EPOLLIN;
    }
    if (ev & EV_WRITE)
    {
        epev.events |= EPOLLOUT;
    }
    if (ERR_FAILED == epoll_ctl(watcher->evhandle,
        (0 == sock->events ? EPOLL_CTL_ADD : EPOLL_CTL_MOD), sock->sock, &epev))
    {
        return ERR_FAILED;
    }
    sock->events = ev;
#elif defined(EV_KQUEUE)
    if ((ev & EV_READ)
        && !(sock->events & EV_READ))
    {
        sock->events |= EV_READ;
        struct kevent kev;
        EV_SET(&kev, sock->sock, EVFILT_READ, EV_ADD, 0, 0, sock);
        if (ERR_FAILED == kevent(watcher->evhandle, &kev, 1, NULL, 0, NULL)
            || 0 != (kev.flags & EV_ERROR))
        {
            return ERR_FAILED;
        }
    }
    if ((ev & EV_WRITE)
        && !(sock->events & EV_WRITE))
    {
        sock->events |= EV_WRITE;
        struct kevent kev;
        EV_SET(&kev, sock->sock, EVFILT_WRITE, EV_ADD, 0, 0, sock);
        if (ERR_FAILED == kevent(watcher->evhandle, &kev, 1, NULL, 0, NULL)
            || 0 != (kev.flags & EV_ERROR))
        {
            return ERR_FAILED;
        }
    }
#elif defined(EV_EVPORT)
    sock->events |= ev;
    ev = 0;
    if (sock->events & EV_READ)
    {
        ev |= POLLIN;
    }
    if (sock->events & EV_WRITE)
    {
        ev |= POLLOUT;
    }
    if (ERR_FAILED == port_associate(watcher->evhandle, PORT_SOURCE_FD, sock->sock, ev, sock))
    {
        return ERR_FAILED;
    }
#endif
    return ERR_OK;
}
static void _del_event(watcher_ctx *watcher, sock_ctx *sock, uint32_t ev)
{
    if (0 == ev
        || (sock->flags & FLAGS_CLOSE))
    {
        return;
    }
#if defined(EV_EPOLL)
    struct epoll_event epev;
    ZERO(&epev, sizeof(epev));
    epev.data.ptr = sock;
    sock->events = sock->events & ~ev;
    if (0 == sock->events)
    {
        (void)epoll_ctl(watcher->evhandle, EPOLL_CTL_DEL, sock->sock, &epev);
    }
    else
    {
        if (sock->events & EV_READ)
        {
            epev.events |= EPOLLIN;
        }
        if (sock->events & EV_WRITE)
        {
            epev.events |= EPOLLOUT;
        }
        (void)epoll_ctl(watcher->evhandle, EPOLL_CTL_MOD, sock->sock, &epev);
    }
#elif defined(EV_KQUEUE)
    if ((ev & EV_READ)
        && (sock->events & EV_READ))
    {
        sock->events = sock->events & ~EV_READ;
        struct kevent kev;
        EV_SET(&kev, sock->sock, EVFILT_READ, EV_DELETE, 0, 0, sock);
        (void)kevent(watcher->evhandle, &kev, 1, NULL, 0, NULL);
    }
    if ((ev & EV_WRITE)
        && (sock->events & EV_WRITE))
    {
        sock->events = sock->events & ~EV_WRITE;
        struct kevent kev;
        EV_SET(&kev, sock->sock, EVFILT_WRITE, EV_DELETE, 0, 0, sock);
        (void)kevent(watcher->evhandle, &kev, 1, NULL, 0, NULL);
    }
#elif defined(EV_EVPORT)
    sock->events = sock->events & ~ev;
    if (0 == sock->events)
    {
        (void)port_dissociate(watcher->evhandle, PORT_SOURCE_FD, sock->sock);
    }
    else
    {
        ev = 0;
        if (sock->events & EV_READ)
        {
            ev |= POLLIN;
        }
        if (sock->events & EV_WRITE)
        {
            ev |= POLLOUT;
        }
        (void)port_associate(watcher->evhandle, PORT_SOURCE_FD, sock->sock, ev, sock);
    }
#endif
}
static int32_t _trans_event(events_t *ev, sock_ctx **sock)
{
    uint32_t rtn = 0;
#if defined(EV_EPOLL)
    if (ev->events & (EPOLLIN | EPOLLHUP | EPOLLERR))
    {
        rtn |= EV_READ;
    }
    if (ev->events & (EPOLLOUT | EPOLLHUP | EPOLLERR))
    {
        rtn |= EV_WRITE;
    }
    *sock = ev->data.ptr;
#elif defined(EV_KQUEUE)
    if (EVFILT_READ == ev->filter)
    {
        rtn |= EV_READ;
    }
    if (EVFILT_WRITE == ev->filter)
    {
        rtn |= EV_WRITE;
    }
    *sock = ev->udata;
#elif defined(EV_EVPORT)
    if (ev->portev_events & POLLIN)
    {
        rtn |= EV_READ;
    }
    if (ev->portev_events & POLLOUT)
    {
        rtn |= EV_WRITE;
    }
    *sock = ev->portev_user;
#endif
    return rtn;
}
static void _sock_close(struct sock_ctx *psock)
{
    /*if (psock->flags & _FLAGS_CONN)
    {
        struct usock_ctx *pusock = _get_usock(psock);
        shutdown(psock->sock, SHUT_WR);
        SAFE_CLOSE_SOCK(psock->sock);
        pusock->conn_cb(psock, ERR_FAILED, &pusock->ud);
        ATOMIC_ADD(&pusock->ref_r, -1);
    }
    else if (psock->flags & _FLAGS_NORM)
    {
        struct usock_ctx *pusock = _get_usock(psock);
        shutdown(psock->sock, SHUT_WR);
        SAFE_CLOSE_SOCK(psock->sock);
        pusock->c_cb(psock, &pusock->ud);
        ATOMIC_ADD(&pusock->ref_r, -1);
    }*/
}
static void _wait_event(watcher_ctx *watcher, int32_t *stop)
{
    int32_t cnt;
#if defined(EV_EPOLL)
    cnt = epoll_wait(watcher->evhandle, watcher->events, watcher->nevent, -1);
#elif defined(EV_KQUEUE)
    cnt = kevent(watcher->evhandle, NULL, 0, watcher->events, watcher->nevent, NULL);
#elif defined(EV_EVPORT)
    uint32_t portcnt = 1;
    (void)port_getn(watcher->evhandle, watcher->events, watcher->nevent, &portcnt, NULL);
    cnt = (int32_t)portcnt;
#endif
    uint32_t ev;
    sock_ctx *sock;
    for (int32_t i = 0; i < cnt; i++)
    {
        ev = _trans_event(&watcher->events[i], &sock);
        sock->ev_cb(watcher, sock, ev, stop);
    }
    sock_ctx **closed;
    while (NULL != (closed = qu_close_pop(&watcher->quclose)))
    {
        _sock_close(*closed);
    }
    if (0 == *stop
        && cnt >= watcher->nevent)
    {
        _resize_events(watcher);
    }
}
static void _cmd_cb(watcher_ctx *watcher, struct sock_ctx *sock, uint32_t ev, int32_t *stop)
{
    char trigger[32];
    cmd_ctx cmd;
    cmd_ctx *tmp;
    ssize_t nread = read(sock->sock, trigger, sizeof(trigger));
    for (ssize_t i = 0; i < nread; i++)
    {
        mutex_lock(&watcher->qucmdlck);
        tmp = qu_cmd_pop(&watcher->qucmd);
        if (NULL == tmp)
        {
            mutex_unlock(&watcher->qucmdlck);
            break;
        }
        cmd = *tmp;
        mutex_unlock(&watcher->qucmdlck);
        switch (cmd.cmd)
        {
        case CMD_STOP:
            *stop = 1;
            PRINTD("%s", "stop event loop.");
            break;
        default:
            break;
        }
    }
    
#ifdef EV_EVPORT
    if (0 == *stop)
    {
        ASSERTAB(ERR_OK == _add_event(watcher, sock, sock->events), "_add_event failed.");
    }
#endif
}
static void _loop(void *arg)
{
    int32_t stop = 0;
    watcher_ctx *watcher = (watcher_ctx *)arg;
    sock_ctx sock;
    ZERO(&sock, sizeof(sock));
    sock.sock = watcher->pipes[0];
    sock.ev_cb = _cmd_cb;
    ASSERTAB(ERR_OK == _add_event(watcher, &sock, EV_READ), "_add_event failed.");
    while (0 == stop)
    {
        _wait_event(watcher, &stop);
    }
}
void ev_init(ev_ctx *ctx, uint32_t nthreads)
{
    ctx->nthreads = (0 == nthreads ? 1 : nthreads);
    MALLOC(ctx->watcher, sizeof(watcher_ctx) * ctx->nthreads);
    watcher_ctx *watcher;
    for (uint32_t i = 0; i < ctx->nthreads; i++)
    {
        watcher = &ctx->watcher[i];
        watcher->ev = ctx;
        watcher->evhandle = _init_ev();
        watcher->nevent = ININT_EVENT_CNT;
        MALLOC(watcher->events, sizeof(events_t) * watcher->nevent);
        ASSERTAB(ERR_OK == pipe(watcher->pipes), ERRORSTR(ERRNO));
        mutex_init(&watcher->qucmdlck);
        qu_cmd_init(&watcher->qucmd, ONEK);
        qu_close_init(&watcher->quclose, ONEK);
        thread_init(&watcher->thread);
        thread_creat(&watcher->thread, _loop, watcher);
    }
}
static void _free_watcher(watcher_ctx *watcher)
{
    qu_close_free(&watcher->quclose);
    qu_cmd_free(&watcher->qucmd);
    mutex_free(&watcher->qucmdlck);
    close(watcher->pipes[0]);
    close(watcher->pipes[1]);
    FREE(watcher->events);    
    (void)close(watcher->evhandle);
}
void ev_free(ev_ctx *ctx)
{
    uint32_t i;
    for (i = 0; i < ctx->nthreads; i++)
    {
        _cmd(&ctx->watcher[i], CMD_STOP, NULL);
    }
    for (i = 0; i < ctx->nthreads; i++)
    {
        thread_join(&ctx->watcher[i].thread);
        _free_watcher(&ctx->watcher[i]);
    }
    FREE(ctx->watcher);
}

#endif //EV_IOCP
