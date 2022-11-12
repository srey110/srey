#include "event/uev.h"
#include "event/evworker.h"
#include "loger.h"
#include "utils.h"
#include "hashmap.h"

#ifndef EV_IOCP

#define EVENT_READ          0x01
#define EVENT_WRITE         0x02
typedef enum UEV_CMDS
{
    UEVCMD_RESET = 0x00,
    UEVCMD_STOP,
    UEVCMD_RECV,
    UEVCMD_SEND,
    UEVCMD_LSN,
    UEVCMD_CONN,

    UEVCMD_TOTAL,
}UEV_CMDS;
typedef struct mapev_ctx
{
    SOCKET fd;
    int32_t events;
}mapev_ctx;
#define PIPCMD_SEND(ev, cmd)\
do {\
    uint64_t hs = FD_HASH(cmd.fd);\
    watcher_ctx *watcher = (1 == (ev)->nthreads) ? (ev)->watcher : (&(ev)->watcher[hs % (ev)->nthreads]);\
    _pipcmd_send(watcher, &cmd);\
} while (0)
static inline uint64_t _map_hash(const void *item, uint64_t seed0, uint64_t seed1)
{
    SOCKET sock = ((const mapev_ctx *)item)->fd;
    return hash((const char *)&sock, sizeof(sock));
}
static inline int _map_compare(const void *a, const void *b, void *udata)
{
    return (int)(((const mapev_ctx *)a)->fd - ((const mapev_ctx *)b)->fd);
}
static inline mapev_ctx *_map_get(watcher_ctx *watcher, SOCKET fd)
{
    mapev_ctx key;
    key.fd = fd;
    mapev_ctx *mapev = hashmap_get(watcher->mapev, &key);
    if (NULL != mapev)
    {
        return mapev;
    }
    mapev_ctx evval;
    evval.fd = fd;
    evval.events = 0;
    hashmap_set(watcher->mapev, &evval);
    return hashmap_get(watcher->mapev, &key);
}
#if defined(EV_KQUEUE)
static inline void _check_resize(watcher_ctx *watcher)
{
    if (watcher->nsize == watcher->nchange)
    {
        watcher->nsize *= 2;
        events_t *newptr;
        REALLOC(newptr, watcher->changelist, sizeof(events_t) * watcher->nsize);
        watcher->changelist = newptr;
    }
}
#endif
void _pipcmd_send(watcher_ctx *watcher, pipcmd_ctx *cmd)
{
    static char trigger[1] = { 's' };
    mutex_lock(&watcher->pipcmdlck);
    pip_cmd_push(&watcher->pipcmd, cmd);
    mutex_unlock(&watcher->pipcmdlck);
    ASSERTAB(sizeof(trigger) == write(watcher->pipes[1], trigger, sizeof(trigger)), "cmd pipe write error.");
}
static inline void _cmd_cb_stop(watcher_ctx *watcher, pipcmd_ctx *cmd, int32_t *stop)
{
    *stop = 1;
}
void _post_reset(ev_ctx *ev, sock_ctx *sock)
{
    pipcmd_ctx cmd;
    cmd.cmd = UEVCMD_RESET;
    cmd.fd = sock->sock;
    PIPCMD_SEND(ev, cmd);
}
static inline void _cmd_cb_reset(watcher_ctx *watcher, pipcmd_ctx *cmd, int32_t *stop)
{
    mapev_ctx *mapev = _map_get(watcher, cmd->fd);
    mapev->events = 0;
}
int32_t _post_recv(ev_ctx *ev, struct sock_ctx *sock)
{
    pipcmd_ctx cmd;
    cmd.cmd = UEVCMD_RECV;
    cmd.fd = sock->sock;
    PIPCMD_SEND(ev, cmd);
    return ERR_OK;
}
static inline void _cmd_cb_recv(watcher_ctx *watcher, pipcmd_ctx *cmd, int32_t *stop)
{
    mapev_ctx *mapev = _map_get(watcher, cmd->fd);
    if (ERR_OK != _add_event(watcher, cmd->fd, &mapev->events, EVENT_READ, NULL))
    {
        ewcmd_error(watcher->ev->worker, cmd->fd);
    }
}
int32_t _post_send(ev_ctx *ev, sock_ctx *sock)
{
    pipcmd_ctx cmd;
    cmd.cmd = UEVCMD_SEND;
    cmd.fd = sock->sock;
    PIPCMD_SEND(ev, cmd);
    return ERR_OK;
}
static inline void _cmd_cb_send(watcher_ctx *watcher, pipcmd_ctx *cmd, int32_t *stop)
{
    mapev_ctx *mapev = _map_get(watcher, cmd->fd);
    if (ERR_OK != _add_event(watcher, cmd->fd, &mapev->events, EVENT_WRITE, NULL))
    {
        ewcmd_error(watcher->ev->worker, cmd->fd);
    }
}
void _post_listen(watcher_ctx *watcher, sock_ctx *sock)
{
    pipcmd_ctx cmd;
    cmd.cmd = UEVCMD_LSN;
    cmd.fd = sock->sock;
    cmd.sock = sock;
    _pipcmd_send(watcher, &cmd);
}
void _post_listen_rand(ev_ctx *ev, sock_ctx *sock)
{
    pipcmd_ctx cmd;
    cmd.cmd = UEVCMD_LSN;
    cmd.fd = sock->sock;
    cmd.sock = sock;
    PIPCMD_SEND(ev, cmd);
}
static inline void _cmd_cb_lsn(watcher_ctx *watcher, pipcmd_ctx *cmd, int32_t *stop)
{
    if (ERR_OK != _add_event(watcher, cmd->fd, &cmd->sock->events, EVENT_READ, cmd->sock))
    {
        LOG_ERROR("%s", "add listen socket in loop error.");
    }
}
void _post_connect(ev_ctx *ev, sock_ctx *sock)
{
    pipcmd_ctx cmd;
    cmd.cmd = UEVCMD_CONN;
    cmd.fd = sock->sock;
    cmd.sock = sock;
    PIPCMD_SEND(ev, cmd);
}
static inline void _cmd_cb_conn(watcher_ctx *watcher, pipcmd_ctx *cmd, int32_t *stop)
{
    if (ERR_OK == _add_event(watcher, cmd->fd, &cmd->sock->events, EVENT_WRITE, cmd->sock))
    {
        return;
    }
    ud_cxt *ud;
    connect_cb cb = _get_connect_ud(cmd->sock, &ud);
    ewcmd_connect(watcher->ev->worker, cmd->fd, cb, ud);
    FREE(cmd->sock);
}
int32_t _add_event(watcher_ctx *watcher, SOCKET fd, int32_t *events, int32_t ev, void *arg)
{
#if defined(EV_EPOLL)
    events_t epev;
    ZERO(&epev, sizeof(epev));
    epev.data.fd = fd;
    epev.data.ptr = arg;
    ev |= (*events);
    if (ev & EVENT_READ)
    {
        epev.events |= EPOLLIN;
    }
    if (ev & EVENT_WRITE)
    {
        epev.events |= EPOLLOUT;
    }
    if (ERR_FAILED == epoll_ctl(watcher->evfd, 
                                (0 == (*events) ? EPOLL_CTL_ADD : EPOLL_CTL_MOD), 
                                fd, &epev))
    {
        return ERR_FAILED;
    }
    *events = ev;
#elif defined(EV_KQUEUE)
    if ((ev & EVENT_READ)
        && !((*events) & EVENT_READ))
    {
        (*events) |= EVENT_READ;
        _check_resize(watcher);
        events_t *kev = &watcher->changelist[watcher->nchange];
        EV_SET(kev, fd, EVFILT_READ, EV_ADD, 0, 0, arg);
        watcher->nchange++;
    }
    if ((ev & EVENT_WRITE)
        && !((*events) & EVENT_WRITE))
    {
        (*events) |= EVENT_WRITE;
        _check_resize(watcher);
        events_t *kev = &watcher->changelist[watcher->nchange];
        EV_SET(kev, fd, EVFILT_WRITE, EV_ADD, 0, 0, arg);
        watcher->nchange++;
    }
#elif defined(EV_EVPORT)
    (*events) |= ev;
    ev = 0;
    if ((*events) & EVENT_READ)
    {
        ev |= POLLIN;
    }
    if ((*events) & EVENT_WRITE)
    {
        ev |= POLLOUT;
    }
    if (ERR_FAILED == port_associate(watcher->evfd, PORT_SOURCE_FD, fd, ev, arg))
    {
        return ERR_FAILED;
    }
#endif
    return ERR_OK;
}
void _del_event(watcher_ctx *watcher, SOCKET fd, int32_t *events, int32_t ev, void *arg)
{
#if defined(EV_EPOLL)
    events_t epev;
    ZERO(&epev, sizeof(epev));
    epev.data.fd = fd;
    epev.data.ptr = arg;
    *events = (*events) & ~ev;
    if (0 == (*events))
    {
        (void)epoll_ctl(watcher->evfd, EPOLL_CTL_DEL, fd, &epev);
    }
    else
    {
        if ((*events) & EVENT_READ)
        {
            epev.events |= EPOLLIN;
        }
        if ((*events) & EVENT_WRITE)
        {
            epev.events |= EPOLLOUT;
        }
        (void)epoll_ctl(watcher->evfd, EPOLL_CTL_MOD, fd, &epev);
    }
#elif defined(EV_KQUEUE)
    if ((ev & EVENT_READ)
        && ((*events) & EVENT_READ))
    {
        *events = (*events) & ~EVENT_READ;
        _check_resize(watcher);
        events_t *kev = &watcher->changelist[watcher->nchange];
        EV_SET(kev, fd, EVFILT_READ, EV_DELETE, 0, 0, arg);        
        watcher->nchange++;
    }
    if ((ev & EVENT_WRITE)
        && ((*events) & EVENT_WRITE))
    {
        *events = (*events) & ~EVENT_WRITE;
        _check_resize(watcher);
        events_t *kev = &watcher->changelist[watcher->nchange];
        EV_SET(kev, fd, EVFILT_WRITE, EV_DELETE, 0, 0, arg);        
        watcher->nchange++;
    }
#elif defined(EV_EVPORT)
    *events = (*events) & ~ev;
    if (0 == (*events))
    {
        (void)port_dissociate(watcher->evfd, PORT_SOURCE_FD, fd);
    }
    else
    {
        ev = 0;
        if ((*events) & EVENT_READ)
        {
            ev |= POLLIN;
        }
        if ((*events) & EVENT_WRITE)
        {
            ev |= POLLOUT;
        }
        (void)port_associate(watcher->evfd, PORT_SOURCE_FD, fd, ev, arg);
    }
#endif
}
static inline int32_t _parse_event(events_t *ev, SOCKET *fd, void **arg)
{
    int32_t rtn = 0;
#if defined(EV_EPOLL)
    if (ev->events & (EPOLLIN | EPOLLHUP | EPOLLERR))
    {
        rtn |= EVENT_READ;
    }
    if (ev->events & (EPOLLOUT | EPOLLHUP | EPOLLERR))
    {
        rtn |= EVENT_WRITE;
    }
    *fd = (SOCKET)(ev->data.fd);
    *arg = ev->data.ptr;
#elif defined(EV_KQUEUE)
    if (EVFILT_READ == ev->filter)
    {
        rtn |= EVENT_READ;
    }
    if (EVFILT_WRITE == ev->filter)
    {
        rtn |= EVENT_WRITE;
    }
    *fd = (SOCKET)(ev->ident);
    *arg = ev->udata;
#elif defined(EV_EVPORT)
    if (ev->portev_events & POLLIN)
    {
        rtn |= EVENT_READ;
    }
    if (ev->portev_events & POLLOUT)
    {
        rtn |= EVENT_WRITE;
    }
    *fd = (SOCKET)(ev->portev_object);
    *arg = ev->portev_user;
#endif
    return rtn;
}
static inline void _fd_event(watcher_ctx *watcher, SOCKET fd, int32_t ev)
{
    mapev_ctx *mapev = _map_get(watcher, fd);
    _del_event(watcher, fd, &mapev->events, ev, NULL);
    if (ev & EVENT_READ)
    {
        ewcmd_canread(watcher->ev->worker, fd);
    }
    if (ev & EVENT_WRITE)
    {
        ewcmd_canwrite(watcher->ev->worker, fd);
    }
}
static void _loop_event(void *arg)
{
    watcher_ctx *watcher = (watcher_ctx *)arg;
    void(*_sockctx_cb[FLAG_TOTAL])(watcher_ctx *, sock_ctx *, int32_t);
    _sockctx_cb[FLAG_LSN] = _on_accept_cb;
    _sockctx_cb[FLAG_CONN] = _on_connect_cb;
    _sockctx_cb[FLAG_CMD] = _on_cmd_cb;

    void(*_cmd_cb[UEVCMD_TOTAL])(watcher_ctx *, pipcmd_ctx *, int32_t *);
    _cmd_cb[UEVCMD_RESET] = _cmd_cb_reset;
    _cmd_cb[UEVCMD_STOP] = _cmd_cb_stop;
    _cmd_cb[UEVCMD_RECV] = _cmd_cb_recv;
    _cmd_cb[UEVCMD_SEND] = _cmd_cb_send;
    _cmd_cb[UEVCMD_LSN] = _cmd_cb_lsn;
    _cmd_cb[UEVCMD_CONN] = _cmd_cb_conn;

    //pipe ¼Ó½øÈ¥
    sock_ctx cmdsock;
    ZERO(&cmdsock, sizeof(cmdsock));
    cmdsock.flag = FLAG_CMD;
    cmdsock.sock = watcher->pipes[0];
    ASSERTAB(ERR_OK == _add_event(watcher, cmdsock.sock, &cmdsock.events, EVENT_READ, &cmdsock), "add cmd pip error.");
#if defined(EV_EVPORT)
    uint32_t nget;
#endif
    SOCKET fd;
    pipcmd_ctx cmd;
    sock_ctx *sock;
    int32_t i, cnt, ev, stop = 0;
    while (0 == stop)
    {
#if defined(EV_EPOLL)
        cnt = epoll_wait(watcher->evfd, watcher->events, watcher->nevent, -1);
#elif defined(EV_KQUEUE)
        cnt = kevent(watcher->evfd, watcher->changelist, watcher->nchange, watcher->events, watcher->nevent, NULL);
        watcher->nchange = 0;
#elif defined(EV_EVPORT)
        nget = 1;
        (void)port_getn(watcher->evfd, watcher->events, watcher->nevent, &nget, NULL);
        cnt = (int32_t)nget;
#endif
        for (i = 0; i < cnt; i++)
        {
            ev = _parse_event(&watcher->events[i], &fd, (void **)&sock);
            ASSERTAB(0 != fd, "111111111111111111111");
            if (NULL != sock)
            {
                _sockctx_cb[sock->flag](watcher, sock, ev);
            }
            else
            {
                _fd_event(watcher, fd, ev);
            }
        }

        for (i = 0; i < watcher->ncmd; i++)
        {
            mutex_lock(&watcher->pipcmdlck);
            cmd = *pip_cmd_pop(&watcher->pipcmd);
            mutex_unlock(&watcher->pipcmdlck);
            _cmd_cb[cmd.cmd](watcher, &cmd, &stop);
        }
        watcher->ncmd = 0;

        if (0 == stop
            && cnt == watcher->nevent)
        {
            FREE(watcher->events);
            watcher->nevent *= 2;
            MALLOC(watcher->events, sizeof(events_t) * watcher->nevent);
        }
    }
}
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
static int32_t _init_evfd()
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
void ev_init(ev_ctx *ctx, uint32_t nthreads)
{
    ctx->stop = 0;
    ctx->nthreads = (0 == nthreads ? 1 : nthreads);
    mutex_init(&ctx->mulsn);
    qu_lsn_init(&ctx->qulsn, 8);
    ctx->worker = eworker_init(ctx, ctx->nthreads, ctx->nthreads * 2);
    MALLOC(ctx->watcher, sizeof(watcher_ctx) * ctx->nthreads);
    watcher_ctx *watcher;
    for (uint32_t i = 0; i < ctx->nthreads; i++)
    {
        watcher = &ctx->watcher[i];  
        watcher->ncmd = 0;
        watcher->ev = ctx;
#if defined(EV_KQUEUE)
        watcher->nsize = INIT_EVENTS_CNT;
        watcher->nchange = 0;
        MALLOC(watcher->changelist, sizeof(events_t) * watcher->nsize);
#endif
        watcher->nevent = INIT_EVENTS_CNT;
        MALLOC(watcher->events, sizeof(events_t) * watcher->nevent);
        watcher->evfd = _init_evfd();
        ASSERTAB(ERR_OK == pipe(watcher->pipes), ERRORSTR(ERRNO));        
        pip_cmd_init(&watcher->pipcmd, ONEK * 4);
        mutex_init(&watcher->pipcmdlck);
        watcher->mapev = hashmap_new_with_allocator(_malloc, _realloc, _free,
            sizeof(mapev_ctx), ONEK * 4, 0, 0, _map_hash, _map_compare, NULL, NULL);
        watcher->thevent = thread_creat(_loop_event, watcher);
    }
}
void ev_free(ev_ctx *ctx)
{
    uint32_t i;
    ctx->stop = 1;
    pipcmd_ctx cmd;
    cmd.cmd = UEVCMD_STOP;
    for (i = 0; i < ctx->nthreads; i++)
    {
        _pipcmd_send(&ctx->watcher[i], &cmd);
    }
    for (i = 0; i < ctx->nthreads; i++)
    {
        thread_join(ctx->watcher[i].thevent);
    }
    eworker_free(ctx->worker);
    watcher_ctx *watcher;
    for (i = 0; i < ctx->nthreads; i++)
    {
        watcher = &ctx->watcher[i];
        close(watcher->pipes[0]);
        close(watcher->pipes[1]);
        close(watcher->evfd);        
        pip_cmd_free(&watcher->pipcmd);
        mutex_free(&watcher->pipcmdlck);
#if defined(EV_KQUEUE)
        FREE(watcher->changelist);
#endif
        FREE(watcher->events);
        hashmap_free(watcher->mapev);
    }
    FREE(ctx->watcher);
    struct listener_ctx **lsn;
    while (NULL != (lsn = qu_lsn_pop(&ctx->qulsn)))
    {
        _freelsn(*lsn);
    }
    qu_lsn_free(&ctx->qulsn);
    mutex_free(&ctx->mulsn);
}

#endif //EV_IOCP
