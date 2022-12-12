#include "event/uev.h"
#include "hashmap.h"
#include "netutils.h"
#include "timer.h"
#include "utils.h"
#include "loger.h"

#ifndef EV_IOCP

static atomic_t _init_once = 0;
static void(*cmd_cbs[CMD_TOTAL])(watcher_ctx *watcher, cmd_ctx *cmd);
typedef struct pip_ctx
{
    int32_t pipes[2];
    sock_ctx skpip;
}pip_ctx;

static inline uint64_t _map_hash(const void *item, uint64_t seed0, uint64_t seed1)
{
    return FD_HASH(((const map_element *)item)->fd);
}
static inline int _map_compare(const void *a, const void *b, void *ud)
{
    return (int)(((const map_element *)a)->fd - ((const map_element *)b)->fd);
}
void _send_cmd(watcher_ctx *watcher, uint32_t index, cmd_ctx *cmd)
{
    ASSERTAB(sizeof(cmd_ctx) == write(watcher->pipes[index].pipes[1], cmd, sizeof(cmd_ctx)), "pipe write error.");
}
static void _cmd_loop(watcher_ctx *watcher, sock_ctx *skctx, int32_t ev)
{
    int32_t i, cnt, nread;
    cmd_ctx cmds[CMD_MAX_NREAD];
    do
    {
        nread = read(skctx->fd, cmds, sizeof(cmds));
        if (nread <= 0)
        {
            break;
        }
        cnt = nread / sizeof(cmd_ctx);
        for (i = 0; i < cnt; i++)
        {
            cmd_cbs[cmds[i].cmd](watcher, &cmds[i]);
        }
    }while (nread == sizeof(cmds));
#ifdef MANUAL_ADD
    if (0 == watcher->stop)
    {
        ASSERTAB(ERR_OK == _add_event(watcher, skctx->fd, &skctx->events, ev, skctx), ERRORSTR(ERRNO));
    }
#endif
}
static void _init_callback()
{
    cmd_cbs[CMD_STOP] = _on_cmd_stop;
    cmd_cbs[CMD_DISCONN] = _on_cmd_disconn;
    cmd_cbs[CMD_LSN] = _on_cmd_lsn;
    cmd_cbs[CMD_CONN] = _on_cmd_conn;
    cmd_cbs[CMD_ADDACP] = _on_cmd_addacp;
    cmd_cbs[CMD_ADDUDP] = _on_cmd_add_udp;
    cmd_cbs[CMD_SEND] = _on_cmd_send;
}
static void _init_cmd(watcher_ctx *watcher)
{
    sock_ctx *skctx;
    for (uint32_t i = 0; i < watcher->npipes; i++)
    {
        skctx = &watcher->pipes[i].skpip;
        skctx->fd = watcher->pipes[i].pipes[0];
        skctx->events = 0;
        skctx->type = 0;
        skctx->ev_cb = _cmd_loop;
        _add_fd(watcher, skctx);
        ASSERTAB(ERR_OK == _add_event(watcher, skctx->fd, &skctx->events, EVENT_READ, skctx), ERRORSTR(ERRNO));
    }
}
#ifdef COMMIT_NCHANGES
static inline void _check_changes(watcher_ctx *watcher)
{
    if (watcher->nchanges >= watcher->nsize)
    {
#if defined(EV_KQUEUE)
        watcher->nsize *= 2;
        REALLOC(watcher->changes, watcher->changes, sizeof(changes_t) * watcher->nsize);
#elif defined(EV_DEVPOLL)
        if (ERR_FAILED == pwrite(watcher->evfd, watcher->changes, sizeof(changes_t) * watcher->nchanges, 0))
        {
            LOG_WARN("%s", ERRORSTR(ERRNO));
        }
        watcher->nchanges = 0;
#endif
    }
}
#endif
int32_t _add_event(watcher_ctx *watcher, SOCKET fd, int32_t *events, int32_t ev, void *arg)
{
#if defined(EV_EPOLL)
    events_t epev;
    ZERO(&epev, sizeof(epev));
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
#if TRIGGER_ET
    epev.events |= EPOLLET;
#endif
    if (ERR_FAILED == epoll_ctl(watcher->evfd,
                               0 == (*events) ? EPOLL_CTL_ADD : EPOLL_CTL_MOD,
                               fd,
                               &epev))
    {
        return ERR_FAILED;
    }
    *events = ev;
#elif defined(EV_KQUEUE)
    if ((ev & EVENT_READ)
        && !((*events) & EVENT_READ))
    {
        (*events) |= EVENT_READ;
        _check_changes(watcher);
        changes_t *kev = &watcher->changes[watcher->nchanges];
        EV_SET(kev, fd, EVFILT_READ, EV_ADD, 0, 0, arg);
        watcher->nchanges++;
    }
    if ((ev & EVENT_WRITE)
        && !((*events) & EVENT_WRITE))
    {
        (*events) |= EVENT_WRITE;
        _check_changes(watcher);
        changes_t *kev = &watcher->changes[watcher->nchanges];
        EV_SET(kev, fd, EVFILT_WRITE, EV_ADD, 0, 0, arg);
        watcher->nchanges++;
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
#elif defined(EV_POLLSET)
    ev |= (*events);
    struct poll_ctl ctl;
    ctl.fd = fd;
    ctl.events = 0;
    if (ev & EVENT_READ)
    {
        ctl.events |= POLLIN;
    }
    if (ev & EVENT_WRITE)
    {
        ctl.events |= POLLOUT;
    }
    ctl.cmd = (0 == (*events) ? PS_ADD : PS_MOD);
    if (0 != pollset_ctl(watcher->evfd, &ctl, 1))
    {
        return ERR_FAILED;
    }
    *events = ev;
#elif defined(EV_DEVPOLL)
    (*events) |= ev;
    _check_changes(watcher);
    changes_t *pfd = &watcher->changes[watcher->nchanges];
    pfd->fd = fd;
    pfd->revents = 0;
    pfd->events = 0;
    if ((*events) & EVENT_READ)
    {
        pfd->events |= POLLIN;
    }
    if ((*events) & EVENT_WRITE)
    {
        pfd->events |= POLLOUT;
    }
    watcher->nchanges++;
#endif
    return ERR_OK;
}
void _del_event(watcher_ctx *watcher, SOCKET fd, int32_t *events, int32_t ev, void *arg)
{
#if defined(EV_EPOLL)
    events_t epev;
    ZERO(&epev, sizeof(epev));
    epev.data.ptr = arg;
    *events = (*events) & ~ev;
#if TRIGGER_ET
    *events = (*events) & ~EPOLLET;
#endif
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
#if TRIGGER_ET
        epev.events |= EPOLLET;
#endif
        (void)epoll_ctl(watcher->evfd, EPOLL_CTL_MOD, fd, &epev);
    }
#elif defined(EV_KQUEUE)
    if ((ev & EVENT_READ)
        && ((*events) & EVENT_READ))
    {
        *events = (*events) & ~EVENT_READ;
        _check_changes(watcher);
        changes_t *kev = &watcher->changes[watcher->nchanges];
        EV_SET(kev, fd, EVFILT_READ, EV_DELETE, 0, 0, arg);
        watcher->nchanges++;
    }
    if ((ev & EVENT_WRITE)
        && ((*events) & EVENT_WRITE))
    {
        *events = (*events) & ~EVENT_WRITE;
        _check_changes(watcher);
        changes_t *kev = &watcher->changes[watcher->nchanges];
        EV_SET(kev, fd, EVFILT_WRITE, EV_DELETE, 0, 0, arg);
        watcher->nchanges++;
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
#elif defined(EV_POLLSET)
    *events = (*events) & ~ev;
    if (0 == (*events))
    {
        struct poll_ctl ctl;
        ctl.cmd = PS_DELETE;
        ctl.events = 0;
        ctl.fd = fd;
        (void)pollset_ctl(watcher->evfd, &ctl, 1);
    }
    else
    {
        struct poll_ctl ctl;
        ctl.cmd = PS_MOD;
        ctl.fd = fd;
        ctl.events = 0;
        if ((*events) & EVENT_READ)
        {
            ctl.events |= POLLIN;
        }
        if ((*events) & EVENT_WRITE)
        {
            ctl.events |= POLLOUT;
        }
        (void)pollset_ctl(watcher->evfd, &ctl, 1);
    }
#elif defined(EV_DEVPOLL)
    *events = (*events) & ~ev;
    _check_changes(watcher);
    changes_t *pfd = &watcher->changes[watcher->nchanges];
    pfd->fd = fd;
    pfd->events = POLLREMOVE;
    pfd->revents = 0;
    watcher->nchanges++;
    if (0 != (*events))
    {
        _check_changes(watcher);
        pfd = &watcher->changes[watcher->nchanges];
        pfd->fd = fd;
        pfd->events = 0;
        pfd->revents = 0;
        if ((*events) & EVENT_READ)
        {
            pfd->events |= POLLIN;
        }
        if ((*events) & EVENT_WRITE)
        {
            pfd->events |= POLLOUT;
        }
        watcher->nchanges++;
    }
#endif
}
static inline int32_t _parse_event(events_t *ev, SOCKET *fd, void **arg)
{
    int32_t rtn = 0;
#if defined(EV_EPOLL)
    if (ev->events & (EPOLLHUP | EPOLLERR))
    {
        rtn = EVENT_READ | EVENT_WRITE;
    }
    else
    {
        if (ev->events & EPOLLIN)
        {
            rtn |= EVENT_READ;
        }
        if (ev->events & EPOLLOUT)
        {
            rtn |= EVENT_WRITE;
        }
    }
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
    *arg = ev->udata;
#elif defined(EV_EVPORT)
    if (ev->portev_events & (POLLERR | POLLHUP))
    {
        rtn = EVENT_READ | EVENT_WRITE;
    }
    else
    {
        if (ev->portev_events & POLLIN)
        {
            rtn |= EVENT_READ;
        }
        if (ev->portev_events & POLLOUT)
        {
            rtn |= EVENT_WRITE;
        }
    }
    *arg = ev->portev_user;
#elif defined(EV_POLLSET)
    if (ev->revents & (POLLERR | POLLHUP))
    {
        rtn = EVENT_READ | EVENT_WRITE;
    }
    else
    {
        if (ev->revents & POLLIN)
        {
            rtn |= EVENT_READ;
        }
        if (ev->revents & POLLOUT)
        {
            rtn |= EVENT_WRITE;
        }
    }
    *fd = ev->fd;
#elif defined(EV_DEVPOLL)
    if (ev->revents & (POLLERR | POLLHUP))
    {
        rtn = EVENT_READ | EVENT_WRITE;
    }
    else
    {
        if (ev->revents & POLLIN)
        {
            rtn |= EVENT_READ;
        }
        if (ev->revents & POLLOUT)
        {
            rtn |= EVENT_WRITE;
        }
    }
    *fd = ev->fd;
#endif
    return rtn;
}
static inline void _pool_shrink(watcher_ctx *watcher, timer_ctx *timer)
{
    if (timer_elapsed_ms(timer) < SHRINK_TIME)
    {
        return;
    }
    timer_start(timer);
    pool_shrink(&watcher->pool, hashmap_count(watcher->element) / 2);
}
static void _loop_event(void *arg)
{
    watcher_ctx *watcher = (watcher_ctx *)arg;
#if defined(EV_EPOLL) || defined(EV_POLLSET) || defined(EV_DEVPOLL)
    int32_t timeout = EVENT_WAIT_TIMEOUT;
#else
    struct timespec timeout;
    fill_timespec(&timeout, EVENT_WAIT_TIMEOUT);
#endif
#ifdef EV_DEVPOLL
    struct dvpoll dvp;
    dvp.dp_fds = watcher->events;
    dvp.dp_nfds = watcher->nevents;
    dvp.dp_timeout = timeout;
#endif
#ifdef EV_EVPORT
    uint32_t nget;
#endif
    SOCKET fd = INVALID_SOCK;
    sock_ctx *skctx;
    int32_t i, cnt, ev;
    timer_ctx tmshrink;
    timer_init(&tmshrink);
    timer_start(&tmshrink);
    while (0 == watcher->stop)
    {
#if defined(EV_EPOLL)
        cnt = epoll_wait(watcher->evfd, watcher->events, watcher->nevents, timeout);
#elif defined(EV_POLLSET)
        cnt = pollset_poll(watcher->evfd, watcher->events, watcher->nevents, timeout);
#elif defined(EV_EVPORT)
        nget = 1;
        (void)port_getn(watcher->evfd, watcher->events, watcher->nevents, &nget, &timeout);
        cnt = (int32_t)nget;
#elif defined(EV_KQUEUE)
        if (watcher->nchanges >= watcher->nevents)
        {
            watcher->nevents = watcher->nchanges * 2;
            FREE(watcher->events);
            MALLOC(watcher->events, sizeof(events_t) * watcher->nevents);
        }
        cnt = kevent(watcher->evfd, watcher->changes, watcher->nchanges, watcher->events, watcher->nevents, &timeout);
        watcher->nchanges = 0;
#elif defined(EV_DEVPOLL)
        if (0 != watcher->nchanges)
        {
            if (ERR_FAILED == pwrite(watcher->evfd, watcher->changes, sizeof(changes_t) * watcher->nchanges, 0))
            {
                LOG_WARN("%s", ERRORSTR(ERRNO));
            }
            watcher->nchanges = 0;
        }
        cnt = ioctl(watcher->evfd, DP_POLL, &dvp);
#endif
        for (i = 0; i < cnt; i++)
        {
            ev = _parse_event(&watcher->events[i], &fd, (void **)&skctx);
#ifdef NO_UDATA
            skctx = _map_getskctx(watcher, fd);
#endif
            if (NULL != skctx)
            {
                skctx->ev_cb(watcher, skctx, ev);
            }
            else
            {
                LOG_ERROR("can't find socket %d in map.", (int32_t)fd);
                int32_t oldev = (EVENT_READ | EVENT_WRITE);
                _del_event(watcher, fd, &oldev, oldev, NULL);
            }
        }
        _pool_shrink(watcher, &tmshrink);
        if (0 == watcher->stop
            && cnt == watcher->nevents)
        {
            watcher->nevents *= 2;
            FREE(watcher->events);
            MALLOC(watcher->events, sizeof(events_t) * watcher->nevents);
#ifdef EV_DEVPOLL
            dvp.dp_fds = watcher->events;
            dvp.dp_nfds = watcher->nevents;
#endif
        }
    }
}
static inline void _free_element(void *item)
{
    map_element *el = (map_element *)item;
    if (SOCK_STREAM == el->sock->type)
    {
        _free_sk(el->sock);
        return;
    }
    if(SOCK_DGRAM == el->sock->type)
    {
        _free_udp(el->sock);
    }
}
static inline int32_t _init_evfd()
{
    int32_t evfd = INVALID_FD;
#if defined(EV_EPOLL)
    evfd = epoll_create1(EPOLL_CLOEXEC);
#elif defined(EV_KQUEUE)
    evfd = kqueue();
#elif defined(EV_EVPORT)
    evfd = port_create();
#elif defined(EV_POLLSET)
    evfd = pollset_create(-1);
#elif defined(EV_DEVPOLL)
    evfd = open("/dev/poll", O_RDWR);
#endif
    ASSERTAB(INVALID_FD != evfd, ERRORSTR(ERRNO));
    return evfd;
}
static struct pip_ctx *_new_pips(uint32_t npipes)
{
    pip_ctx *pips;
    MALLOC(pips, sizeof(pip_ctx) * npipes);
    for (uint32_t i = 0; i < npipes; i++)
    {
        ASSERTAB(ERR_OK == pipe(pips[i].pipes), ERRORSTR(ERRNO));
        ASSERTAB(ERR_OK == sock_nbio(pips[i].pipes[0]), "set pipe O_NONBLOCK error.");
    }
    return pips;
}
void ev_init(ev_ctx *ctx, uint32_t nthreads)
{
    ctx->nthreads = (0 == nthreads ? 1 : nthreads);
    mutex_init(&ctx->qulsnlck);
    qu_lsn_init(&ctx->qulsn, 8);
    if (ATOMIC_CAS(&_init_once, 0, 1))
    {
        _init_callback();
    }
    MALLOC(ctx->watcher, sizeof(watcher_ctx) * ctx->nthreads);
    watcher_ctx *watcher;
    for (uint32_t i = 0; i < ctx->nthreads; i++)
    {
        watcher = &ctx->watcher[i];
        watcher->index = i;
        watcher->stop = 0;
        watcher->ev = ctx;
#ifdef COMMIT_NCHANGES
        watcher->nsize = EVENT_CHANGES_CNT;
        watcher->nchanges = 0;
        MALLOC(watcher->changes, sizeof(changes_t) * watcher->nsize);
#endif
        watcher->nevents = INIT_EVENTS_CNT;
        MALLOC(watcher->events, sizeof(events_t) * watcher->nevents);
        watcher->evfd = _init_evfd();
        watcher->npipes = ctx->nthreads * 2;
        watcher->pipes = _new_pips(watcher->npipes);
        watcher->element = hashmap_new_with_allocator(_malloc, _realloc, _free,
                                                      sizeof(map_element), ONEK * 2, 0, 0, 
                                                      _map_hash, _map_compare, _free_element, NULL);
        pool_init(&watcher->pool, ONEK);
        _init_cmd(watcher);
        watcher->thevent = thread_creat(_loop_event, watcher);
    }
}
static void _free_pips(watcher_ctx *watcher)
{
    for (uint32_t i = 0; i < watcher->npipes; i++)
    {
        close(watcher->pipes[i].pipes[0]);
        close(watcher->pipes[i].pipes[1]);
    }
    FREE(watcher->pipes);
}
void ev_free(ev_ctx *ctx)
{
    uint32_t i;
    cmd_ctx cmd;
    cmd.cmd = CMD_STOP;
    for (i = 0; i < ctx->nthreads; i++)
    {
        _send_cmd(&ctx->watcher[i], ctx->watcher[i].npipes - 1, &cmd);
    }
    for (i = 0; i < ctx->nthreads; i++)
    {
        thread_join(ctx->watcher[i].thevent);
    }
    watcher_ctx *watcher;
    for (i = 0; i < ctx->nthreads; i++)
    {
        watcher = &ctx->watcher[i];
        _free_pips(watcher);
#ifdef EV_POLLSET
        pollset_destroy(watcher->evfd);
#else
        close(watcher->evfd); 
#endif
#ifdef COMMIT_NCHANGES
        FREE(watcher->changes);
#endif
        FREE(watcher->events);
        hashmap_free(watcher->element);
        pool_free(&watcher->pool);
    }
    FREE(ctx->watcher);
    struct listener_ctx **lsn;
    while (NULL != (lsn = qu_lsn_pop(&ctx->qulsn)))
    {
        _freelsn(*lsn);
    }
    qu_lsn_free(&ctx->qulsn);
    mutex_free(&ctx->qulsnlck);
}

#endif//EV_IOCP
