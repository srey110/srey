#include "event/iocp.h"
#include "containers/hashmap.h"
#include "thread/spinlock.h"
#include "utils/netutils.h"

#ifdef EV_IOCP

#define IOCP_STOP_DRAIN_TIMEOUT  3000  // 停止排空整体截止(ms);超时说明有 socket close 后未从 map 摘除(bug)
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
static bool _iocp_disconnect_iter(const void *item, void *udata) {
    (void)udata;
    sock_ctx *sk = *((sock_ctx **)item);
    //防止 ERROR socket 还有在途未被取消的
    CancelIoEx((HANDLE)sk->fd, NULL);
    _iocp_disconnect(sk, 1);
    return true;
}
void _iocp_disconnect_all(watcher_ctx *watcher) {
    hashmap_scan(watcher->element, _iocp_disconnect_iter, NULL);
}
// 初始化命令回调函数表，_iocp_on_cmd 批量处理cmd，为了快速消费掉cmd，里面不应有耗时操作。
// 如 在_ev_send里面直接发送数据
static void _iocp_init_callback(void) {
    cmd_cbs[CMD_STOP] = _on_cmd_stop;
    cmd_cbs[CMD_ADDACP] = _on_cmd_addacp;
    cmd_cbs[CMD_CONN] = _on_cmd_conn;
    cmd_cbs[CMD_ADD] = _on_cmd_add;
    cmd_cbs[CMD_SENDTO] = _on_cmd_sendto;
    cmd_cbs[CMD_PROPS] = _on_cmd_props;
}
// 懒加载初始化AcceptEx/ConnectEx等扩展函数（全进程只执行一次）
static void _iocp_init_funcs(void) {
    if (ATOMIC_CAS(&_init_once, 0, 1)) {
        SOCKET fd = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, 0);
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
    return ERR_OK;
}
// 命令管道可读事件回调：批量窃取队列后无锁处理
static void _iocp_on_cmd(watcher_ctx *watcher, sock_ctx *skctx, DWORD bytes) {
    size_t cnt_total = 0;
    int32_t i, cnt;
    char ntrigger[CMD_MAX_NREAD];
    cmd_ctx cmds[CMD_MAX_NREAD];
    overlap_cmd_ctx *olcmd = UPCAST(skctx, overlap_cmd_ctx, ol_r);
    // 触发字节仅作唤醒信号，先抽干清可读态（零字节 WSARecv re-arm 只在新字节到达时再触发）
    while (recv(olcmd->ol_r.fd, ntrigger, sizeof(ntrigger), 0) > 0) { }
    // 与字节数解耦全量抽干：队头被并发生产者占槽未发布时本轮提前停，其触发字节随后必到再唤醒补齐，不丢命令也不空转
    do {
        cnt = (int32_t)fsqu_pop_sc_batch(&olcmd->qu, cmds, CMD_MAX_NREAD);
        for (i = 0; i < cnt; i++) {
            cmd_cbs[cmds[i].cmd](watcher, &cmds[i]);
        }
        cnt_total += (size_t)cnt;
    } while (cnt > 0);
    if (tda_check(&olcmd->tda, cnt_total)) {
        LOG_WARN("watcher %d cmd queue overload, count %zu.", watcher->index, cnt_total);
    }
    if (0 == ATOMIC_GET(&watcher->stop)) {
        ASSERTAB(ERR_OK == _iocp_post_recv(&olcmd->ol_r, &olcmd->bytes, &olcmd->flag, &olcmd->wsabuf, 1), ERRORSTR(ERRNO));
    }
}
// 驱动 tick 并按 EVENT_CHECK_INTERVAL 节流触发 pool_shrink；返回下次 wait 超时(ms)
static uint32_t _iocp_loop_check(watcher_ctx *watcher, uint32_t *shrink_cnt, uint64_t *shrink_start) {
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
static int32_t _iocp_check_stop(watcher_ctx *watcher, int32_t stop, uint64_t *drain_deadline) {
    if (0 == stop) {
        return 0;
    }
    // 停止后收干 CancelIoEx 触发的在途完成:element 里的 socket 仅在其 IRP 全完成、refcount 归 0 时
    // 才被摘除,count 归 0 即无在途 IRP,hashmap_free 才不会释放仍有在途 IRP 的 sock_ctx(内核 write-after-free)
    // cmd socket 在处理完 CMD_STOP 命令已不再投递
    if (0 == hashmap_count(watcher->element)) {
        return 1;
    }
    uint64_t now = timer_cur_ms(&watcher->timer);
    if (0 == *drain_deadline) {
        *drain_deadline = now + IOCP_STOP_DRAIN_TIMEOUT;
    } else if (now >= *drain_deadline) {
        // 超时兜底:仍有 socket 未从 map 摘除,说明有 close 后未被完成回调移除的 socket(程序 bug),告警后退出防挂死
        LOG_ERROR("watcher %d stop drain timeout, %zu socket(s) still in map (possible leak/bug).",
            watcher->index, hashmap_count(watcher->element));
        return 1;
    }
    return 0;
}
// 事件循环主函数（使用GetQueuedCompletionStatusEx批量获取事件）
static void _iocp_loop_event(void *arg) {
    watcher_ctx *watcher = (watcher_ctx *)arg;
    int32_t err, stop;
    ULONG i, count, nevent = INIT_EVENTS_CNT;
    sock_ctx *sock;
    uint32_t shrink_cnt = 0;
    uint32_t next_to = EVENT_WAIT_TIMEOUT;
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
        ok = GetQueuedCompletionStatusEx(watcher->iocp,
                                        overlappeds,
                                        nevent,
                                        &count,
                                        0 != stop ? EVENT_WAIT_TIMEOUT : next_to,
                                        FALSE);
        if (ok) {
            for (i = 0; i < count; i++) {
                overlap = overlappeds[i].lpOverlapped;
                if (NULL == overlap) {
                    continue;
                }
                sock = UPCAST(overlap, sock_ctx, overlapped);
                sock->ev_cb(watcher, sock, overlappeds[i].dwNumberOfBytesTransferred);
            }
            if (0 == ATOMIC_GET(&watcher->stop)) {
                if (count == nevent) {
                    MALLOC(tmp, sizeof(OVERLAPPED_ENTRY) * nevent * 2);
                    FREE(overlappeds);
                    overlappeds = tmp;
                    nevent *= 2;
                }
            }
        } else if (WAIT_TIMEOUT != (err = ERRNO)) {
            LOG_ERROR("%s", ERRORSTR(err));
        }
        next_to = _iocp_loop_check(watcher, &shrink_cnt, &shrink_start);
    }
    LOG_INFO("net event thread %d exited.", watcher->index);
    FREE(overlappeds);
}
// AcceptEx专用线程事件循环（批量处理accept完成事件）
static void _iocp_loop_acpex(void *arg) {
    acceptex_ctx *acpex = (acceptex_ctx *)arg;
    int32_t err, stop;
    ULONG i, count, nevent = INIT_EVENTS_CNT;
    sock_ctx *sock;
    BOOL ok = FALSE;
    LPOVERLAPPED overlap;
    LPOVERLAPPED_ENTRY tmp;
    LPOVERLAPPED_ENTRY overlappeds;
    MALLOC(overlappeds, sizeof(OVERLAPPED_ENTRY) * nevent);
    while (0 == (stop = (int32_t)ATOMIC_GET(&acpex->stop))
           || (stop && ok)) {
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
                sock = UPCAST(overlap, sock_ctx, overlapped);
                sock->ev_cb(acpex, sock, overlappeds[i].dwNumberOfBytesTransferred);
            }
            if (0 == ATOMIC_GET(&acpex->stop)) {
                if (count == nevent) {
                    MALLOC(tmp, sizeof(OVERLAPPED_ENTRY) * nevent * 2);
                    FREE(overlappeds);
                    overlappeds = tmp;
                    nevent *= 2;
                }
            } else {
                if (count < nevent) {//退出时已经没数据了
                    break;
                }
            }
        } else if (WAIT_TIMEOUT == (err = ERRNO)) {
            if (0 == ATOMIC_GET(&acpex->stop)) {//防止退出时第一次while 判断错误
                ok = TRUE;
            }
        } else {
            LOG_ERROR("%s", ERRORSTR(err));
        }
    }
    LOG_INFO("accept thread %d exited.", acpex->index);
    FREE(overlappeds);
}
// hashmap元素释放回调：根据socket类型选择释放函数
static void _iocp_sockel_free(void *item) {
    sock_ctx *sock = *((sock_ctx **)item);
    if (SOCK_STREAM == sock->type) {
        _evpub_sk_free(sock);
    } else {
        _iocp_free_udp(sock);
    }
}
// 初始化watcher的命令通道（sock_pair + IOCP注册 + 提交首次WSARecv）
static void _iocp_init_cmd(watcher_ctx *watcher) {
    SOCKET pair[2];
    overlap_cmd_ctx *olcmd = &watcher->cmd;
    ASSERTAB(ERR_OK == sock_pair(pair), ERRORSTR(ERRNO));
    olcmd->ol_r.ev_cb = _iocp_on_cmd;
    olcmd->ol_r.fd = pair[0];
    olcmd->ol_r.type = 0;
    olcmd->fd = pair[1];
    fsqu_init(&olcmd->qu, sizeof(cmd_ctx), 4 * ONEK);
    tda_init(&olcmd->tda, (size_t)(fsqu_capacity(&olcmd->qu) / QUEUE_OVERLOAD_RATIO));
    olcmd->wsabuf.IOV_PTR_FIELD = NULL;
    olcmd->wsabuf.IOV_LEN_FIELD = 0;
    ASSERTAB(ERR_OK == _iocp_join(watcher, olcmd->ol_r.fd), ERRORSTR(ERRNO));
    ASSERTAB(ERR_OK == _iocp_post_recv(&olcmd->ol_r, &olcmd->bytes, &olcmd->flag, &olcmd->wsabuf, 1), ERRORSTR(ERRNO));
}
void ev_init(ev_ctx *ctx, uint32_t nthreads, const thread_hooks *hooks) {
    ctx->nthreads = (0 == nthreads ? procscnt() : nthreads);
    ctx->nacpex = ctx->nthreads > 3 ? 2 : 1;
    ATOMIC_SET(&ctx->nlsn, 0);
    _iocp_init_funcs();
    MALLOC(ctx->watcher, sizeof(watcher_ctx) * ctx->nthreads);
    watcher_ctx *watcher;
    uint32_t i;
    el_cbs skcbs = { _evpub_sk_new, _evpub_sk_free, _evpub_sk_reset, _evpub_sk_clear };
    for (i = 0; i < ctx->nthreads; i++) {
        watcher = &ctx->watcher[i];
        watcher->index = i;
        ATOMIC_SET(&watcher->stop, 0);
        watcher->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);// 1线程 对同一socket操作是线程安全
        ASSERTAB(NULL != watcher->iocp, ERRORSTR(ERRNO));
        watcher->ev = ctx;
        watcher->element = hashmap_new_with_allocator(_malloc, _realloc, _free,
                                                      sizeof(sock_ctx *), ONEK, 0, 0,
                                                      _evpub_sockel_hash, _evpub_sockel_compare, _iocp_sockel_free, NULL);
        pool_init(&watcher->pool, 0, 4 * ONEK, INIT_EVENTS_CNT, 0, &skcbs);
        timer_init(&watcher->timer);
        _iocp_init_cmd(watcher);
        list_init(&watcher->ticks);
        if (NULL != hooks) {
            watcher->thevent = thread_creat_hooks(_iocp_loop_event, hooks->init, hooks->exit, watcher, hooks->assist);
        } else {
            watcher->thevent = thread_creat(_iocp_loop_event, watcher);
        }
    }
    spin_init(&ctx->spin, SPIN_CNT);
    array_init(&ctx->arrlsn, sizeof(struct listener_ctx *), 0);
    HANDLE iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, ctx->nacpex);
    ASSERTAB(NULL != iocp, ERRORSTR(ERRNO));
    MALLOC(ctx->acpex, sizeof(acceptex_ctx) * ctx->nacpex);
    acceptex_ctx *acpex;
    for (i = 0; i < ctx->nacpex; i++) {
        acpex = &ctx->acpex[i];
        acpex->index = i;
        ATOMIC_SET(&acpex->stop, 0);
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
// 释放watcher的命令通道（排空队列中未处理的命令，释放内存，关闭socket对）
static void _iocp_free_cmd(watcher_ctx *watcher) {
    cmd_ctx cmd_local;
    void *data;
    sock_ctx *skctx;
    overlap_cmd_ctx *olcmd = &watcher->cmd;
    while (ERR_OK == fsqu_pop_sc(&olcmd->qu, &cmd_local)) {
        switch (cmd_local.cmd) {
        case CMD_SENDTO:
            data = cmd_local.args.sendto.data;
            FREE(data);
            break;
        case CMD_CONN:
            skctx = cmd_local.args.conn.skctx;
            _evpub_sk_free(skctx);
            break;
        case CMD_ADD:
            skctx = cmd_local.args.skctx;
            if (SOCK_STREAM == skctx->type) {
                _evpub_sk_free(skctx);
            } else {
                _iocp_free_udp(skctx);
            }
            break;
        case CMD_ADDACP:
            // fd 是 accept 到的连接，未能加入事件循环；同时配对 _on_accept_cb
            // path 3 投递前 ref++ 占位的减法，ref 归零时释放 lsn
            CLOSE_SOCK(cmd_local.sk.fd);
            _iocp_try_freelsn(cmd_local.args.lsn);
            break;
        case CMD_PROPS:
            UD_FREE(cmd_local.args.props.fcb, cmd_local.args.props.data);
            break;
        default:
            break;
        }
    }
    CLOSE_SOCK(olcmd->ol_r.fd);
    CLOSE_SOCK(olcmd->fd);
    fsqu_free(&olcmd->qu);
}
static void _iocp_stop_acpex_thread(ev_ctx *ctx) {
    uint32_t i;
    // 停止 AcceptEx 线程（暂不关共用 IOCP，步骤4 仍需从中取出取消完成）
    for (i = 0; i < ctx->nacpex; i++) {
        ATOMIC_SET(&ctx->acpex[i].stop, 1);
        // 投递空包唤醒线程；失败时线程会在 EVENT_WAIT_TIMEOUT 后自行检测 stop 退出
        if (!PostQueuedCompletionStatus(ctx->acpex[i].iocp, 0, ((ULONG_PTR)-1), NULL)) {
            LOG_ERROR("PostQueuedCompletionStatus failed: %s", ERRORSTR(ERRNO));
        }
    }
    for (i = 0; i < ctx->nacpex; i++) {
        thread_join(ctx->acpex[i].thacp);
    }
}
static void _iocp_free_watcher(ev_ctx *ctx) {
    uint32_t i;
    cmd_ctx cmd;
    cmd.cmd = CMD_STOP;
    watcher_ctx *watcher;
    for (i = 0; i < ctx->nthreads; i++) {
        watcher = &ctx->watcher[i];
        (void)_send_cmd(watcher, &cmd);
    }
    for (i = 0; i < ctx->nthreads; i++) {
        watcher = &ctx->watcher[i];
        thread_join(watcher->thevent);
        (void)CloseHandle(watcher->iocp);
        _iocp_free_cmd(watcher);
        hashmap_free(watcher->element);
        pool_free(&watcher->pool);
    }
    FREE(ctx->watcher);
}
static void _iocp_free_acpex(ev_ctx *ctx) {
    DWORD bytes;
    ULONG_PTR key;
    LPOVERLAPPED overlap;
    sock_ctx *sock;
    uint32_t idle = 0;
    while (ATOMIC_GET(&ctx->nlsn) > 0) {
        overlap = NULL;
        (void)GetQueuedCompletionStatus(ctx->acpex[0].iocp, &bytes, &key, &overlap, EVENT_WAIT_TIMEOUT);
        if (NULL != overlap) {
            sock = UPCAST(overlap, sock_ctx, overlapped);
            _iocp_acpex_release(sock);
            idle = 0;
        } else {
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
    array_free(&ctx->arrlsn);
    spin_free(&ctx->spin);
}

#endif//EV_IOCP
