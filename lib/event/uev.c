#include "event/uev.h"
#include "hashmap.h"
#include "netutils.h"
#include "timer.h"

#ifndef EV_IOCP

#define CMD_MAX_NREAD                 64
static volatile atomic_t _init_once = 0;
static void(*cmd_cbs[CMD_TOTAL])(watcher_ctx *watcher, cmd_ctx *cmd, int32_t *stop);
typedef struct pip_ctx
{
    int32_t pipes[2];
    sock_ctx skpip;
}pip_ctx;

static inline uint64_t _map_hash(const void *item, uint64_t seed0, uint64_t seed1)
{
    SOCKET fd = ((const map_element *)item)->fd;
    return FD_HASH(fd);
}
static inline int _map_compare(const void *a, const void *b, void *ud)
{
    return (int)(((const map_element *)a)->fd - ((const map_element *)b)->fd);
}
void _cmd_send(watcher_ctx *watcher, uint32_t index, cmd_ctx *cmd)
{
    ASSERTAB(sizeof(cmd_ctx) == write(watcher->pipes[index].pipes[1], cmd, sizeof(cmd_ctx)), "pipe write error.");
}
static void _cmd_loop(watcher_ctx *watcher, sock_ctx *skctx, int32_t ev, int32_t *stop)
{
    int32_t i, cnt, nread;
    cmd_ctx cmds[CMD_MAX_NREAD];
    while ((nread = read(skctx->fd, cmds, sizeof(cmds))) > 0)
    {
        cnt = nread / sizeof(cmd_ctx);
        for (i = 0; i < cnt; i++)
        {
            cmd_cbs[cmds[i].cmd](watcher, &cmds[i], stop);
        }
    }
#ifdef EV_EVPORT
    ASSERTAB(ERR_OK == _add_event(watcher, skctx->fd, &skctx->events, ev, skctx), "add pipe in loop error.");
#endif
}
static void _init_callback()
{
    cmd_cbs[CMD_STOP] = _on_cmd_stop;
    cmd_cbs[CMD_DISCONN] = _on_cmd_disconn;
    cmd_cbs[CMD_LSN] = _on_cmd_lsn;
    cmd_cbs[CMD_CONN] = _on_cmd_conn;
    cmd_cbs[CMD_ADD] = _on_cmd_add;
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
        skctx->ev_cb = _cmd_loop;
        ASSERTAB(ERR_OK == _add_event(watcher, skctx->fd, &skctx->events, EVENT_READ, skctx), "add pipe in loop error");
    }
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
static inline int32_t _parse_event(events_t *ev, void **arg)
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
    if (ev->portev_events & POLLIN)
    {
        rtn |= EVENT_READ;
    }
    if (ev->portev_events & POLLOUT)
    {
        rtn |= EVENT_WRITE;
    }
    *arg = ev->portev_user;
#endif
    return rtn;
}
static inline void _pool_shrink(watcher_ctx *watcher, timer_ctx *timer)
{
    uint64_t elapsed = timer_elapsed_ms(timer);
    if (elapsed < SHRINK_TIME)
    {
        return;
    }
    timer_start(timer);
    pool_shrink(&watcher->pool, hashmap_count(watcher->element) / 2);
}
static void _loop_event(void *arg)
{
    watcher_ctx *watcher = (watcher_ctx *)arg;
    _init_cmd(watcher);
#if defined(EV_EVPORT)
    uint32_t nget;
#endif
    sock_ctx *skctx;    
    int32_t i, cnt, ev, stop = 0;
    timer_ctx tmshrink;
    timer_init(&tmshrink);
    timer_start(&tmshrink);
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
            ev = _parse_event(&watcher->events[i], (void **)&skctx);
            if (NULL != skctx)
            {
                skctx->ev_cb(watcher, skctx, ev, &stop);
            }
        }
        _pool_shrink(watcher, &tmshrink);
        if (0 == stop
            && cnt == watcher->nevent
            && watcher->nevent < MAX_EVENTS_CNT)
        {
            FREE(watcher->events);
            watcher->nevent *= 2;
            MALLOC(watcher->events, sizeof(events_t) * watcher->nevent);
        }
    }
}
static inline void _free_mapitem(void *item)
{
    _free_sk(((map_element *)item)->sock);
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
    ctx->stop = 0;
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
        watcher->ev = ctx;
#if defined(EV_KQUEUE)
        watcher->nsize = INIT_EVENTS_CNT;
        watcher->nchange = 0;
        MALLOC(watcher->changelist, sizeof(events_t) * watcher->nsize);
#endif
        watcher->nevent = INIT_EVENTS_CNT;
        MALLOC(watcher->events, sizeof(events_t) * watcher->nevent);
        watcher->evfd = _init_evfd();
        watcher->npipes = ctx->nthreads * 2;
        watcher->pipes = _new_pips(watcher->npipes);
        watcher->element = hashmap_new_with_allocator(_malloc, _realloc, _free,
            sizeof(map_element), ONEK * 4, 0, 0, _map_hash, _map_compare, _free_mapitem, NULL);
        pool_init(&watcher->pool, ONEK);
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
    ctx->stop = 1;
    cmd_ctx cmd;
    cmd.cmd = CMD_STOP;
    for (i = 0; i < ctx->nthreads; i++)
    {
        _cmd_send(&ctx->watcher[i], ctx->watcher[i].npipes - 1, &cmd);
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
        close(watcher->evfd);
#if defined(EV_KQUEUE)
        FREE(watcher->changelist);
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
