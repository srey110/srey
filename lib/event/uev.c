#include "event/uev.h"
#include "containers/hashmap.h"
#include "utils/netutils.h"
#include "utils/timer.h"
#include "utils/utils.h"

#ifndef EV_IOCP

static atomic_t _init_once = 0;                                       // 保证命令回调表只初始化一次
static void(*cmd_cbs[CMD_TOTAL])(watcher_ctx *watcher, cmd_ctx *cmd); // 命令回调函数表
// 命令管道上下文：一对匿名管道 + 读端的sock_ctx（注册到事件循环）
typedef struct pip_ctx {
    int32_t pipes[2]; // pipes[0] 读端，pipes[1] 写端
    sock_ctx skpip;   // 读端的sock_ctx（ev_cb = _cmd_loop）
}pip_ctx;

// hashmap哈希函数：以fd作为key计算哈希值
static uint64_t _map_hash(const void *item, uint64_t seed0, uint64_t seed1) {
    (void)seed0;
    (void)seed1;
    return hash((const char *)&((*(const sock_ctx **)item)->fd), sizeof(SOCKET));
}
// hashmap比较函数：比较两个sock_ctx的fd
static int _map_compare(const void *a, const void *b, void *ud) {
    (void)ud;
    return (int)((*(const sock_ctx **)a)->fd - (*(const sock_ctx **)b)->fd);
}
// 向指定管道发送命令（原子写，EAGAIN时sched_yield重试，其他错误断言失败）
// sizeof(cmd_ctx) < PIPE_BUF 保证POSIX下的原子性，即使多个生产者共享同一管道
void _send_cmd(watcher_ctx *watcher, uint32_t index, cmd_ctx *cmd) {
    int32_t erro;
    while (0 == watcher->stop
        && ERR_FAILED == write(watcher->pipes[index].pipes[1], cmd, sizeof(cmd_ctx))) {
        erro = ERRNO;
        ASSERTAB(ERR_RW_RETRIABLE(erro), ERRORSTR(erro));
        sched_yield();
    };
}
// 命令管道可读事件回调：批量读取并处理所有待处理命令
static void _cmd_loop(watcher_ctx *watcher, sock_ctx *skctx, int32_t ev) {
    int32_t i, cnt, nread;
    cmd_ctx cmds[CMD_MAX_NREAD];
    for (;;) {
        nread = read(skctx->fd, cmds, sizeof(cmds));
        if (nread <= 0) {
            break;
        }
        cnt = nread / sizeof(cmd_ctx);
        for (i = 0; i < cnt; i++) {
            cmd_cbs[cmds[i].cmd](watcher, &cmds[i]);
        }
    }
#ifdef MANUAL_ADD
    if (0 == watcher->stop) {
        ASSERTAB(ERR_OK == _add_event(watcher, skctx->fd, &skctx->events, ev, skctx), ERRORSTR(ERRNO));
    }
#else
    (void)ev;
#endif
}
// 初始化命令回调函数表
static void _init_callback(void) {
    cmd_cbs[CMD_STOP] = _on_cmd_stop;
    cmd_cbs[CMD_DISCONN] = _on_cmd_disconn;
    cmd_cbs[CMD_LSN] = _on_cmd_lsn;
    cmd_cbs[CMD_UNLSN] = _on_cmd_unlsn;
    cmd_cbs[CMD_CONN] = _on_cmd_conn;
    cmd_cbs[CMD_ADDACP] = _on_cmd_addacp;
    cmd_cbs[CMD_ADDUDP] = _on_cmd_add_udp;
    cmd_cbs[CMD_SEND] = _on_cmd_send;
    cmd_cbs[CMD_SETUD] = _on_cmd_setud;
    cmd_cbs[CMD_SSL] = _on_cmd_ssl;
}
// 将所有命令管道读端注册到事件循环（读事件触发_cmd_loop）
static void _init_cmd(watcher_ctx *watcher) {
    sock_ctx *skctx;
    for (uint32_t i = 0; i < watcher->npipes; i++) {
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
// 检查changes数组是否已满，满时扩容（kqueue）或批量提交（devpoll）
static void _check_changes(watcher_ctx *watcher) {
    if (watcher->nchanges >= watcher->nsize) {
#if defined(EV_KQUEUE)
        watcher->nsize *= 2;
        REALLOC(watcher->changes, watcher->changes, sizeof(changes_t) * watcher->nsize);
#elif defined(EV_DEVPOLL)
        if (ERR_FAILED == pwrite(watcher->evfd, watcher->changes, sizeof(changes_t) * watcher->nchanges, 0)) {
            LOG_ERROR("%s", ERRORSTR(ERRNO));
        }
        watcher->nchanges = 0;
#endif
    }
}
#endif
int32_t _add_event(watcher_ctx *watcher, SOCKET fd, int32_t *events, int32_t ev, void *arg) {
#if defined(EV_EPOLL)
    events_t epev;
    ZERO(&epev, sizeof(epev));
    epev.data.ptr = arg;
    BIT_SET(ev, (*events));
    if (BIT_CHECK(ev, EVENT_READ)) {
        BIT_SET(epev.events, EPOLLIN);
    }
    if (BIT_CHECK(ev, EVENT_WRITE)) {
        BIT_SET(epev.events, EPOLLOUT);
    }
#if TRIGGER_ET
    BIT_SET(epev.events, EPOLLET);
#endif
    if (ERR_FAILED == epoll_ctl(watcher->evfd,
        0 == (*events) ? EPOLL_CTL_ADD : EPOLL_CTL_MOD,
        fd,
        &epev)) {
        return ERR_FAILED;
    }
    *events = ev;
#elif defined(EV_KQUEUE)
    if (BIT_CHECK(ev, EVENT_READ)
        && !BIT_CHECK((*events), EVENT_READ)) {
        BIT_SET((*events), EVENT_READ);
        _check_changes(watcher);
        changes_t *kev = &watcher->changes[watcher->nchanges];
        EV_SET(kev, fd, EVFILT_READ, EV_ADD, 0, 0, arg);
        watcher->nchanges++;
    }
    if (BIT_CHECK(ev, EVENT_WRITE)
        && !BIT_CHECK((*events), EVENT_WRITE)) {
        BIT_SET((*events), EVENT_WRITE);
        _check_changes(watcher);
        changes_t *kev = &watcher->changes[watcher->nchanges];
        EV_SET(kev, fd, EVFILT_WRITE, EV_ADD, 0, 0, arg);
        watcher->nchanges++;
    }
#elif defined(EV_EVPORT)
    BIT_SET((*events), ev);
    ev = 0;
    if (BIT_CHECK((*events), EVENT_READ)) {
        BIT_SET(ev, POLLIN);
    }
    if (BIT_CHECK((*events), EVENT_WRITE)) {
        BIT_SET(ev, POLLOUT);
    }
    if (ERR_FAILED == port_associate(watcher->evfd, PORT_SOURCE_FD, fd, ev, arg)) {
        return ERR_FAILED;
    }
#elif defined(EV_POLLSET)
    BIT_SET(ev, (*events));
    struct poll_ctl ctl;
    ctl.fd = fd;
    ctl.events = 0;
    if (BIT_CHECK(ev, EVENT_READ)) {
        BIT_SET(ctl.events, POLLIN);
    }
    if (BIT_CHECK(ev, EVENT_WRITE)) {
        BIT_SET(ctl.events, POLLOUT);
    }
    ctl.cmd = (0 == (*events) ? PS_ADD : PS_MOD);
    if (0 != pollset_ctl(watcher->evfd, &ctl, 1)) {
        return ERR_FAILED;
    }
    *events = ev;
#elif defined(EV_DEVPOLL)
    BIT_SET((*events), ev);
    _check_changes(watcher);
    changes_t *pfd = &watcher->changes[watcher->nchanges];
    pfd->fd = fd;
    pfd->revents = 0;
    pfd->events = 0;
    if (BIT_CHECK((*events), EVENT_READ)) {
        BIT_SET(pfd->events, POLLIN);
    }
    if (BIT_CHECK((*events), EVENT_WRITE)) {
        BIT_SET(pfd->events, POLLOUT);
    }
    watcher->nchanges++;
#endif
    return ERR_OK;
}
void _del_event(watcher_ctx *watcher, SOCKET fd, int32_t *events, int32_t ev, void *arg) {
#if defined(EV_EPOLL)
    events_t epev;
    ZERO(&epev, sizeof(epev));
    epev.data.ptr = arg;
    BIT_REMOVE((*events), ev);
#if TRIGGER_ET
    BIT_REMOVE((*events), EPOLLET);
#endif
    if (0 == (*events)) {
        (void)epoll_ctl(watcher->evfd, EPOLL_CTL_DEL, fd, &epev);
    } else {
        if (BIT_CHECK((*events), EVENT_READ)) {
            BIT_SET(epev.events, EPOLLIN);
        }
        if (BIT_CHECK((*events), EVENT_WRITE)) {
            BIT_SET(epev.events, EPOLLOUT);
        }
#if TRIGGER_ET
        BIT_SET(epev.events, EPOLLET);
#endif
        (void)epoll_ctl(watcher->evfd, EPOLL_CTL_MOD, fd, &epev);
    }
#elif defined(EV_KQUEUE)
    if (BIT_CHECK(ev, EVENT_READ)
        && BIT_CHECK((*events), EVENT_READ)) {
        BIT_REMOVE((*events), EVENT_READ);
        _check_changes(watcher);
        changes_t *kev = &watcher->changes[watcher->nchanges];
        EV_SET(kev, fd, EVFILT_READ, EV_DELETE, 0, 0, arg);
        watcher->nchanges++;
    }
    if (BIT_CHECK(ev, EVENT_WRITE)
        && BIT_CHECK((*events), EVENT_WRITE)) {
        BIT_REMOVE((*events), EVENT_WRITE);
        _check_changes(watcher);
        changes_t *kev = &watcher->changes[watcher->nchanges];
        EV_SET(kev, fd, EVFILT_WRITE, EV_DELETE, 0, 0, arg);
        watcher->nchanges++;
    }
#elif defined(EV_EVPORT)
    BIT_REMOVE((*events), ev);
    if (0 == (*events)) {
        (void)port_dissociate(watcher->evfd, PORT_SOURCE_FD, fd);
    } else {
        ev = 0;
        if (BIT_CHECK((*events), EVENT_READ)) {
            BIT_SET(ev, POLLIN);
        }
        if (BIT_CHECK((*events), EVENT_WRITE)) {
            BIT_SET(ev, POLLOUT);
        }
        (void)port_associate(watcher->evfd, PORT_SOURCE_FD, fd, ev, arg);
    }
#elif defined(EV_POLLSET)
    BIT_REMOVE((*events), ev);
    if (0 == (*events)) {
        struct poll_ctl ctl;
        ctl.cmd = PS_DELETE;
        ctl.events = 0;
        ctl.fd = fd;
        (void)pollset_ctl(watcher->evfd, &ctl, 1);
    } else {
        struct poll_ctl ctl;
        ctl.cmd = PS_MOD;
        ctl.fd = fd;
        ctl.events = 0;
        if (BIT_CHECK((*events), EVENT_READ)) {
            BIT_SET(ctl.events, POLLIN);
        }
        if (BIT_CHECK((*events), EVENT_WRITE)) {
            BIT_SET(ctl.events, POLLOUT);
        }
        (void)pollset_ctl(watcher->evfd, &ctl, 1);
    }
#elif defined(EV_DEVPOLL)
    BIT_REMOVE((*events), ev);
    _check_changes(watcher);
    changes_t *pfd = &watcher->changes[watcher->nchanges];
    pfd->fd = fd;
    pfd->events = POLLREMOVE;
    pfd->revents = 0;
    watcher->nchanges++;
    if (0 != (*events)) {
        _check_changes(watcher);
        pfd = &watcher->changes[watcher->nchanges];
        pfd->fd = fd;
        pfd->events = 0;
        pfd->revents = 0;
        if (BIT_CHECK((*events), EVENT_READ)) {
            BIT_SET(pfd->events, POLLIN);
        }
        if (BIT_CHECK((*events), EVENT_WRITE)) {
            BIT_SET(pfd->events, POLLOUT);
        }
        watcher->nchanges++;
    }
#endif
}
// 解析平台事件结构体，提取事件类型掩码、fd和用户数据指针
static int32_t _parse_event(events_t *ev, SOCKET *fd, void **arg) {
    int32_t rtn = 0;
    *fd = INVALID_SOCK;
    *arg = NULL;
#if defined(EV_EPOLL)
    if (BIT_CHECK(ev->events, (EPOLLHUP | EPOLLERR))) {
        BIT_SET(rtn, (EVENT_READ | EVENT_WRITE));
    } else {
        if (BIT_CHECK(ev->events, EPOLLIN)) {
            BIT_SET(rtn, EVENT_READ);
        }
        if (BIT_CHECK(ev->events, EPOLLOUT)) {
            BIT_SET(rtn, EVENT_WRITE);
        }
    }
    *arg = ev->data.ptr;
#elif defined(EV_KQUEUE)
    if (EVFILT_READ == ev->filter) {
        BIT_SET(rtn, EVENT_READ);
    }
    if (EVFILT_WRITE == ev->filter) {
        BIT_SET(rtn, EVENT_WRITE);
    }
    *arg = ev->udata;
#elif defined(EV_EVPORT)
    if (BIT_CHECK(ev->portev_events, (POLLERR | POLLHUP))) {
        BIT_SET(rtn, (EVENT_READ | EVENT_WRITE));
    } else {
        if (BIT_CHECK(ev->portev_events, POLLIN)) {
            BIT_SET(rtn, EVENT_READ);
        }
        if (BIT_CHECK(ev->portev_events, POLLOUT)) {
            BIT_SET(rtn, EVENT_WRITE);
        }
    }
    *arg = ev->portev_user;
#elif defined(EV_POLLSET)
    if (BIT_CHECK(ev->revents, (POLLERR | POLLHUP))) {
        BIT_SET(rtn, (EVENT_READ | EVENT_WRITE));
    } else {
        if (BIT_CHECK(ev->revents, POLLIN)) {
            BIT_SET(rtn, EVENT_READ);
        }
        if (BIT_CHECK(ev->revents, POLLOUT)) {
            BIT_SET(rtn, EVENT_WRITE);
        }
    }
    *fd = ev->fd;
#elif defined(EV_DEVPOLL)
    if (BIT_CHECK(ev->revents, (POLLERR | POLLHUP))) {
        BIT_SET(rtn, (EVENT_READ | EVENT_WRITE));
    } else {
        if (BIT_CHECK(ev->revents, POLLIN)) {
            BIT_SET(rtn, EVENT_READ);
        }
        if (BIT_CHECK(ev->revents, POLLOUT)) {
            BIT_SET(rtn, EVENT_WRITE);
        }
    }
    *fd = ev->fd;
#endif
    return rtn;
}
// 定期收缩对象池（每SHRINK_IDLE_CNT次检查一次时钟，避免高负载时频繁syscall）
static void _pool_shrink(watcher_ctx *watcher, timer_ctx *timer, uint32_t *cnt) {
    if (++(*cnt) < SHRINK_IDLE_CNT) {
        return;
    }
    *cnt = 0;
    if (timer_elapsed_ms(timer) < SHRINK_TIME) {
        return;
    }
    timer_start(timer);
    pool_shrink(&watcher->pool, hashmap_count(watcher->element) / 2);
}
// 事件循环主函数（Unix平台：epoll/kqueue/evport/pollset/devpoll）
static void _loop_event(void *arg) {
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
    uint32_t shrink_cnt = 0;
    timer_ctx tmshrink;
    timer_init(&tmshrink);
    timer_start(&tmshrink);
    while (0 == watcher->stop) {
#if defined(EV_EPOLL)
        cnt = epoll_wait(watcher->evfd, watcher->events, watcher->nevents, timeout);
#elif defined(EV_POLLSET)
        cnt = pollset_poll(watcher->evfd, watcher->events, watcher->nevents, timeout);
#elif defined(EV_EVPORT)
        nget = 1;
        (void)port_getn(watcher->evfd, watcher->events, watcher->nevents, &nget, &timeout);
        cnt = (int32_t)nget;
#elif defined(EV_KQUEUE)
        if (watcher->nchanges >= watcher->nevents) {
            watcher->nevents = watcher->nchanges * 2;
            FREE(watcher->events);
            MALLOC(watcher->events, sizeof(events_t) * watcher->nevents);
        }
        cnt = kevent(watcher->evfd, watcher->changes, watcher->nchanges, watcher->events, watcher->nevents, &timeout);
        watcher->nchanges = 0;
#elif defined(EV_DEVPOLL)
        if (0 != watcher->nchanges) {
            if (ERR_FAILED == pwrite(watcher->evfd, watcher->changes, sizeof(changes_t) * watcher->nchanges, 0)) {
                LOG_ERROR("%s", ERRORSTR(ERRNO));
            }
            watcher->nchanges = 0;
        }
        cnt = ioctl(watcher->evfd, DP_POLL, &dvp);
#endif
        for (i = 0; i < cnt; i++) {
            ev = _parse_event(&watcher->events[i], &fd, (void **)&skctx);
#ifdef NO_UDATA
            skctx = _map_get(watcher, fd);
#endif
            if (NULL != skctx) {
                skctx->ev_cb(watcher, skctx, ev);
            }
        }
        if (0 == watcher->stop
            && cnt == watcher->nevents) {
            watcher->nevents *= 2;
            FREE(watcher->events);
            MALLOC(watcher->events, sizeof(events_t) * watcher->nevents);
#ifdef EV_DEVPOLL
            dvp.dp_fds = watcher->events;
            dvp.dp_nfds = watcher->nevents;
#endif
        }
        _pool_shrink(watcher, &tmshrink, &shrink_cnt);
    }
    LOG_INFO("net event thread %d exited.", watcher->index);
}
// hashmap元素释放回调：根据socket类型选择释放函数（管道fd type=0不释放）
static void _free_element(void *item) {
    sock_ctx *sock = *((sock_ctx **)item);
    if (SOCK_STREAM == sock->type) {
        _free_sk(sock);
        return;
    }
    if (SOCK_DGRAM == sock->type) {
        _free_udp(sock);
    }
}
// 根据编译宏创建对应平台的事件fd（epoll_create1/kqueue/port_create等）
static int32_t _init_evfd(void) {
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
// 创建npipes对匿名管道，读写两端均设为非阻塞
static struct pip_ctx *_new_pips(uint32_t npipes) {
    pip_ctx *pips;
    MALLOC(pips, sizeof(pip_ctx) * npipes);
    for (uint32_t i = 0; i < npipes; i++) {
        ASSERTAB(ERR_OK == pipe(pips[i].pipes), ERRORSTR(ERRNO));
        // 读端非阻塞：_cmd_loop可用read-until-EAGAIN循环排空，不阻塞事件线程
        // 写端非阻塞：_send_cmd不会永久阻塞，管道满时sched_yield重试
        ASSERTAB(ERR_OK == sock_nonblock(pips[i].pipes[0]), ERRORSTR(ERRNO));
        ASSERTAB(ERR_OK == sock_nonblock(pips[i].pipes[1]), ERRORSTR(ERRNO));
    }
    return pips;
}
void ev_init(ev_ctx *ctx, uint32_t nthreads) {
    ctx->nthreads = (0 == nthreads ? procscnt() : nthreads);
    spin_init(&ctx->spin, SPIN_CNT_LSN);
    arr_ptr_init(&ctx->arrlsn, 0);
    if (ATOMIC_CAS(&_init_once, 0, 1)) {
        _init_callback();
    }
    MALLOC(ctx->watcher, sizeof(watcher_ctx) * ctx->nthreads);
    watcher_ctx *watcher;
    for (uint32_t i = 0; i < ctx->nthreads; i++) {
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
        // 每个watcher只需一个管道：sizeof(cmd_ctx) < PIPE_BUF 保证POSIX原子写
        // 之前使用nthreads*2个管道减少竞争，但实测竞争不明显，多余fd浪费内核资源
        watcher->npipes = 1;
        watcher->pipes = _new_pips(watcher->npipes);
        watcher->element = hashmap_new_with_allocator(_malloc, _realloc, _free,
            sizeof(sock_ctx *), ONEK * 2, 0, 0,
            _map_hash, _map_compare, _free_element, NULL);
        pool_init(&watcher->pool, ONEK);
        _init_cmd(watcher);
        watcher->thevent = thread_creat(_loop_event, watcher);
    }
#ifdef SO_REUSEPORT 
    LOG_INFO("event: %s, SO_REUSEPORT: true.", EV_NAME);
#else
    LOG_INFO("event: %s, SO_REUSEPORT: false.", EV_NAME);
#endif
}
// 排空管道中未处理的命令并关闭管道fd（释放watcher前调用）
static void _free_pips(watcher_ctx *watcher) {
    void *data;
    sock_ctx *skctx;
    int32_t j, cnt, nread;
    cmd_ctx cmds[CMD_MAX_NREAD];
    for (uint32_t i = 0; i < watcher->npipes; i++) {
        for (;;) {
            nread = read(watcher->pipes[i].pipes[0], cmds, sizeof(cmds));
            if (nread <= 0) {
                break;
            }
            cnt = nread / sizeof(cmd_ctx);
            for (j = 0; j < cnt; j++) {
                switch (cmds[j].cmd) {
                case CMD_SEND:
                    data = (void *)cmds[j].arg;
                    FREE(data);
                    break;
                case CMD_CONN:
                    skctx = (sock_ctx *)cmds[j].arg;
                    _free_sk(skctx);
                    break;
                case CMD_ADDUDP:
                    skctx = (sock_ctx *)cmds[j].arg;
                    _free_udp(skctx);
                    break;
                default:
                    break;
                }
            }
        }
        close(watcher->pipes[i].pipes[0]);
        close(watcher->pipes[i].pipes[1]);
    }
    FREE(watcher->pipes);
}
void ev_free(ev_ctx *ctx) {
    //stop
    uint32_t i;
    cmd_ctx cmd;
    cmd.cmd = CMD_STOP;
    watcher_ctx *watcher;
    for (i = 0; i < ctx->nthreads; i++) {
        watcher = &ctx->watcher[i];
        _send_cmd(watcher, watcher->npipes - 1, &cmd);
    }
    for (i = 0; i < ctx->nthreads; i++) {
        watcher = &ctx->watcher[i];
        thread_join(watcher->thevent);
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
    //free listener
    struct listener_ctx **lsn;
    uint32_t nlsn = arr_ptr_size(&ctx->arrlsn);
    for (i = 0; i < nlsn; i++) {
        lsn = (struct listener_ctx **)arr_ptr_at(&ctx->arrlsn, i);
        _freelsn(*lsn);
    }
    arr_ptr_free(&ctx->arrlsn);
    spin_free(&ctx->spin);
}

#endif//EV_IOCP
