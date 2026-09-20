#include "event/uev.h"
#include "containers/hashmap.h"
#include "utils/netutils.h"
#include "utils/utils.h"

#ifndef EV_IOCP

static atomic_t _init_once = 0;// 保证命令回调表只初始化一次，口径同 IOCP 侧 _iocp_init_funcs
static void(*cmd_cbs[CMD_TOTAL])(watcher_ctx *watcher, cmd_ctx *cmd); // 命令回调函数表

//触发器只负责唤醒，命令在qu里面获取
static size_t _uev_cmd_run(watcher_ctx *watcher, evsock_ctx *evsk, pip_ctx *pip) {
    size_t cnt_total = 0;
    int32_t i, cnt;
    cmd_ctx cmds[CMD_MAX_NREAD];
#ifdef NO_CMD_PIPE
    (void)evsk;
#else
    int32_t rd;
    char ntrigger[8];
    // 触发字节仅作唤醒信号，读一次即清可读态（epoll ET / MANUAL_ADD re-arm 后仅新字节再触发）。
    // 敢只读一次是因为写侧有门控：_send_cmd 把 wake_pending 从 0 CAS 成 1 才写那个字节，
    // 而清零在下面、晚于本次读，读期间别人 CAS 必失败，故在途恒不超过 1 个字节（8 是余量）。
    // EINTR 必须续读，否则这次唤醒一个字节都没读到，可读态不清
    do {
        rd = (int32_t)read(evsk->sk.fd, ntrigger, sizeof(ntrigger));
    } while (ERR_FAILED == rd && EINTR == ERRNO);
#endif
    ATOMIC_SET(&pip->wake_pending, 0);
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
static void _uev_cmd_loop(watcher_ctx *watcher, evsock_ctx *evsk, int32_t ev) {
    (void)ev;
    pip_ctx *pip = UPCAST(evsk, pip_ctx, skpip);
    size_t cnt_total = _uev_cmd_run(watcher, evsk, pip);
    if (tda_check(&pip->tda, cnt_total)) {
        LOG_WARN("watcher %d cmd pipe overload, count %zu.", watcher->index, cnt_total);
    }
#ifdef MANUAL_ADD
    // 命令管道只关心读事件，硬编码 EVENT_READ 避免依赖回调入参（evport 平台下避免误注册写事件造成忙循环）
    if (0 == ATOMIC_GET(&watcher->stop)) {
        ASSERTAB(ERR_OK == _uev_add_event(watcher, evsk->sk.fd, &evsk->events, EVENT_READ, evsk), ERRORSTR(ERRNO));
    }
#endif
}
// 初始化命令回调函数表，_on_cmd 批量处理cmd，为了快速消费掉cmd，里面不应有耗时操作。
// 如 在_ev_send里面直接发送数据。
// cmd_cbs 是进程全局、且事件线程在派发热路径上裸读它，所以只许写一次：ev_init 是公开接口，
// 已有 ev_ctx 在跑时再建一个(见 test/task_acpstorm.c)就会与那些线程并发写同一批槽位
static void _uev_init_callback(void) {
    if (!ATOMIC_CAS(&_init_once, 0, 1)) {
        return;
    }
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
// 将命令唤醒源注册到事件循环（触发_uev_cmd_loop）
static void _uev_init_cmd(watcher_ctx *watcher) {
    evsock_ctx *evsk = &watcher->pipe.skpip;
    evsk->events = 0;
#ifdef COMMIT_NCHANGES
    evsk->chg_round = 0;
#endif
    evsk->type = 0;
    evsk->ev_cb = _uev_cmd_loop;
    evsk->sk.index = watcher->index;// 命令通道恒属本 watcher,同样不按 fd 取模
#ifdef NO_CMD_PIPE
#if defined(EV_KQUEUE)
    // 没有 fd 就不进 element,口径同 IOCP 侧的 _iocp_init_cmd。EV_CLEAR 让取到即自动复位;
    // udata 这里和 _send_cmd 的 NOTE_TRIGGER 那次必须挂同一个指针,只挂一次取回的是 NULL。
    // ident 取 0:knote 按 (ident, filter) 索引,与按 fd 索引的读写事件互不干扰
    evsk->sk.fd = INVALID_SOCK;
    changes_t kev;
    EV_SET(&kev, 0, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, evsk);
    ASSERTAB(ERR_FAILED != kevent(watcher->evfd, &kev, 1, NULL, 0, NULL), ERRORSTR(ERRNO));
#else
    #error "Unsupported!"
#endif
#else
    evsk->sk.fd = watcher->pipe.pipes[0];
    _evpub_sockel_add(watcher, evsk);
    ASSERTAB(ERR_OK == _uev_add_event(watcher, evsk->sk.fd, &evsk->events, EVENT_READ, evsk),
        ERRORSTR(ERRNO));
#endif
}
#ifdef COMMIT_NCHANGES
// 检查changes数组是否已满，满时扩容（kqueue）或批量提交（devpoll）
static inline void _uev_check_changes(watcher_ctx *watcher) {
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
// evsk 数组里必无它的项。误命中(回绕/回池残留)只是多扫一遍,"该扫却跳过"构造不出来。
// 扫完不把戳归零:_usk_on_connect_cb_err 的补排就紧跟在本函数之后
void _uev_drop_changes(watcher_ctx *watcher, evsock_ctx *evsk) {
#if defined(EV_KQUEUE) || defined(EV_DEVPOLL)
    if (evsk->chg_round != watcher->chg_round) {
        return;
    }
    SOCKET fd = evsk->sk.fd;
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
    (void)evsk;
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
// 分开写漏一处,MOD 就把边缘触发静默降级成水平触发。默认 TRIGGER_ET=0 即不加,见 os.h
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
int32_t _uev_add_event(watcher_ctx *watcher, SOCKET fd, int32_t *curevents, int32_t ev, evsock_ctx *evsk) {
#if defined(EV_EPOLL)
    events_t epev = { 0 };
    epev.data.ptr = evsk;
    int32_t newevents = ev | (*curevents);
    epev.events = _uev_ev2epoll(newevents);
    if (ERR_FAILED == epoll_ctl(watcher->evfd,
                                0 == (*curevents) ? EPOLL_CTL_ADD : EPOLL_CTL_MOD,
                                fd,
                                &epev)) {
        return ERR_FAILED;
    }
    *curevents = newevents;
#elif defined(EV_KQUEUE)
    if (BIT_CHECK(ev, EVENT_READ)
        && !BIT_CHECK((*curevents), EVENT_READ)) {
        BIT_SET((*curevents), EVENT_READ);
        _uev_check_changes(watcher);
        changes_t *kev = &watcher->changes[watcher->nchanges];
        EV_SET(kev, fd, EVFILT_READ, EV_ADD, 0, 0, evsk);
        watcher->nchanges++;
        evsk->chg_round = watcher->chg_round;
    }
    if (BIT_CHECK(ev, EVENT_WRITE)
        && !BIT_CHECK((*curevents), EVENT_WRITE)) {
        BIT_SET((*curevents), EVENT_WRITE);
        _uev_check_changes(watcher);
        changes_t *kev = &watcher->changes[watcher->nchanges];
        EV_SET(kev, fd, EVFILT_WRITE, EV_ADD, 0, 0, evsk);
        watcher->nchanges++;
        evsk->chg_round = watcher->chg_round;
    }
#elif defined(EV_EVPORT)
    int32_t newevents = ev | (*curevents);
    int32_t pollev = _uev_ev2poll(newevents);
    if (ERR_FAILED == port_associate(watcher->evfd, PORT_SOURCE_FD, fd, pollev, evsk)) {
        return ERR_FAILED;
    }
    *curevents = newevents;
#elif defined(EV_POLLSET)
    (void)evsk;
    int32_t newevents = ev | (*curevents);
    struct poll_ctl ctl;
    ctl.fd = fd;
    ctl.events = _uev_ev2poll(newevents);
    ctl.cmd = (0 == (*curevents) ? PS_ADD : PS_MOD);
    if (0 != pollset_ctl(watcher->evfd, &ctl, 1)) {
        return ERR_FAILED;
    }
    *curevents = newevents;
#elif defined(EV_DEVPOLL)
    BIT_SET((*curevents), ev);
    _uev_check_changes(watcher);
    changes_t *pfd = &watcher->changes[watcher->nchanges];
    pfd->fd = fd;
    pfd->revents = 0;
    pfd->events = (short)_uev_ev2poll(*curevents);
    watcher->nchanges++;
    evsk->chg_round = watcher->chg_round;
#endif
    return ERR_OK;
}
void _uev_del_event(watcher_ctx *watcher, SOCKET fd, int32_t *curevents, int32_t ev, evsock_ctx *evsk) {
#if defined(EV_EPOLL)
    events_t epev = { 0 };
    epev.data.ptr = evsk;
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
        EV_SET(kev, fd, EVFILT_READ, EV_DELETE, 0, 0, evsk);
        watcher->nchanges++;
        evsk->chg_round = watcher->chg_round;
    }
    if (BIT_CHECK(ev, EVENT_WRITE)
        && BIT_CHECK((*curevents), EVENT_WRITE)) {
        BIT_REMOVE((*curevents), EVENT_WRITE);
        _uev_check_changes(watcher);
        changes_t *kev = &watcher->changes[watcher->nchanges];
        EV_SET(kev, fd, EVFILT_WRITE, EV_DELETE, 0, 0, evsk);
        watcher->nchanges++;
        evsk->chg_round = watcher->chg_round;
    }
#elif defined(EV_EVPORT)
    BIT_REMOVE((*curevents), ev);
    if (0 == (*curevents)) {
        (void)port_dissociate(watcher->evfd, PORT_SOURCE_FD, fd);
    } else {
        int32_t pollev = _uev_ev2poll(*curevents);
        (void)port_associate(watcher->evfd, PORT_SOURCE_FD, fd, pollev, evsk);
    }
#elif defined(EV_POLLSET)
    (void)evsk;
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
    evsk->chg_round = watcher->chg_round;
    if (0 != (*curevents)) {
        _uev_check_changes(watcher);
        pfd = &watcher->changes[watcher->nchanges];
        pfd->fd = fd;
        pfd->revents = 0;
        pfd->events = (short)_uev_ev2poll(*curevents);
        watcher->nchanges++;
        evsk->chg_round = watcher->chg_round;
    }
#endif
}
// 解析平台事件结构体，提取事件类型掩码、fd和用户数据指针
static inline int32_t _uev_parse_event(events_t *ev, SOCKET *fd, void **arg) {
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
#ifdef NO_CMD_PIPE
        if (EVFILT_USER == ev->filter) {
            BIT_SET(rtn, EVENT_READ);
        }
#endif
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
    _evpub_set_cur_watcher(watcher);
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
    evsock_ctx *evsk;
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
            ev = _uev_parse_event(&watcher->events[i], &fd, (void **)&evsk);
#ifdef NO_UDATA
            evsk = _evpub_sockel_get(watcher, fd);
#endif
            if (NULL == evsk) {
                continue;
            }
            // _close_tcp 路径会清 ev_cb=NULL；qtn 隔离期内 evsk 内存活，读 ev_cb 安全
            if (NULL == evsk->ev_cb) {
                continue;
            }
#if defined(EV_KQUEUE)
            if (BIT_CHECK(ev, EVENT_ERROR)) {
                // 注册失败的 fd 已无 knote,再等事件就永不关闭,故置位后本轮同步派发读写,
                // 由入口的 STATUS_ERROR 分支就地 close；pipe / listen(type 为 0)不能走
                // _uev_disconnect(它对非 SOCK_STREAM 一律 UPCAST 成 udp_ctx 会越界),只记日志
                if (SOCK_STREAM == evsk->type
                    || SOCK_DGRAM == evsk->type) {
                    _uev_disconnect(watcher, evsk);
                } else if (evsk == &watcher->pipe.skpip) {
                    LOG_FATAL("watcher %d cmd pipe lost its knote, this thread no longer takes commands.",
                              watcher->index);
                } else {
                    LOG_ERROR("watcher %d listener fd %d lost its knote, no longer accepts.",
                              watcher->index, (int32_t)evsk->sk.fd);
                }
                evsk->ev_cb(watcher, evsk, (EVENT_READ | EVENT_WRITE));
                continue;
            }
            if (0 == ev) {
                continue;// EV_ERROR 被过滤(data 为 0 / ENOENT)时无有效事件位，空掩码会让忽略 ev 的回调(_usk_on_connect_cb)误判就绪
            }
#endif
            evsk->ev_cb(watcher, evsk, ev);
        }
        // 本轮派发完统一冲:命令回调与读回调攒下的发送都在这里发出,
        // 合并窗口是整轮派发,同一 fd 的多条 ev_send 仍合成一次 writev
        _uev_flush_pending(watcher);
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
    _evpub_set_cur_watcher(NULL);
    LOG_INFO("net event thread %d exited.", watcher->index);
}
// hashmap元素释放回调：根据socket类型选择释放函数（管道fd type=0不释放）
static void _uev_free_element(void *item) {
    evsock_ctx *evsk = *((evsock_ctx **)item);
    if (SOCK_STREAM == evsk->type) {
        _evpub_sk_free(evsk);
        return;
    }
    if (SOCK_DGRAM == evsk->type) {
        _uev_free_udp(evsk);
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
// 创建命令通道：命令一律存 fsqu，只有传唤醒信号的载体按平台分叉（NO_CMD_PIPE 下是
// kqueue 用户事件，不占 fd，故无管道可建）
static void _uev_new_pipe(pip_ctx *pip) {
#ifndef NO_CMD_PIPE
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
#endif//NO_CMD_PIPE
    // 命令存 fsqu、触发器只负责唤醒，告警阈值按 fsqu 容量算
    fsqu_init(&pip->qu, sizeof(cmd_ctx), 4 * ONEK);
    tda_init(&pip->tda, (size_t)(fsqu_capacity(&pip->qu) / QUEUE_OVERLOAD_RATIO));
}
void ev_init(ev_ctx *ctx, uint32_t nthreads, const thread_hooks *hooks) {
    ctx->nthreads = (0 == nthreads ? procscnt() : nthreads);
#if defined(EV_KQUEUE)
    // kqueue 上多线程反而更慢,epoll/IOCP 无此问题
    if (ctx->nthreads > 1) {
        LOG_WARN("kqueue with %u net threads, throughput may fall below single thread.", ctx->nthreads);
    }
#endif
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
        watcher->element = hashmap_new(sizeof(evsock_ctx *), ONEK, 0, 0,
                                       _evpub_sockel_hash, _evpub_sockel_compare, _uev_free_element, NULL);
        pool_init(&watcher->pool, 0, 4 * ONEK, INIT_EVENTS_CNT, 0, &skcbs);
        queue_init(&watcher->qtn, sizeof(qtn_entry), ONEK);
        list_init(&watcher->ticks);
        list_init(&watcher->flushes);
#if WITH_SSL
        list_init(&watcher->wpends);
        watcher->wpend_tick.cb = NULL;
#endif
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
#ifndef NO_CMD_PIPE
    close(watcher->pipe.pipes[0]);
    close(watcher->pipe.pipes[1]);
#endif
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
        // 走管道那档时 _uev_init_cmd 会把 pip_ctx::skpip（嵌入 watcher->pipe，不由 element 持有）
        // 以 type=0 注册进 element，_uev_free_element 靠 type==0 跳过它；NO_CMD_PIPE 下它压根不进表
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
