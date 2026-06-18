#include "event/uev.h"
#include "containers/hashmap.h"
#include "utils/netutils.h"
#include "utils/timer.h"
#include "utils/utils.h"

#ifndef EV_IOCP

static void(*cmd_cbs[CMD_TOTAL])(watcher_ctx *watcher, cmd_ctx *cmd); // 命令回调函数表
//pipe作为触发器，命令在qu里面获取
static size_t _uev_cmd_run(watcher_ctx *watcher, sock_ctx *skctx, pip_ctx *pip) {
    size_t cnt_total = 0;
    int32_t i, cnt, nread;
    cmd_ctx cmds[CMD_MAX_NREAD];
#if CMD_PIPE_QU
    char ntrigger[CMD_MAX_NREAD];
    for (;;) {
        nread = read(skctx->fd, ntrigger, sizeof(ntrigger));
        if (nread <= 0) {
            break;
        }
        cnt = (int32_t)fsqu_pop_sc_batch(&pip->qu, cmds, (uint32_t)nread);
        cnt_total += (size_t)cnt;
        for (i = 0; i < cnt; i++) {
            cmd_cbs[cmds[i].cmd](watcher, &cmds[i]);
        }
    }
#else
    (void)pip;
    for (;;) {
        nread = read(skctx->fd, cmds, sizeof(cmds));
        if (nread <= 0) {
            break;
        }
        cnt = (int32_t)(nread / sizeof(cmd_ctx));
        cnt_total += (size_t)cnt;
        for (i = 0; i < cnt; i++) {
            cmd_cbs[cmds[i].cmd](watcher, &cmds[i]);
        }
    }
#endif
    return cnt_total;
}
// 命令管道可读事件回调：批量读取并处理所有待处理命令
static void _uev_cmd_loop(watcher_ctx *watcher, sock_ctx *skctx, int32_t ev) {
    (void)ev;
    pip_ctx *pip = UPCAST(skctx, pip_ctx, skpip);
    size_t cnt_total = _uev_cmd_run(watcher, skctx, pip);
    if (tda_check(&pip->tda, cnt_total)) {
        LOG_WARN("watcher %d cmd pipe overload, count %zu.", watcher->index, cnt_total);
    }
#ifdef MANUAL_ADD
    // 命令管道只关心读事件，硬编码 EVENT_READ 避免依赖回调入参（evport 平台下避免误注册写事件造成忙循环）
    if (0 == ATOMIC_GET(&watcher->stop)) {
        ASSERTAB(ERR_OK == _uev_add_event(watcher, skctx->fd, &skctx->events, EVENT_READ, skctx), ERRORSTR(ERRNO));
    }
#endif
}
// 初始化命令回调函数表，_on_cmd 批量处理cmd，为了快速消费掉cmd，里面不应有耗时操作。
// 如 在_on_cmd_send里面直接发送数据
static void _uev_init_callback(void) {
    cmd_cbs[CMD_STOP]    = _on_cmd_stop;
    cmd_cbs[CMD_DISCONN] = _on_cmd_disconn;
    cmd_cbs[CMD_ADDACP]  = _on_cmd_addacp;
    cmd_cbs[CMD_CONN]    = _on_cmd_conn;
    cmd_cbs[CMD_ADD]     = _on_cmd_add;
    cmd_cbs[CMD_SEND]    = _on_cmd_send;
    cmd_cbs[CMD_SEND_MULTI] = _on_cmd_send_multi;
    cmd_cbs[CMD_SENDTO]  = _on_cmd_sendto;
    cmd_cbs[CMD_UDP_OPT] = _on_cmd_udp_opt;
    cmd_cbs[CMD_SETUD]   = _on_cmd_setud;
    cmd_cbs[CMD_SSL]     = _on_cmd_ssl;
    cmd_cbs[CMD_LSN]     = _on_cmd_lsn;
    cmd_cbs[CMD_UNLSN]   = _on_cmd_unlsn;
    cmd_cbs[CMD_LSN_UNREF] = _on_cmd_lsn_unref;
}
// 将命令管道读端注册到事件循环（读事件触发_uev_cmd_loop）
static void _uev_init_cmd(watcher_ctx *watcher) {
    sock_ctx *skctx = &watcher->pipe.skpip;
    skctx->fd = watcher->pipe.pipes[0];
    skctx->events = 0;
    skctx->type = 0;
    skctx->ev_cb = _uev_cmd_loop;
    _evpub_sockel_add(watcher, skctx);
    ASSERTAB(ERR_OK == _uev_add_event(watcher, skctx->fd, &skctx->events, EVENT_READ, skctx), ERRORSTR(ERRNO));
}
#ifdef COMMIT_NCHANGES
// 检查changes数组是否已满，满时扩容（kqueue）或批量提交（devpoll）
static void _uev_check_changes(watcher_ctx *watcher) {
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
int32_t _uev_add_event(watcher_ctx *watcher, SOCKET fd, int32_t *events, int32_t ev, void *arg) {
#if defined(EV_EPOLL)
    events_t epev = { 0 };
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
        _uev_check_changes(watcher);
        changes_t *kev = &watcher->changes[watcher->nchanges];
        EV_SET(kev, fd, EVFILT_READ, EV_ADD, 0, 0, arg);
        watcher->nchanges++;
    }
    if (BIT_CHECK(ev, EVENT_WRITE)
        && !BIT_CHECK((*events), EVENT_WRITE)) {
        BIT_SET((*events), EVENT_WRITE);
        _uev_check_changes(watcher);
        changes_t *kev = &watcher->changes[watcher->nchanges];
        EV_SET(kev, fd, EVFILT_WRITE, EV_ADD, 0, 0, arg);
        watcher->nchanges++;
    }
#elif defined(EV_EVPORT)
    BIT_SET(ev, (*events));
    int32_t pollev = 0;
    if (BIT_CHECK(ev, EVENT_READ)) {
        BIT_SET(pollev, POLLIN);
    }
    if (BIT_CHECK(ev, EVENT_WRITE)) {
        BIT_SET(pollev, POLLOUT);
    }
    if (ERR_FAILED == port_associate(watcher->evfd, PORT_SOURCE_FD, fd, pollev, arg)) {
        return ERR_FAILED;
    }
    *events = ev;
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
    _uev_check_changes(watcher);
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
void _uev_del_event(watcher_ctx *watcher, SOCKET fd, int32_t *events, int32_t ev, void *arg) {
#if defined(EV_EPOLL)
    events_t epev = { 0 };
    epev.data.ptr = arg;
    BIT_REMOVE((*events), ev);
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
        _uev_check_changes(watcher);
        changes_t *kev = &watcher->changes[watcher->nchanges];
        EV_SET(kev, fd, EVFILT_READ, EV_DELETE, 0, 0, arg);
        watcher->nchanges++;
    }
    if (BIT_CHECK(ev, EVENT_WRITE)
        && BIT_CHECK((*events), EVENT_WRITE)) {
        BIT_REMOVE((*events), EVENT_WRITE);
        _uev_check_changes(watcher);
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
    _uev_check_changes(watcher);
    changes_t *pfd = &watcher->changes[watcher->nchanges];
    pfd->fd = fd;
    pfd->events = POLLREMOVE;
    pfd->revents = 0;
    watcher->nchanges++;
    if (0 != (*events)) {
        _uev_check_changes(watcher);
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
static int32_t _uev_parse_event(events_t *ev, SOCKET *fd, void **arg) {
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
// 定期收缩对象池
static void _uev_pool_shrink(watcher_ctx *watcher, uint64_t *shrink_start, uint64_t now_ms) {
    if (now_ms - *shrink_start < SHRINK_TIME) {
        return;
    }
    *shrink_start = now_ms;
    // hashmap_count 含 1 个命令管道 sock(type=0)，偏差可忽略
    pool_shrink(&watcher->pool, (uint32_t)SHRINK_NKEEP(hashmap_count(watcher->element)), SHRINK_BUSY);
}
// 事件循环主函数（Unix平台：epoll/kqueue/evport/pollset/devpoll）
static void _uev_loop_event(void *arg) {
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
    uint64_t now_ms;
    uint64_t shrink_start = timer_cur_ms(&watcher->tm_qtn);
    //主循环
    while (0 == ATOMIC_GET(&watcher->stop)) {
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
            ev = _uev_parse_event(&watcher->events[i], &fd, (void **)&skctx);
#ifdef NO_UDATA
            skctx = _evpub_sockel_get(watcher, fd);
#endif
            if (NULL == skctx) {
                continue;
            }
            // _close_tcp 路径会清 ev_cb=NULL；qtn 隔离期内 skctx 内存活，读 ev_cb 安全
            if (NULL != skctx->ev_cb) {
                skctx->ev_cb(watcher, skctx, ev);
            }
        }
        if (0 == ATOMIC_GET(&watcher->stop)
            && cnt == watcher->nevents) {
            watcher->nevents *= 2;
            FREE(watcher->events);
            MALLOC(watcher->events, sizeof(events_t) * watcher->nevents);
#ifdef EV_DEVPOLL
            dvp.dp_fds = watcher->events;
            dvp.dp_nfds = watcher->nevents;
#endif
        }
        shrink_cnt++;
        if (shrink_cnt < EVENT_CHECK_INTERVAL) {
            continue;
        }
        shrink_cnt = 0;
        now_ms = timer_cur_ms(&watcher->tm_qtn);
        _uev_qtn_drain(watcher, now_ms);
        _uev_pool_shrink(watcher, &shrink_start, now_ms);
    }
    LOG_INFO("net event thread %d exited.", watcher->index);
}
// hashmap元素释放回调：根据socket类型选择释放函数（管道fd type=0不释放）
static void _uev_free_element(void *item) {
    sock_ctx *sock = *((sock_ctx **)item);
    if (SOCK_STREAM == sock->type) {
        _evpub_sk_free(sock);
        return;
    }
    if (SOCK_DGRAM == sock->type) {
        _uev_free_udp(sock);
    }
}
// 根据编译宏创建对应平台的事件fd（epoll_create1/kqueue/port_create等）
static int32_t _uev_init_evfd(void) {
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
    evfd = open("/dev/poll", O_RDWR | O_CLOEXEC);
#endif
    ASSERTAB(INVALID_FD != evfd, ERRORSTR(ERRNO));
#if defined(EV_KQUEUE) || defined(EV_EVPORT)
    (void)fcntl(evfd, F_SETFD, FD_CLOEXEC);
#endif
    return evfd;
}
// 创建匿名管道，读写两端均设为非阻塞
static void _uev_new_pipe(pip_ctx *pip) {
    ASSERTAB(ERR_OK == pipe(pip->pipes), ERRORSTR(ERRNO));
    // 读端非阻塞：_uev_cmd_loop可用read-until-EAGAIN循环排空，不阻塞事件线程
    // 写端非阻塞：_send_cmd不会永久阻塞，管道满时CPU_PAUSE重试
    ASSERTAB(ERR_OK == sock_nonblock(pip->pipes[0]), ERRORSTR(ERRNO));
    ASSERTAB(ERR_OK == sock_nonblock(pip->pipes[1]), ERRORSTR(ERRNO));
#if CMD_PIPE_QU
    // 命令存 fsqu、pipe 仅传 1 字节信号，告警阈值按 fsqu 容量算
    fsqu_init(&pip->qu, sizeof(cmd_ctx), 4 * ONEK);
    tda_init(&pip->tda, (size_t)(fsqu_capacity(&pip->qu) / QUEUE_OVERLOAD_RATIO));
#else
    // pipe-direct：cmd_ctx 直接进 pipe，告警阈值按 pipe 可存命令数算
    // Linux 2.6.35+ 支持 F_GETPIPE_SZ 查实际 pipe 容量（默认 64KB，失败兜底同值）；
    // 其他平台无此 API，按 16KB 估算
    int32_t pipe_cap;
#if defined(OS_LINUX)
    pipe_cap = fcntl(pip->pipes[1], F_GETPIPE_SZ);
    if (pipe_cap < 0) {
        pipe_cap = 64 * 1024;
    }
#else
    pipe_cap = 16 * 1024;
#endif
    tda_init(&pip->tda, (size_t)((pipe_cap / (int32_t)sizeof(cmd_ctx)) / QUEUE_OVERLOAD_RATIO));
#endif
}
void ev_init(ev_ctx *ctx, uint32_t nthreads, const thread_hooks *hooks) {
    ctx->nthreads = (0 == nthreads ? procscnt() : nthreads);
    spin_init(&ctx->spin, SPIN_CNT);
    array_init(&ctx->arrlsn, sizeof(struct listener_ctx *), 0);
    _uev_init_callback();
    MALLOC(ctx->watcher, sizeof(watcher_ctx) * ctx->nthreads);
    watcher_ctx *watcher;
    el_cbs skcbs = { _evpub_sk_new, _evpub_sk_free, _evpub_sk_reset, _evpub_sk_clear };
    for (uint32_t i = 0; i < ctx->nthreads; i++) {
        watcher = &ctx->watcher[i];
        watcher->index = i;
        ATOMIC_SET(&watcher->stop, 0);
        watcher->ev = ctx;
#ifdef COMMIT_NCHANGES
        watcher->nsize = EVENT_CHANGES_CNT;
        watcher->nchanges = 0;
        MALLOC(watcher->changes, sizeof(changes_t) * watcher->nsize);
#endif
        watcher->nevents = INIT_EVENTS_CNT;
        MALLOC(watcher->events, sizeof(events_t) * watcher->nevents);
        watcher->evfd = _uev_init_evfd();
        // 每个watcher单管道：sizeof(cmd_ctx) < PIPE_BUF 保证POSIX原子写
        _uev_new_pipe(&watcher->pipe);
        watcher->element = hashmap_new_with_allocator(_malloc, _realloc, _free,
                                                      sizeof(sock_ctx *), ONEK, 0, 0,
                                                      _evpub_sockel_hash, _evpub_sockel_compare, _uev_free_element, NULL);
        pool_init(&watcher->pool, 0, 4 * ONEK, INIT_EVENTS_CNT, 0, &skcbs);
        queue_init(&watcher->qtn, sizeof(qtn_entry), ONEK);
        timer_init(&watcher->tm_qtn);
        timer_start(&watcher->tm_qtn);
        _uev_init_cmd(watcher);
        if (NULL != hooks) {
            watcher->thevent = thread_creat_hooks(_uev_loop_event, hooks->init, hooks->exit, watcher, hooks->assist);
        } else {
            watcher->thevent = thread_creat(_uev_loop_event, watcher);
        }
    }
#ifdef SO_REUSEPORT
    LOG_INFO("event: %s, SO_REUSEPORT: true.", EV_NAME);
#else
    LOG_INFO("event: %s, SO_REUSEPORT: false.", EV_NAME);
#endif
}
// 排空管道中未处理的命令并关闭管道fd（释放watcher前调用）
static void _uev_free_pipe(watcher_ctx *watcher) {
    void *data;
    sock_ctx *skctx;
    int32_t j, cnt;
    cmd_ctx cmds[CMD_MAX_NREAD];
    for (;;) {
#if CMD_PIPE_QU
        cnt = (int32_t)fsqu_pop_sc_batch(&watcher->pipe.qu, cmds, CMD_MAX_NREAD);
        if (cnt <= 0) {
            break;
        }
#else
        int32_t nread = read(watcher->pipe.pipes[0], cmds, sizeof(cmds));
        if (nread <= 0) {
            break;
        }
        cnt = (int32_t)(nread / sizeof(cmd_ctx));
#endif
        for (j = 0; j < cnt; j++) {
            switch (cmds[j].cmd) {
            // CMD_SEND 持有裸 payload；CMD_SENDTO 持有 [netaddr_ctx + payload]
            // 一整段 MALLOC，关闭路径都只需 FREE(arg) 释放整段
            case CMD_SEND:
            case CMD_SENDTO:
                data = (void *)cmds[j].arg;
                FREE(data);
                break;
            // CMD_SEND_MULTI 持有 shared_data*；多 fd 共享,归还本 fd 引用归 0 时释放
            case CMD_SEND_MULTI: {
                shared_data *pack = (shared_data *)cmds[j].arg;
                if (1 == ATOMIC_ADD(&pack->ref, -1)) {
                    FREE(pack->data);
                    FREE(pack);
                }
                break;
            }
            // CMD_UDP_OPT 持有 udp_opt_arg*；watcher 退出时未执行的命令直接释放参数包
            case CMD_UDP_OPT: {
                udp_opt_arg *udp_arg = (udp_opt_arg *)cmds[j].arg;
                FREE(udp_arg);
                break;
            }
            case CMD_CONN:
                skctx = (sock_ctx *)cmds[j].arg;
                _evpub_sk_free(skctx);
                break;
            case CMD_ADD:
                skctx = (sock_ctx *)cmds[j].arg;
                if (SOCK_STREAM == skctx->type) {
                    _evpub_sk_free(skctx);
                } else {
                    _uev_free_udp(skctx);
                }
                break;
            case CMD_UNLSN:
                // CMD_UNLSN：减 _cmd_listen 时建立的每 watcher 监听占位 ref
                // CMD_ADDACP：减 _on_accept_cb 跨投递前的 ref 占位
                // 两者都需关闭 fd（listen 端口 / 未注册的 accept 连接）+ _uev_try_freelsn
            case CMD_ADDACP:
                CLOSE_SOCK(cmds[j].fd);
                _uev_try_freelsn((struct listener_ctx *)cmds[j].arg);
                break;
            case CMD_LSN_UNREF:
                // ev_unlisten 末尾减占位 ref: worker 已退出 loop, 无 events[] 迭代, 立即释放安全
                _uev_try_freelsn((struct listener_ctx *)cmds[j].arg);
                break;
            default:
                break;
            }
        }
    }
    close(watcher->pipe.pipes[0]);
    close(watcher->pipe.pipes[1]);
#if CMD_PIPE_QU
    fsqu_free(&watcher->pipe.qu);
#endif
}
void ev_free(ev_ctx *ctx) {
    //stop
    uint32_t i;
    cmd_ctx cmd;
    cmd.cmd = CMD_STOP;
    watcher_ctx *watcher;
    for (i = 0; i < ctx->nthreads; i++) {
        watcher = &ctx->watcher[i];
        _send_cmd(watcher, &cmd);
    }
    for (i = 0; i < ctx->nthreads; i++) {
        watcher = &ctx->watcher[i];
        thread_join(watcher->thevent);
        // _uev_init_cmd 将 pip_ctx::skpip（嵌入 watcher->pipe）以 type=0 注册进 element；
        // 必须先 hashmap_free 再 _uev_free_pipe，否则 _uev_free_element 读 sock->type 时访问已释放内存。
        hashmap_free(watcher->element);
        pool_free(&watcher->pool);
        _uev_free_pipe(watcher);
        // worker 已退出 _uev_loop_event, 兜底 flush 隔离队列剩余对象
        _uev_qtn_flush(watcher);
#ifdef EV_POLLSET
        pollset_destroy(watcher->evfd);
#else
        close(watcher->evfd);
#endif
#ifdef COMMIT_NCHANGES
        FREE(watcher->changes);
#endif
        FREE(watcher->events);
    }
    FREE(ctx->watcher);
    //free listener
    struct listener_ctx **lsn;
    uint32_t nlsn = array_size(&ctx->arrlsn);
    for (i = 0; i < nlsn; i++) {
        lsn = (struct listener_ctx **)array_at(&ctx->arrlsn, i);
        _uev_freelsn(*lsn);
    }
    array_free(&ctx->arrlsn);
    spin_free(&ctx->spin);
}

#endif//EV_IOCP
