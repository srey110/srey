#include "event/uev.h"
#include "containers/hashmap.h"
#include "utils/netutils.h"
#include "utils/utils.h"

#ifndef EV_IOCP

static void(*cmd_cbs[CMD_TOTAL])(watcher_ctx *watcher, cmd_ctx *cmd); // 命令回调函数表

//pipe作为触发器，命令在qu里面获取
static size_t _uev_cmd_run(watcher_ctx *watcher, sock_ctx *skctx, pip_ctx *pip) {
    size_t cnt_total = 0;
    int32_t i, cnt;
    cmd_ctx cmds[CMD_MAX_NREAD];
    char ntrigger[CMD_MAX_NREAD];
    // 触发字节仅作唤醒信号，先抽干清可读态（epoll ET / MANUAL_ADD re-arm 后仅新字节再触发）
    while (read(skctx->fd, ntrigger, sizeof(ntrigger)) > 0) { }
    // 与字节数解耦,循环抽干至队列空;触发字节仅作唤醒,本轮后入队命令其字节随后必到再唤醒,不丢命令也不空转。
    // 有意不设每轮上限:fsqu_push 改为永不阻塞后队列已无背压,持续高压会推迟本 watcher 的 socket 读写
    // (过载由下方 tda_check 的 overload 告警暴露);设上限须在退出时补写自唤醒字节——触发字节已在上面抽干,
    // 不补则残留命令无人唤醒——反而引入"补写失败即命令永久滞留"的新失败模式,故保持排空语义
    do {
        cnt = (int32_t)fsqu_pop_sc_batch(&pip->qu, cmds, CMD_MAX_NREAD);
        for (i = 0; i < cnt; i++) {
            cmd_cbs[cmds[i].cmd](watcher, &cmds[i]);
        }
        cnt_total += (size_t)cnt;
    } while (cnt > 0);
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
// 如 在_ev_send里面直接发送数据
static void _uev_init_callback(void) {
    cmd_cbs[CMD_STOP] = _on_cmd_stop;
    cmd_cbs[CMD_ADDACP] = _on_cmd_addacp;
    cmd_cbs[CMD_CONN] = _on_cmd_conn;
    cmd_cbs[CMD_ADD] = _on_cmd_add;
    cmd_cbs[CMD_SENDTO] = _on_cmd_sendto;
    cmd_cbs[CMD_LSN] = _on_cmd_lsn;
    cmd_cbs[CMD_UNLSN] = _on_cmd_unlsn;
    cmd_cbs[CMD_LSN_UNREF] = _on_cmd_lsn_unref;
    cmd_cbs[CMD_PROPS] = _on_cmd_props;
}
// 将命令管道读端注册到事件循环（读事件触发_uev_cmd_loop）
static void _uev_init_cmd(watcher_ctx *watcher) {
    sock_ctx *skctx = &watcher->pipe.skpip;
    skctx->fd = watcher->pipe.pipes[0];
    skctx->events = 0;
#ifdef COMMIT_NCHANGES
    skctx->chg_round = 0;
#endif
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
// 戳不等于当轮就直接返回:nchanges 每轮轮首归零,而任何追加都同时打当轮戳,所以本轮没追加过的
// sock 数组里必无它的项。误命中(回绕/回池残留)只是多扫一遍,"该扫却跳过"构造不出来。
// 扫完不把戳归零:_usk_on_connect_cb_err 的补排就紧跟在本函数之后
void _uev_drop_changes(watcher_ctx *watcher, sock_ctx *skctx) {
#if defined(EV_KQUEUE) || defined(EV_DEVPOLL)
    if (skctx->chg_round != watcher->chg_round) {
        return;
    }
    SOCKET fd = skctx->fd;
    int32_t n = 0;
    for (int32_t i = 0; i < watcher->nchanges; i++) {
#if defined(EV_KQUEUE)
        if ((SOCKET)watcher->changes[i].ident != fd) {
#else
        if ((SOCKET)watcher->changes[i].fd != fd) {
#endif
            if (n != i) {
                watcher->changes[n] = watcher->changes[i];
            }
            n++;
        }
    }
    watcher->nchanges = n;
#else
    (void)watcher;
    (void)skctx;
#endif
}
#if defined(EV_EVPORT) || defined(EV_POLLSET) || defined(EV_DEVPOLL)
// EVENT_* 与 poll 位(POLLIN/POLLOUT)互译，evport / pollset / devpoll 三个后端共用同一张表
static inline int32_t _uev_ev2poll(int32_t ev) {
    int32_t rtn = 0;
    if (BIT_CHECK(ev, EVENT_READ)) {
        BIT_SET(rtn, POLLIN);
    }
    if (BIT_CHECK(ev, EVENT_WRITE)) {
        BIT_SET(rtn, POLLOUT);
    }
    return rtn;
}
// POLLERR / POLLHUP 一律翻成读写双就绪：真错交给随后的读写调用去撞，这里不区分
static inline int32_t _uev_poll2ev(int32_t revents) {
    if (BIT_CHECK(revents, (POLLERR | POLLHUP))) {
        return EVENT_READ | EVENT_WRITE;
    }
    int32_t rtn = 0;
    if (BIT_CHECK(revents, POLLIN)) {
        BIT_SET(rtn, EVENT_READ);
    }
    if (BIT_CHECK(revents, POLLOUT)) {
        BIT_SET(rtn, EVENT_WRITE);
    }
    return rtn;
}
#endif
#if defined(EV_EPOLL)
// EVENT_* → epoll 位。ET 位只在这里加,add 与 del 走的 EPOLL_CTL_MOD 都经过它——
// 分开写漏一处,MOD 就把边缘触发静默降级成水平触发
static inline uint32_t _uev_ev2epoll(int32_t ev) {
    uint32_t rtn = 0;
    if (BIT_CHECK(ev, EVENT_READ)) {
        BIT_SET(rtn, EPOLLIN);
    }
    if (BIT_CHECK(ev, EVENT_WRITE)) {
        BIT_SET(rtn, EPOLLOUT);
    }
#if TRIGGER_ET
    BIT_SET(rtn, EPOLLET);
#endif
    return rtn;
}
// EPOLLHUP / EPOLLERR 一律翻成读写双就绪,同 _uev_poll2ev
static inline int32_t _uev_epoll2ev(uint32_t revents) {
    if (BIT_CHECK(revents, (EPOLLHUP | EPOLLERR))) {
        return EVENT_READ | EVENT_WRITE;
    }
    int32_t rtn = 0;
    if (BIT_CHECK(revents, EPOLLIN)) {
        BIT_SET(rtn, EVENT_READ);
    }
    if (BIT_CHECK(revents, EPOLLOUT)) {
        BIT_SET(rtn, EVENT_WRITE);
    }
    return rtn;
}
#endif
int32_t _uev_add_event(watcher_ctx *watcher, SOCKET fd, int32_t *curevents, int32_t ev, sock_ctx *skctx) {
#if defined(EV_EPOLL)
    events_t epev = { 0 };
    epev.data.ptr = skctx;
    BIT_SET(ev, (*curevents));
    epev.events = _uev_ev2epoll(ev);
    if (ERR_FAILED == epoll_ctl(watcher->evfd,
                                0 == (*curevents) ? EPOLL_CTL_ADD : EPOLL_CTL_MOD,
                                fd,
                                &epev)) {
        return ERR_FAILED;
    }
    *curevents = ev;
#elif defined(EV_KQUEUE)
    if (BIT_CHECK(ev, EVENT_READ)
        && !BIT_CHECK((*curevents), EVENT_READ)) {
        BIT_SET((*curevents), EVENT_READ);
        _uev_check_changes(watcher);
        changes_t *kev = &watcher->changes[watcher->nchanges];
        EV_SET(kev, fd, EVFILT_READ, EV_ADD, 0, 0, skctx);
        watcher->nchanges++;
        skctx->chg_round = watcher->chg_round;
    }
    if (BIT_CHECK(ev, EVENT_WRITE)
        && !BIT_CHECK((*curevents), EVENT_WRITE)) {
        BIT_SET((*curevents), EVENT_WRITE);
        _uev_check_changes(watcher);
        changes_t *kev = &watcher->changes[watcher->nchanges];
        EV_SET(kev, fd, EVFILT_WRITE, EV_ADD, 0, 0, skctx);
        watcher->nchanges++;
        skctx->chg_round = watcher->chg_round;
    }
#elif defined(EV_EVPORT)
    BIT_SET(ev, (*curevents));
    int32_t pollev = _uev_ev2poll(ev);
    if (ERR_FAILED == port_associate(watcher->evfd, PORT_SOURCE_FD, fd, pollev, skctx)) {
        return ERR_FAILED;
    }
    *curevents = ev;
#elif defined(EV_POLLSET)
    (void)skctx;
    BIT_SET(ev, (*curevents));
    struct poll_ctl ctl;
    ctl.fd = fd;
    ctl.events = _uev_ev2poll(ev);
    ctl.cmd = (0 == (*curevents) ? PS_ADD : PS_MOD);
    if (0 != pollset_ctl(watcher->evfd, &ctl, 1)) {
        return ERR_FAILED;
    }
    *curevents = ev;
#elif defined(EV_DEVPOLL)
    BIT_SET((*curevents), ev);
    _uev_check_changes(watcher);
    changes_t *pfd = &watcher->changes[watcher->nchanges];
    pfd->fd = fd;
    pfd->revents = 0;
    pfd->events = (short)_uev_ev2poll(*curevents);
    watcher->nchanges++;
    skctx->chg_round = watcher->chg_round;
#endif
    return ERR_OK;
}
void _uev_del_event(watcher_ctx *watcher, SOCKET fd, int32_t *curevents, int32_t ev, sock_ctx *skctx) {
#if defined(EV_EPOLL)
    events_t epev = { 0 };
    epev.data.ptr = skctx;
    BIT_REMOVE((*curevents), ev);
    if (0 == (*curevents)) {
        (void)epoll_ctl(watcher->evfd, EPOLL_CTL_DEL, fd, &epev);
    } else {
        epev.events = _uev_ev2epoll(*curevents);
        (void)epoll_ctl(watcher->evfd, EPOLL_CTL_MOD, fd, &epev);
    }
#elif defined(EV_KQUEUE)
    if (BIT_CHECK(ev, EVENT_READ)
        && BIT_CHECK((*curevents), EVENT_READ)) {
        BIT_REMOVE((*curevents), EVENT_READ);
        _uev_check_changes(watcher);
        changes_t *kev = &watcher->changes[watcher->nchanges];
        EV_SET(kev, fd, EVFILT_READ, EV_DELETE, 0, 0, skctx);
        watcher->nchanges++;
        skctx->chg_round = watcher->chg_round;
    }
    if (BIT_CHECK(ev, EVENT_WRITE)
        && BIT_CHECK((*curevents), EVENT_WRITE)) {
        BIT_REMOVE((*curevents), EVENT_WRITE);
        _uev_check_changes(watcher);
        changes_t *kev = &watcher->changes[watcher->nchanges];
        EV_SET(kev, fd, EVFILT_WRITE, EV_DELETE, 0, 0, skctx);
        watcher->nchanges++;
        skctx->chg_round = watcher->chg_round;
    }
#elif defined(EV_EVPORT)
    BIT_REMOVE((*curevents), ev);
    if (0 == (*curevents)) {
        (void)port_dissociate(watcher->evfd, PORT_SOURCE_FD, fd);
    } else {
        ev = _uev_ev2poll(*curevents);
        (void)port_associate(watcher->evfd, PORT_SOURCE_FD, fd, ev, skctx);
    }
#elif defined(EV_POLLSET)
    (void)skctx;
    BIT_REMOVE((*curevents), ev);
    if (0 == (*curevents)) {
        struct poll_ctl ctl;
        ctl.cmd = PS_DELETE;
        ctl.events = 0;
        ctl.fd = fd;
        (void)pollset_ctl(watcher->evfd, &ctl, 1);
    } else {
        struct poll_ctl ctl;
        ctl.fd = fd;
        ctl.cmd = PS_DELETE;
        ctl.events = 0;
        (void)pollset_ctl(watcher->evfd, &ctl, 1);
        ctl.cmd = PS_ADD;
        ctl.events = _uev_ev2poll(*curevents);
        (void)pollset_ctl(watcher->evfd, &ctl, 1);
    }
#elif defined(EV_DEVPOLL)
    BIT_REMOVE((*curevents), ev);
    _uev_check_changes(watcher);
    changes_t *pfd = &watcher->changes[watcher->nchanges];
    pfd->fd = fd;
    pfd->events = POLLREMOVE;
    pfd->revents = 0;
    watcher->nchanges++;
    skctx->chg_round = watcher->chg_round;
    if (0 != (*curevents)) {
        _uev_check_changes(watcher);
        pfd = &watcher->changes[watcher->nchanges];
        pfd->fd = fd;
        pfd->revents = 0;
        pfd->events = (short)_uev_ev2poll(*curevents);
        watcher->nchanges++;
        skctx->chg_round = watcher->chg_round;
    }
#endif
}
// 解析平台事件结构体，提取事件类型掩码、fd和用户数据指针
static int32_t _uev_parse_event(events_t *ev, SOCKET *fd, void **arg) {
    int32_t rtn = 0;
    *fd = INVALID_SOCK;
    *arg = NULL;
#if defined(EV_EPOLL)
    rtn = _uev_epoll2ev(ev->events);
    *arg = ev->data.ptr;
#elif defined(EV_KQUEUE)
    if (BIT_CHECK(ev->flags, EV_ERROR)) {
        if (0 != ev->data
            && ENOENT != (int32_t)ev->data) {
            LOG_ERROR("kevent register failed on fd %d: %s.",
                      (int32_t)ev->ident, ERRORSTR((int32_t)ev->data));
            BIT_SET(rtn, EVENT_ERROR);
        }
    } else {
        if (EVFILT_READ == ev->filter) {
            BIT_SET(rtn, EVENT_READ);
        }
        if (EVFILT_WRITE == ev->filter) {
            BIT_SET(rtn, EVENT_WRITE);
        }
    }
    *arg = ev->udata;
#elif defined(EV_EVPORT)
    rtn = _uev_poll2ev(ev->portev_events);
    *arg = ev->portev_user;
#elif defined(EV_POLLSET)
    rtn = _uev_poll2ev(ev->revents);
    *fd = ev->fd;
#elif defined(EV_DEVPOLL)
    rtn = _uev_poll2ev(ev->revents);
    *fd = ev->fd;
#endif
    return rtn;
}
// 事件循环主函数（Unix平台：epoll/kqueue/evport/pollset/devpoll）
static void _uev_loop_event(void *arg) {
    watcher_ctx *watcher = (watcher_ctx *)arg;
#if defined(EV_EPOLL) || defined(EV_POLLSET) || defined(EV_DEVPOLL)
    int32_t timeout;
#else
    struct timespec timeout;
#endif
#ifdef EV_DEVPOLL
    struct dvpoll dvp;
    dvp.dp_fds = watcher->events;
    dvp.dp_nfds = watcher->nevents;
#endif
#ifdef EV_EVPORT
    uint32_t nget;
    int32_t err;
#endif
    SOCKET fd = INVALID_SOCK;
    sock_ctx *skctx;
    int32_t i, cnt, ev;
    uint32_t loop_cnt = 0, next_to = EVENT_WAIT_TIMEOUT;
    uint64_t now_ms, shrink_start = timer_cur_ms(&watcher->timer);
    //主循环
    while (0 == ATOMIC_GET(&watcher->stop)) {
#ifdef COMMIT_NCHANGES
        watcher->chg_round++;
#endif
        //设置超时时间
#if defined(EV_EPOLL) || defined(EV_POLLSET) || defined(EV_DEVPOLL)
        timeout = (int32_t)next_to;
#else
        fill_timespec(&timeout, next_to);
#endif
#ifdef EV_DEVPOLL
        dvp.dp_timeout = timeout;
#endif
#if defined(EV_EPOLL)
        cnt = epoll_wait(watcher->evfd, watcher->events, watcher->nevents, timeout);
#elif defined(EV_POLLSET)
        cnt = pollset_poll(watcher->evfd, watcher->events, watcher->nevents, timeout);
#elif defined(EV_EVPORT)
        nget = 1;
        if (ERR_FAILED == port_getn(watcher->evfd, watcher->events, watcher->nevents, &nget, &timeout)) {
            err = ERRNO;
            // port_getn 出错时不保证更新 nget:ETIME/EINTR 已按契约置为实际取回数(须处理以完成 MANUAL_ADD 重注册),
            // 硬错误强制 0,避免按预置值解析未初始化的 events[0]
            if (ETIME != err && EINTR != err) {
                nget = 0;
            }
        }
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
            if (NULL == skctx->ev_cb) {
                continue;
            }
#if defined(EV_KQUEUE)
            if (BIT_CHECK(ev, EVENT_ERROR)) {
                // 注册失败的 fd 已无 knote，此后不会再有事件，关闭须本轮同步做完：_uev_disconnect 只置
                // STATUS_ERROR 并注册 EVENT_WRITE 等回调来关，而 _uev_add_event 排队 EV_SET 前已乐观置好
                // events 位，_usk_keep_event 遂直接返回 ERR_OK 什么也不排，等不到的事件即永不关闭(fd 泄漏)。
                // 故置位后立刻派发读写事件，由 _usk_on_rw_cb / _usk_on_udp_rw 入口的 STATUS_ERROR 分支 close。
                // pipe / listen(type 为 0)不走 _uev_disconnect：它对非 SOCK_STREAM 一律 UPCAST 成 udp_ctx
                // 会越界。这两类也补不回 knote(events 位已乐观置好,_usk_keep_event 恒短路),派发一次只为
                // 排空已入队的命令,之后即永久失联,故按类别把后果写进日志——上游那条只报了 fd 与 errno
                if (SOCK_STREAM == skctx->type
                    || SOCK_DGRAM == skctx->type) {
                    _uev_disconnect(watcher, skctx, 1);
                } else if (skctx == &watcher->pipe.skpip) {
                    LOG_FATAL("watcher %d cmd pipe lost its knote, this thread no longer takes commands.",
                              watcher->index);
                } else {
                    LOG_ERROR("watcher %d listener fd %d lost its knote, no longer accepts.",
                              watcher->index, (int32_t)skctx->fd);
                }
                skctx->ev_cb(watcher, skctx, (EVENT_READ | EVENT_WRITE));
                continue;
            }
            if (0 == ev) {
                continue;// EV_ERROR 被过滤(data 为 0 / ENOENT)时无有效事件位，空掩码会让忽略 ev 的回调(_usk_on_connect_cb)误判就绪
            }
#endif
            skctx->ev_cb(watcher, skctx, ev);
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
        next_to = _evpub_tick_drive(watcher, &watcher->timer, &now_ms);
        loop_cnt++;
        if (loop_cnt < EVENT_CHECK_INTERVAL) {
            continue;
        }
        loop_cnt = 0;
        if (0 == now_ms) {
            now_ms = timer_cur_ms(&watcher->timer);
        }
        _uev_qtn_drain(watcher, now_ms);
        _evpub_pool_shrink(watcher, &shrink_start, now_ms);
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
#if defined(HAVE_PIPE2)
    // 支持 pipe2 的平台：原子设置 CLOEXEC，防被 fork+exec 的子进程继承
    ASSERTAB(ERR_OK == pipe2(pip->pipes, O_CLOEXEC), ERRORSTR(ERRNO));
#else
    // macOS 等无 pipe2：pipe 后两端补 CLOEXEC
    ASSERTAB(ERR_OK == pipe(pip->pipes), ERRORSTR(ERRNO));
    SET_CLOEXEC(pip->pipes[0]);
    SET_CLOEXEC(pip->pipes[1]);
#endif
    // 读端非阻塞：_uev_cmd_loop可用read-until-EAGAIN循环排空，不阻塞事件线程
    // 写端非阻塞：_send_cmd 写触发字节不会永久阻塞
    ASSERTAB(ERR_OK == sock_nonblock(pip->pipes[0]), ERRORSTR(ERRNO));
    ASSERTAB(ERR_OK == sock_nonblock(pip->pipes[1]), ERRORSTR(ERRNO));
    // 命令存 fsqu、pipe 仅传 1 字节信号，告警阈值按 fsqu 容量算
    fsqu_init(&pip->qu, sizeof(cmd_ctx), 4 * ONEK);
    tda_init(&pip->tda, (size_t)(fsqu_capacity(&pip->qu) / QUEUE_OVERLOAD_RATIO));
}
void ev_init(ev_ctx *ctx, uint32_t nthreads, const thread_hooks *hooks) {
    ctx->nthreads = (0 == nthreads ? procscnt() : nthreads);
    ATOMIC_SET(&ctx->stopping, 0);
    spin_init(&ctx->spin, SPIN_CNT);
    array_init(&ctx->arrlsn, sizeof(struct listener_ctx *), 0);
    _uev_init_callback();
    CALLOC(ctx->watcher, ctx->nthreads, sizeof(watcher_ctx));
    watcher_ctx *watcher;
    pool_cbs skcbs = { _evpub_sk_new, _evpub_sk_free, _evpub_sk_reset, _evpub_sk_clear };
    for (uint32_t i = 0; i < ctx->nthreads; i++) {
        watcher = &ctx->watcher[i];
        watcher->index = i;
        ATOMIC_SET(&watcher->stop, 0);
        watcher->ev = ctx;
#ifdef COMMIT_NCHANGES
        watcher->nsize = EVENT_CHANGES_CNT;
        MALLOC(watcher->changes, sizeof(changes_t) * watcher->nsize);
#endif
        watcher->nevents = INIT_EVENTS_CNT;
        MALLOC(watcher->events, sizeof(events_t) * watcher->nevents);
        watcher->evfd = _uev_init_evfd();
        _uev_new_pipe(&watcher->pipe);
        watcher->element = hashmap_new_with_allocator(_malloc, _realloc, _free,
                                                      sizeof(sock_ctx *), ONEK, 0, 0,
                                                      _evpub_sockel_hash, _evpub_sockel_compare, _uev_free_element, NULL);
        pool_init(&watcher->pool, 0, 4 * ONEK, INIT_EVENTS_CNT, 0, &skcbs);
        queue_init(&watcher->qtn, sizeof(qtn_entry), ONEK);
        list_init(&watcher->ticks);
        timer_init(&watcher->timer);
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
timer_ctx *_evpub_watcher_timer(watcher_ctx *watcher) {
    return &watcher->timer;
}
// 排空管道中未处理的命令并关闭管道fd（释放watcher前调用）
static void _uev_free_pipe(watcher_ctx *watcher) {
    int32_t j, cnt;
    cmd_ctx cmds[CMD_MAX_NREAD];
    for (;;) {
        cnt = (int32_t)fsqu_pop_sc_batch(&watcher->pipe.qu, cmds, CMD_MAX_NREAD);
        if (cnt <= 0) {
            break;
        }
        for (j = 0; j < cnt; j++) {
            _cmd_drain_free(&cmds[j]);
        }
    }
    close(watcher->pipe.pipes[0]);
    close(watcher->pipe.pipes[1]);
    fsqu_free(&watcher->pipe.qu);
}
static void _uev_stop_watcher(ev_ctx *ctx) {
    uint32_t i;
    cmd_ctx cmd = { 0 };
    cmd.cmd = CMD_STOP;
    watcher_ctx *watcher;
    for (i = 0; i < ctx->nthreads; i++) {
        watcher = &ctx->watcher[i];
        (void)_send_cmd(watcher, &cmd);
    }
    for (i = 0; i < ctx->nthreads; i++) {// 等全部停止，防 accept push到释放后的队列
        thread_join(ctx->watcher[i].thevent);
    }
}
static void _uev_free_watcher(ev_ctx *ctx) {
    uint32_t i;
    watcher_ctx *watcher;
    for (i = 0; i < ctx->nthreads; i++) {
        watcher = &ctx->watcher[i];
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
}
static void _uev_free_alllsn(ev_ctx *ctx) {
    struct listener_ctx **lsn;
    uint32_t i;
    uint32_t nlsn = array_size(&ctx->arrlsn);
    for (i = 0; i < nlsn; i++) {
        lsn = (struct listener_ctx **)array_at(&ctx->arrlsn, i);
        _uev_freelsn(*lsn);
    }
    array_free(&ctx->arrlsn);
}
void ev_free(ev_ctx *ctx) {
    // 先置关停标志再停线程：此后 ev_listen/ev_connect/ev_udp 一律拒绝，
    // 免调用方拿到"已入队但永不被 watcher 处理"的句柄
    ATOMIC_SET(&ctx->stopping, 1);
    // 先停全部 watcher 线程再释放，防 accept 向已释放队列 push
    _uev_stop_watcher(ctx);
    _uev_free_watcher(ctx);
    _uev_free_alllsn(ctx);
    spin_free(&ctx->spin);
}

#endif//EV_IOCP
