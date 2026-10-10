#include "event/iocp.h"
#include "containers/hashmap.h"
#include "thread/spinlock.h"
#include "utils/netutils.h"

#ifdef EV_IOCP

#define IOCP_STOP_DRAIN_TIMEOUT 3000// 停止排空整体截止(ms);超时说明有 socket close 后未从 map 摘除(bug)
exfuncs_ctx _exfuncs;// 全局扩展函数指针（AcceptEx/ConnectEx）
static atomic_t _init_once = 0;// 保证扩展函数只初始化一次
static void(*cmd_cbs[CMD_TOTAL])(watcher_ctx *watcher, cmd_ctx *cmd);// 命令回调函数表

// 通过WSAIoctl获取指定GUID的Windows扩展函数指针
static void *_iocp_exfunc(SOCKET fd, GUID *guid) {
    void *func = NULL;
    DWORD bytes = 0;
    int32_t rtn = WSAIoctl(fd,
                           SIO_GET_EXTENSION_FUNCTION_POINTER,
                           guid,
                           sizeof(GUID),
                           &func,
                           sizeof(func),
                           &bytes,
                           NULL,
                           NULL);
    ASSERTAB(rtn != SOCKET_ERROR, ERRORSTR(ERRNO));
    return func;
}
static int32_t _iocp_disconnect_iter(evsock_ctx *const *item, void *udata) {
    (void)udata;
    evsock_ctx *evsk = *item;
    //防止 ERROR socket 还有在途未被取消的
    CancelIoEx((HANDLE)evsk->sk.fd, NULL);
    _iocp_disconnect(evsk);
    return 1;
}
void _iocp_disconnect_all(watcher_ctx *watcher) {
    sockel_map_scan(watcher->element, _iocp_disconnect_iter, NULL);
}
// 初始化命令回调函数表，_iocp_cmd_drain 批量处理cmd，为了快速消费掉cmd，里面不应有耗时操作。
// 如 在_ev_send里面直接发送数据
static void _iocp_init_callback(void) {
    cmd_cbs[CMD_STOP] = _on_cmd_stop;
    cmd_cbs[CMD_ADDACP] = _on_cmd_addacp;
    cmd_cbs[CMD_CONN] = _on_cmd_conn;
    cmd_cbs[CMD_ADD] = _on_cmd_add;
    cmd_cbs[CMD_SENDTO] = _on_cmd_sendto;
    cmd_cbs[CMD_PROPS] = _on_cmd_props;
    cmd_cbs[CMD_DEFER_EXEC] = _on_cmd_defer_exec;
}
// 懒加载初始化AcceptEx/ConnectEx等扩展函数（全进程只执行一次）
static void _iocp_init_funcs(void) {
    if (ATOMIC_CAS(&_init_once, 0, 1)) {
        SOCKET fd = sock_create_cloexec(AF_INET, SOCK_STREAM, 0, 0);
        ASSERTAB(INVALID_SOCK != fd, ERRORSTR(ERRNO));
        GUID accept_uid = WSAID_ACCEPTEX;
        GUID connect_uid = WSAID_CONNECTEX;
        _exfuncs.acceptex = _iocp_exfunc(fd, &accept_uid);
        _exfuncs.connectex = _iocp_exfunc(fd, &connect_uid);
        CLOSE_SOCK(fd);
        _iocp_init_callback();
    }
}
int32_t _iocp_join(watcher_ctx *watcher, SOCKET fd) {
    if (NULL == CreateIoCompletionPort((HANDLE)fd, watcher->iocp, 0, 1)) {
        return ERR_FAILED;
    }
    SetFileCompletionNotificationModes((HANDLE)fd, FILE_SKIP_SET_EVENT_ON_HANDLE);
    return ERR_OK;
}
// 抽干命令队列并逐条执行，返回条数。抽到命令才喂告警：喂 0 会把告警阈值复位
static size_t _iocp_cmd_drain(watcher_ctx *watcher) {
    size_t cnt_total = 0;
    int32_t i, cnt;
    cmd_ctx cmds[CMD_MAX_NREAD];
    overlap_cmd_ctx *olcmd = &watcher->cmd;
    do {
        cnt = (int32_t)cmdq_pop_sc_batch(&olcmd->qu, cmds, CMD_MAX_NREAD);
        for (i = 0; i < cnt; i++) {
            cmd_cbs[cmds[i].cmd](watcher, &cmds[i]);
        }
        cnt_total += (size_t)cnt;
    } while (cnt > 0);
    if (0 != cnt_total
        && tda_check(&olcmd->tda, cnt_total)) {
        LOG_WARN("watcher %d cmd queue overload, count %zu.", watcher->index, cnt_total);
    }
    return cnt_total;
}
// 命令通道完成包回调：只抽队列不清标志，标志由 _iocp_loop_event 进等待前清（见 _send_cmd）
static void _iocp_on_cmd(watcher_ctx *watcher, evsock_ctx *evsk, DWORD bytes) {
    (void)evsk;
    (void)bytes;
    (void)_iocp_cmd_drain(watcher);
}
// 驱动 tick 并按 EVENT_CHECK_INTERVAL 节流触发 pool_shrink；返回下次 wait 超时(ms)
static inline uint32_t _iocp_loop_check(watcher_ctx *watcher, uint32_t *shrink_cnt, uint64_t *shrink_start) {
    uint64_t now_ms;
    uint32_t next_to = _evpub_tick_drive(watcher, &watcher->timer, &now_ms);
    (*shrink_cnt)++;
    if (*shrink_cnt < EVENT_CHECK_INTERVAL) {
        return next_to;
    }
    *shrink_cnt = 0;
    if (0 == now_ms) {
        now_ms = timer_cur_ms(&watcher->timer);
    }
    _evpub_pool_shrink(watcher, shrink_start, now_ms);
    return next_to;
}
timer_ctx *_evpub_watcher_timer(watcher_ctx *watcher) {
    return &watcher->timer;
}
// stop 后判断事件循环是否应退出：排空完成(element 空)或排空超时则返回 1，否则(含未 stop)返回 0
static inline int32_t _iocp_check_stop(watcher_ctx *watcher, int32_t stop, uint64_t *drain_deadline) {
    if (0 == stop) {
        return 0;
    }
    // 停止后收干 CancelIoEx 触发的在途完成:element 里的 socket 仅在其 IRP 全完成、refcount 归 0 时
    // 才被摘除,count 归 0 即无在途 IRP,sockel_map_free 才不会释放仍有在途 IRP 的 evsock_ctx(内核 write-after-free)
    // cmd 通道不建 socket 也不进 element，不影响这个计数
    if (0 == sockel_map_size(watcher->element)) {
        return 1;
    }
    uint64_t now = timer_cur_ms(&watcher->timer);
    if (0 == *drain_deadline) {
        *drain_deadline = now + IOCP_STOP_DRAIN_TIMEOUT;
    } else if (now >= *drain_deadline) {
        // 超时兜底:仍有 socket 未从 map 摘除,说明有 close 后未被完成回调移除的 socket(程序 bug),告警后退出防挂死；
        // 剩下的 socket 不释放，见 _iocp_free_watcher
        LOG_ERROR("watcher %d stop drain timeout, %u socket(s) still in map (possible leak/bug).",
            watcher->index, sockel_map_size(watcher->element));
        return 1;
    }
    return 0;
}
// 事件循环主函数（使用GetQueuedCompletionStatusEx批量获取事件）
static void _iocp_loop_event(void *arg) {
    watcher_ctx *watcher = (watcher_ctx *)arg;
    _evpub_set_cur_watcher(watcher);
    int32_t err, stop;
    ULONG i, count, nevent = INIT_EVENTS_CNT;
    evsock_ctx *evsk;
    size_t ndone;
    uint32_t shrink_cnt = 0;
    uint32_t next_to = EVENT_WAIT_TIMEOUT;
    uint64_t now_ms;
    BOOL ok = FALSE;
    LPOVERLAPPED overlap;
    LPOVERLAPPED_ENTRY tmp;
    LPOVERLAPPED_ENTRY overlappeds;
    MALLOC(overlappeds, sizeof(OVERLAPPED_ENTRY) * nevent);
    uint64_t shrink_start = timer_cur_ms(&watcher->timer);
    uint64_t drain_deadline = 0;// stop 后进入排空的截止时刻(ms);0=未进入
    for (;;) {
        stop = (int32_t)ATOMIC_GET(&watcher->stop);
        if (0 != _iocp_check_stop(watcher, stop, &drain_deadline)) {
            break;
        }
        // 醒着期间标志拿在 1，生产者 CAS 失败不投 PQCS，命令留到这里抽，丢不了。清 0 必须在抽之前、
        // 抽完才能进等待，颠倒会丢唤醒。上个命令包可能还排着没取，在途可多于一个，无害(见 _send_cmd)。
        // 停止后进等待前不再抽，残留由 ev_free 收
        if (0 == stop) {
            ATOMIC_SET(&watcher->cmd.wake_pending, 0);
            ndone = _iocp_cmd_drain(watcher);
            // 投来的回调跟命令一起在这里跑：派发完与超时返回都会回到这里。抽到 CMD_STOP 就不跑，留给 ev_free 走 fcb
            if (0 == ATOMIC_GET(&watcher->stop)) {
                ndone += _evpub_defer_exec_drain(watcher);
            }
            if (0 != ndone) {
                _iocp_flush_pending(watcher);
                // 抽到的可能正是 CMD_STOP：回循环顶交给 _iocp_check_stop，别带着旧超时进等待
                if (0 != ATOMIC_GET(&watcher->stop)) {
                    continue;
                }
                // 这批命令可能新挂了 tick，重算等待超时才能按时叫醒它
                next_to = _evpub_tick_drive(watcher, &watcher->timer, &now_ms);
            }
            if (0 != _evpub_defer_exec_pending(watcher)) {
                next_to = 0;// 回调里新投的下一轮立刻跑
            }
        }
        ok = GetQueuedCompletionStatusEx(watcher->iocp,
                                        overlappeds,
                                        nevent,
                                        &count,
                                        0 != stop ? EVENT_WAIT_TIMEOUT : next_to,
                                        FALSE);
        ATOMIC_SET_RELAXED(&watcher->cmd.wake_pending, 1);// 只是提示，被看漏无非多投一次 PQCS
        if (ok) {
            for (i = 0; i < count; i++) {
                overlap = overlappeds[i].lpOverlapped;
                if (NULL == overlap) {
                    continue;
                }
                evsk = UPCAST(overlap, evsock_ctx, overlapped);
                evsk->ev_cb(watcher, evsk, overlappeds[i].dwNumberOfBytesTransferred);
            }
            // 本轮派发完统一冲。与 uev 不同，IOCP 只在派发后与进等待前抽到命令、跑过投递时冲，超时返回不冲，
            // 故 tick 回调不得触发明文攒发
            _iocp_flush_pending(watcher);
            if (count == nevent
                && 0 == ATOMIC_GET(&watcher->stop)) {
                MALLOC(tmp, sizeof(OVERLAPPED_ENTRY) * nevent * 2);
                FREE(overlappeds);
                overlappeds = tmp;
                nevent *= 2;
            }
        } else if (WAIT_TIMEOUT != (err = ERRNO)) {
            LOG_ERROR("%s", ERRORSTR(err));
        }
        next_to = _iocp_loop_check(watcher, &shrink_cnt, &shrink_start);
    }
    _evpub_set_cur_watcher(NULL);
    LOG_INFO("net event thread %d exited.", watcher->index);
    FREE(overlappeds);
}
// AcceptEx专用线程事件循环（批量处理accept完成事件）
static void _iocp_loop_acpex(void *arg) {
    acceptex_ctx *acpex = (acceptex_ctx *)arg;
    int32_t err, loop_cnt = 0;
    ULONG i, count, nevent = INIT_EVENTS_CNT;
    evsock_ctx *evsk;
    BOOL ok;
    LPOVERLAPPED overlap;
    LPOVERLAPPED_ENTRY tmp;
    LPOVERLAPPED_ENTRY overlappeds;
    uint64_t now, last_revive = 0;
    timer_ctx timer;
    timer_init(&timer);
    MALLOC(overlappeds, sizeof(OVERLAPPED_ENTRY) * nevent);
    // 停止后立即退出,不在此排空:遗留 AcceptEx 完成由 ev_free 的 _iocp_free_acpex 统一排空(见其注释)，
    // 每个 slot 预分配的客户端 socket 由 _olp_free_acceptex 同步 take-and-close,与本线程是否先排空过无关
    while (0 == ATOMIC_GET(&acpex->stop)) {
        ok = GetQueuedCompletionStatusEx(acpex->iocp,
                                         overlappeds,
                                         nevent,
                                         &count,
                                         EVENT_WAIT_TIMEOUT,
                                         FALSE);
        if (ok) {
            for (i = 0; i < count; i++) {
                overlap = overlappeds[i].lpOverlapped;
                if (NULL == overlap) {
                    continue;
                }
                evsk = UPCAST(overlap, evsock_ctx, overlapped);
                evsk->ev_cb(acpex, evsk, overlappeds[i].dwNumberOfBytesTransferred);
            }
            if (count == nevent
                && 0 == ATOMIC_GET(&acpex->stop)) {
                MALLOC(tmp, sizeof(OVERLAPPED_ENTRY) * nevent * 2);
                FREE(overlappeds);
                overlappeds = tmp;
                nevent *= 2;
            }
        } else if (WAIT_TIMEOUT != (err = ERRNO)) {
            LOG_ERROR("%s", ERRORSTR(err));
        }
        if (0 != acpex->index) {
            continue;
        }
        loop_cnt++;
        if (loop_cnt < EVENT_CHECK_INTERVAL) {
            continue;
        }
        loop_cnt = 0;
        now = timer_cur_ms(&timer);
        if (now - last_revive >= ACCEPT_BACKOFF_MS) {
            last_revive = now;
            _olp_revive_dead(acpex->ev);
        }
    }
    LOG_INFO("accept thread %d exited.", acpex->index);
    FREE(overlappeds);
}
// sockel_map 元素释放回调：根据socket类型选择释放函数
static void _iocp_sockel_free(void *item) {
    evsock_ctx *evsk = *((evsock_ctx **)item);
    if (SOCK_STREAM == evsk->type) {
        _evpub_sk_free(evsk);
    } else {
        _iocp_free_udp(evsk);
    }
}
// 初始化watcher的命令通道（不建 socket：唤醒由 _send_cmd 直接投完成包）
static void _iocp_init_cmd(watcher_ctx *watcher) {
    overlap_cmd_ctx *olcmd = &watcher->cmd;
    olcmd->ol_r.ev_cb = _iocp_on_cmd;
    olcmd->ol_r.sk.fd = INVALID_SOCK;
    olcmd->ol_r.sk.index = watcher->index;// 命令通道恒属本 watcher,口径同 unix 侧 _uev_init_cmd
    cmdq_init(&olcmd->qu, 4 * ONEK);
    tda_init(&olcmd->tda, (size_t)(cmdq_capacity(&olcmd->qu) / QUEUE_OVERLOAD_RATIO));
}
void ev_init(ev_ctx *ctx, uint32_t nthreads, const thread_hooks *hooks) {
    ctx->nthreads = (0 == nthreads ? procscnt() : nthreads);
    ATOMIC_SET_RELAXED(&ctx->stopping, 0);
    ctx->nacpex = ctx->nthreads > 3 ? 2 : 1;
    ATOMIC_SET_RELAXED(&ctx->nlsn, 0);
    ATOMIC_SET_RELAXED(&ctx->ndead_total, 0);
    _iocp_init_funcs();
    CALLOC(ctx->watcher, ctx->nthreads, sizeof(watcher_ctx));
    watcher_ctx *watcher;
    uint32_t i;
    pool_cbs skcbs = { _evpub_sk_new, _evpub_sk_free, _evpub_sk_reset, _evpub_sk_clear };
    for (i = 0; i < ctx->nthreads; i++) {
        watcher = &ctx->watcher[i];
        watcher->index = i;
        ATOMIC_SET_RELAXED(&watcher->stop, 0);
        watcher->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);// 1线程 对同一socket操作是线程安全
        ASSERTAB(NULL != watcher->iocp, ERRORSTR(ERRNO));
        watcher->ev = ctx;
        watcher->element = sockel_map_new(ONEK, _iocp_sockel_free);
        pool_init(&watcher->pool, 0, 4 * ONEK, INIT_EVENTS_CNT, POOL_FIFO, &skcbs);
        timer_init(&watcher->timer);
        _iocp_init_cmd(watcher);
        list_init(&watcher->ticks);
        defer_exec_que_init(&watcher->defer_execs, 0);
        list_init(&watcher->flushes);
        list_init(&watcher->lingers);
        watcher->linger_tick.cb = NULL;
#if WITH_SSL
        list_init(&watcher->wpends);
        watcher->wpend_tick.cb = NULL;
#endif
        if (NULL != hooks) {
            watcher->thevent = thread_creat_hooks(_iocp_loop_event, hooks->init, hooks->exit, watcher, hooks->assist);
        } else {
            watcher->thevent = thread_creat(_iocp_loop_event, watcher);
        }
    }
    spin_init(&ctx->spin, SPIN_CNT);
    lsn_arr_init(&ctx->arrlsn, 0);
    HANDLE iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, ctx->nacpex);
    ASSERTAB(NULL != iocp, ERRORSTR(ERRNO));
    CALLOC(ctx->acpex, ctx->nacpex, sizeof(acceptex_ctx));
    acceptex_ctx *acpex;
    for (i = 0; i < ctx->nacpex; i++) {
        acpex = &ctx->acpex[i];
        acpex->index = i;
        ATOMIC_SET_RELAXED(&acpex->stop, 0);
        acpex->ev = ctx;
        acpex->iocp = iocp;
        if (NULL != hooks) {
            acpex->thacp = thread_creat_hooks(_iocp_loop_acpex, hooks->init, hooks->exit, acpex, hooks->assist);
        } else {
            acpex->thacp = thread_creat(_iocp_loop_acpex, acpex);
        }
    }
    LOG_INFO("event: %s", EV_NAME);
}
// 释放watcher的命令通道（排空队列中未处理的命令，释放内存）
static void _iocp_free_cmd(watcher_ctx *watcher) {
    cmd_ctx cmd_local;
    overlap_cmd_ctx *olcmd = &watcher->cmd;
    while (ERR_OK == cmdq_pop_sc(&olcmd->qu, &cmd_local)) {
        _cmd_drain_free(&cmd_local);
    }
    cmdq_free(&olcmd->qu);
}
static void _iocp_stop_acpex_thread(ev_ctx *ctx) {
    uint32_t i;
    // 停止 AcceptEx 线程（暂不关共用 IOCP，步骤4 仍需从中取出取消完成）
    for (i = 0; i < ctx->nacpex; i++) {
        ATOMIC_SET_RELEASE(&ctx->acpex[i].stop, 1);
        // 投递空包唤醒线程；失败时线程会在 EVENT_WAIT_TIMEOUT 后自行检测 stop 退出
        if (!PostQueuedCompletionStatus(ctx->acpex[i].iocp, 0, ((ULONG_PTR)-1), NULL)) {
            LOG_ERROR("PostQueuedCompletionStatus failed: %s", ERRORSTR(ERRNO));
        }
    }
    for (i = 0; i < ctx->nacpex; i++) {
        thread_join(ctx->acpex[i].thacp);
    }
}
// 停止并释放全部 watcher：先全部 join 再逐个释放。排空期间某个 watcher 上的回调可能给别的 watcher
// 投命令，逐个 join 逐个释放会往已释放的队列与已关的 IOCP 句柄上投(同 uev.c 的 _uev_stop_watcher)
static void _iocp_free_watcher(ev_ctx *ctx) {
    uint32_t i;
    cmd_ctx cmd = { 0 };
    cmd.cmd = CMD_STOP;
    watcher_ctx *watcher;
    for (i = 0; i < ctx->nthreads; i++) {
        watcher = &ctx->watcher[i];
        (void)_send_cmd(watcher, &cmd);
    }
    for (i = 0; i < ctx->nthreads; i++) {
        thread_join(ctx->watcher[i].thevent);
    }
    for (i = 0; i < ctx->nthreads; i++) {
        watcher = &ctx->watcher[i];
        (void)CloseHandle(watcher->iocp);
        _iocp_free_cmd(watcher);
        _evpub_defer_exec_free(watcher);
        // map 没排空说明是排空超时退出的：剩下的 socket 可能还有没完成的 IRP，内核之后还会写它的 OVERLAPPED。
        // 宁可泄漏也不释放(同监听对象的处理)，内存检查会把这份泄漏报出来
        if (0 != sockel_map_size(watcher->element)) {
            watcher->element->elfree = NULL;
        }
        sockel_map_free(watcher->element);
        pool_free(&watcher->pool);
    }
    FREE(ctx->watcher);
}
static void _iocp_free_acpex(ev_ctx *ctx) {
    DWORD bytes;
    ULONG_PTR key;
    LPOVERLAPPED overlap;
    evsock_ctx *evsk;
    uint32_t idle = 0;
    BOOL got;
    while (ATOMIC_GET(&ctx->nlsn) > 0) {
        overlap = NULL;
        got = GetQueuedCompletionStatus(ctx->acpex[0].iocp, &bytes, &key, &overlap, EVENT_WAIT_TIMEOUT);
        if (NULL != overlap) {
            evsk = UPCAST(overlap, evsock_ctx, overlapped);
            _iocp_acpex_release(evsk);
            idle = 0;
        } else if (!got) {
            // 只有真等满 EVENT_WAIT_TIMEOUT(或句柄出错)才算空转。取到 lpOverlapped==NULL 且
            // 返回成功的是 _iocp_stop_acpex_thread 投的唤醒包(未被 acpex 线程取光的残留),
            // 它是瞬时返回的,按超时计会白扣排空预算、让边界情形更早撞上下面的 break
            idle += EVENT_WAIT_TIMEOUT;
            if (idle >= IOCP_STOP_DRAIN_TIMEOUT) {
                LOG_ERROR("ev_free acpex drain timeout, %d listener(s) leaked (AcceptEx cancel completion missing).",
                          (int32_t)ATOMIC_GET(&ctx->nlsn));
                break;
            }
        }
    }
    // 关闭共用 acpex IOCP 并释放（所有 acceptex_ctx 共用同一个 iocp，只需关闭一次）
    (void)CloseHandle(ctx->acpex[0].iocp);
    FREE(ctx->acpex);
}
void ev_free(ev_ctx *ctx) {
    // 先置关停标志：此后 ev_listen/ev_connect/ev_udp 一律拒绝，免调用方拿到永不被处理的句柄
    ATOMIC_SET_RELEASE(&ctx->stopping, 1);
    // 1. 停止 AcceptEx 线程（暂不关共用 IOCP，步骤4 仍需从中取出取消完成）
    _iocp_stop_acpex_thread(ctx);
    // 2. ev_unlisten 全部残留 listener：取消在途 AcceptEx，取消完成排队到仍开着的 acpex IOCP（步骤4排空）
    _iocp_unlisten_all(ctx);
    // 3. 停止并释放所有 watcher：_iocp_free_cmd 内 CMD_ADDACP 的 _iocp_try_freelsn 此时 lsn 仍活，正确
    _iocp_free_watcher(ctx);
    // 4. 本线程独占排空 acpex IOCP 的 AcceptEx 取消完成（acpex/watcher 均已停，无并发）：
    //    _olp_on_accept_cb 见 remove==1 走释放分支只减 ref，ref 归零 → _iocp_freelsn 安全释放（内核已写完 OVERLAPPED）。
    //    排空到 nlsn 归零；连续 IOCP_STOP_DRAIN_TIMEOUT 无完成仍未清零 → LOG_ERROR（取消完成缺失=bug，宁可残留泄漏也不强释造成 UAF）。
    _iocp_free_acpex(ctx);
    lsn_arr_free(&ctx->arrlsn);
    spin_free(&ctx->spin);
}

#endif//EV_IOCP
