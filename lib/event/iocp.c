#include "event/iocp.h"
#include "containers/hashmap.h"
#include "thread/spinlock.h"
#include "utils/netutils.h"
#include "utils/timer.h"

#ifdef EV_IOCP

exfuncs_ctx _exfuncs;                                           // 全局扩展函数指针（AcceptEx/ConnectEx）
static atomic_t _init_once = 0;                                 // 保证扩展函数只初始化一次
static void(*cmd_cbs[CMD_TOTAL])(watcher_ctx *watcher, cmd_ctx *cmd); // 命令回调函数表

// 通过WSAIoctl获取指定GUID的Windows扩展函数指针
static void *_iocp_exfunc(SOCKET fd, GUID  *guid) {
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
// 初始化命令回调函数表，_iocp_on_cmd 批量处理cmd，为了快速消费掉cmd，里面不应有耗时操作。
// 如 在_on_cmd_send里面直接发送数据
static void _iocp_init_callback(void) {
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
    int32_t i, cnt, nread;
    char ntrigger[CMD_MAX_NREAD];
    cmd_ctx cmds[CMD_MAX_NREAD];
    overlap_cmd_ctx *olcmd = UPCAST(skctx, overlap_cmd_ctx, ol_r);
    for (;;) {
        nread = recv(olcmd->ol_r.fd, ntrigger, sizeof(ntrigger), 0);
        if (nread <= 0) {
            break;
        }
        cnt = (int32_t)fsqu_pop_sc_batch(&olcmd->qu, cmds, (uint32_t)nread);
        cnt_total += (size_t)cnt;
        for (i = 0; i < cnt; i++) {
            cmd_cbs[cmds[i].cmd](watcher, &cmds[i]);
        }
    }
    if (tda_check(&olcmd->tda, cnt_total)) {
        LOG_WARN("watcher %d cmd queue overload, count %zu.", watcher->index, cnt_total);
    }
    if (0 == ATOMIC_GET(&watcher->stop)) {
        ASSERTAB(ERR_OK == _iocp_post_recv(&olcmd->ol_r, &olcmd->bytes, &olcmd->flag, &olcmd->wsabuf, 1), ERRORSTR(ERRNO));
    }
}
// 定期收缩对象池（每EVENT_CHECK_INTERVAL轮检查一次时钟，避免频繁syscall）
static void _iocp_pool_shrink(watcher_ctx *watcher, timer_ctx *timer, uint32_t *cnt) {
    if (++(*cnt) < EVENT_CHECK_INTERVAL) {
        return;
    }
    *cnt = 0;
    if (timer_elapsed_ms(timer) < SHRINK_TIME) {
        return;
    }
    timer_start(timer);
    // cmd sock 仅 _iocp_join 不入 hashmap，hashmap_count 即业务 socket 数
    pool_shrink(&watcher->pool, (uint32_t)SHRINK_NKEEP(hashmap_count(watcher->element)), SHRINK_BUSY);
}
#if (_WIN32_WINNT >= 0x0600)
// 事件循环主函数（Vista+，使用GetQueuedCompletionStatusEx批量获取事件）
static void _iocp_loop_event(void *arg) {
    watcher_ctx *watcher = (watcher_ctx *)arg;
    int32_t err;
    ULONG i;
    ULONG count;
    ULONG nevent = INIT_EVENTS_CNT;
    sock_ctx *sock;
    uint32_t shrink_cnt = 0;
    timer_ctx timer;
    LPOVERLAPPED overlap;
    LPOVERLAPPED_ENTRY tmp;
    LPOVERLAPPED_ENTRY overlappeds;
    MALLOC(overlappeds, sizeof(OVERLAPPED_ENTRY) * nevent);
    timer_init(&timer);
    timer_start(&timer);
    while (0 == ATOMIC_GET(&watcher->stop)) {
        if (GetQueuedCompletionStatusEx(watcher->iocp,
                                        overlappeds,
                                        nevent,
                                        &count,
                                        EVENT_WAIT_TIMEOUT,
                                        FALSE)) {
            for (i = 0; i < count; i++) {
                overlap = overlappeds[i].lpOverlapped;
                if (NULL == overlap) {
                    continue;
                }
                sock = UPCAST(overlap, sock_ctx, overlapped);
                sock->ev_cb(watcher, sock, overlappeds[i].dwNumberOfBytesTransferred);
            }
            if (0 == ATOMIC_GET(&watcher->stop)
                && count == nevent) {
                MALLOC(tmp, sizeof(OVERLAPPED_ENTRY) * nevent * 2);
                FREE(overlappeds);
                overlappeds = tmp;
                nevent *= 2;
            }
        } else if (WAIT_TIMEOUT != (err = ERRNO)) {
            LOG_ERROR("%s", ERRORSTR(err));
        }
        _iocp_pool_shrink(watcher, &timer, &shrink_cnt);
    }
    LOG_INFO("net event thread %d exited.", watcher->index);
    FREE(overlappeds);
}
// AcceptEx专用线程事件循环（Vista+，批量处理accept完成事件）
static void _iocp_loop_acpex(void *arg) {
    acceptex_ctx *acpex = (acceptex_ctx *)arg;
    int32_t err;
    ULONG i;
    ULONG count;
    ULONG nevent = INIT_EVENTS_CNT;
    sock_ctx *sock;
    BOOL ok = FALSE;
    LPOVERLAPPED overlap;
    LPOVERLAPPED_ENTRY tmp;
    LPOVERLAPPED_ENTRY overlappeds;
    MALLOC(overlappeds, sizeof(OVERLAPPED_ENTRY) * nevent);
    while (0 == ATOMIC_GET(&acpex->stop)
           || (1 == ATOMIC_GET(&acpex->stop) && ok)) {//退出前抽干事件，防止lsn泄漏
        ok = GetQueuedCompletionStatusEx(acpex->iocp,
                                         overlappeds,
                                         nevent,
                                         &count,
                                         1 == ATOMIC_GET(&acpex->stop) ? 0 : EVENT_WAIT_TIMEOUT,
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
#else
// 事件循环主函数（XP兼容，使用GetQueuedCompletionStatus单个获取事件）
static void _iocp_loop_event(void *arg) {
    watcher_ctx *watcher = (watcher_ctx *)arg;
    DWORD bytes;
    int32_t err;
    ULONG_PTR key;
    sock_ctx *sock;
    uint32_t shrink_cnt = 0;
    timer_ctx timer;
    OVERLAPPED *overlap;
    timer_init(&timer);
    timer_start(&timer);
    while (0 == ATOMIC_GET(&watcher->stop)) {
        GetQueuedCompletionStatus(watcher->iocp,
                                  &bytes,
                                  &key,
                                  &overlap,
                                  EVENT_WAIT_TIMEOUT);
        if (NULL != overlap) {
            sock = UPCAST(overlap, sock_ctx, overlapped);
            sock->ev_cb(watcher, sock, bytes);
        } else if (WAIT_TIMEOUT != (err = ERRNO)) {
            LOG_ERROR("%s", ERRORSTR(err));
        }
        _iocp_pool_shrink(watcher, &timer, &shrink_cnt);
    }
    LOG_INFO("net event thread %d exited.", watcher->index);
}
// AcceptEx专用线程事件循环（XP兼容）
static void _iocp_loop_acpex(void *arg) {
    acceptex_ctx *acpex = (acceptex_ctx *)arg;
    DWORD bytes;
    int32_t err;
    BOOL ok = FALSE;
    ULONG_PTR key;
    sock_ctx *sock;
    OVERLAPPED *overlap;
    while (0 == ATOMIC_GET(&acpex->stop)
           || (1 == ATOMIC_GET(&acpex->stop) && ok)) {//退出前抽干事件，防止lsn泄漏
        overlap = NULL;
        ok = GetQueuedCompletionStatus(acpex->iocp,
                                       &bytes,
                                       &key,
                                       &overlap,
                                       1 == ATOMIC_GET(&acpex->stop) ? 0 : EVENT_WAIT_TIMEOUT);
        if (NULL != overlap) {
            sock = UPCAST(overlap, sock_ctx, overlapped);
            sock->ev_cb(acpex, sock, bytes);
            if (1 == ATOMIC_GET(&acpex->stop)) {//cancelled 完成事件（accept 被 CancelIoEx 取消的完成）会返回 ok=FALSE, overlap!=NULL
                ok = TRUE;
            }
        } else if (!ok) {
            if (WAIT_TIMEOUT == (err = ERRNO)) {
                if (0 == ATOMIC_GET(&acpex->stop)) {
                    ok = TRUE;
                }
            } else {
                LOG_ERROR("%s", ERRORSTR(err));
            }
        }
    }
    LOG_INFO("accept thread %d exited.", acpex->index);
}
#endif
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
    _iocp_init_funcs();
    MALLOC(ctx->watcher, sizeof(watcher_ctx) * ctx->nthreads);
    watcher_ctx *watcher;
    uint32_t i;
    el_cbs skcbs = { _evpub_sk_new, _evpub_sk_free, _evpub_sk_reset, _evpub_sk_clear };
    for (i = 0; i < ctx->nthreads; i++) {
        watcher = &ctx->watcher[i];
        watcher->index = i;
        ATOMIC_SET(&watcher->stop, 0);
        watcher->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
        ASSERTAB(NULL != watcher->iocp, ERRORSTR(ERRNO));
        watcher->ev = ctx;
        watcher->element = hashmap_new_with_allocator(_malloc, _realloc, _free,
                                                      sizeof(sock_ctx *), ONEK, 0, 0,
                                                      _evpub_sockel_hash, _evpub_sockel_compare, _iocp_sockel_free, NULL);
        pool_init(&watcher->pool, 0, 4 * ONEK, INIT_EVENTS_CNT, 0, &skcbs);
        _iocp_init_cmd(watcher);
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
    cmd_ctx *cmd;
    cmd_ctx cmd_local;
    void *data;
    sock_ctx *skctx;
    overlap_cmd_ctx *olcmd = &watcher->cmd;
    cmd = &cmd_local;
    while (ERR_OK == fsqu_pop_sc(&olcmd->qu, &cmd_local)) {
        switch (cmd->cmd) {
        // CMD_SEND 持有裸 payload；CMD_SENDTO 持有 [netaddr_ctx + payload]
        // 一整段 MALLOC，关闭路径都只需 FREE(arg) 释放整段
        case CMD_SEND:
        case CMD_SENDTO:
            data = (void *)cmd->arg;
            FREE(data);
            break;
        // CMD_SEND_MULTI 持有 shared_data*；多 fd 共享同一 pack,归还本 fd 引用即可
        case CMD_SEND_MULTI: {
            shared_data *pack = (shared_data *)cmd->arg;
            if (1 == ATOMIC_ADD(&pack->ref, -1)) {
                FREE(pack->data);
                FREE(pack);
            }
            break;
        }
        // CMD_UDP_OPT 持有 udp_opt_arg*；watcher 退出时未执行的命令直接释放参数包
        case CMD_UDP_OPT: {
            udp_opt_arg *udp_arg = (udp_opt_arg *)cmd->arg;
            FREE(udp_arg);
            break;
        }
        case CMD_CONN:
            skctx = (sock_ctx *)cmd->arg;
            _evpub_sk_free(skctx);
            FREE((void *)cmd->skid);  // CMD_CONN 借 skid 携带 MALLOC 的 conn addr,排空未处理命令时一并释放
            break;
        case CMD_ADD:
            skctx = (sock_ctx *)cmd->arg;
            if (SOCK_STREAM == skctx->type) {
                _evpub_sk_free(skctx);
            } else {
                _iocp_free_udp(skctx);
            }
            break;
        case CMD_ADDACP:
            // fd 是 accept 到的连接，未能加入事件循环；同时配对 _on_accept_cb
            // path 3 投递前 ref++ 占位的减法，ref 归零时释放 lsn
            CLOSE_SOCK(cmd->fd);
            _iocp_try_freelsn((struct listener_ctx *)cmd->arg);
            break;
        default:
            break;
        }
    }
    CLOSE_SOCK(olcmd->ol_r.fd);
    CLOSE_SOCK(olcmd->fd);
    fsqu_free(&olcmd->qu);
}
void ev_free(ev_ctx *ctx) {
    // 顺序：先停 acpex 防止新 CMD_ADDACP 投递；再停 watcher 并 _iocp_free_cmd 排空 cmd 队列
    // （内部 CMD_ADDACP 经 _iocp_try_freelsn 释放跨 watcher 投递占位 ref，操作仍存活的 lsn）；
    // 最后才释放仍在 arrlsn 中的 listener（此时 watcher/acpex 均已停，无路径再访问 lsn）。
    // 反向顺序（先释 listener 再 _iocp_free_cmd）会令 _iocp_free_cmd 中 CMD_ADDACP 的 _iocp_try_freelsn
    // 在已释放的 lsn->ref 上 atomic add → UAF；与 Unix 路径 ev_free 顺序对称。
    uint32_t i;
    // 1. 停止 AcceptEx 线程：之后不会再有 _on_accept_cb 投递新的 CMD_ADDACP
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
    (void)CloseHandle(ctx->acpex[0].iocp); // 所有acceptex_ctx共用同一个iocp，只需关闭一次
    FREE(ctx->acpex);
    // 2. 停止并释放所有 watcher：_iocp_free_cmd 内 CMD_ADDACP 的 _iocp_try_freelsn 此时 lsn 仍活，正确
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
        (void)CloseHandle(watcher->iocp);
        _iocp_free_cmd(watcher);
        hashmap_free(watcher->element);
        pool_free(&watcher->pool);
    }
    FREE(ctx->watcher);
    // 3. 释放仍在 arrlsn 中的 listener（未被 ev_unlisten 过的）：watcher/acpex 已停 + cmd 队列已空，
    //    ref 已无任何并发访问路径，_iocp_freelsn 不查 ref 强释安全（与 Unix 路径行为一致）
    struct listener_ctx **lsn;
    uint32_t nlsn = array_size(&ctx->arrlsn);
    for (i = 0; i < nlsn; i++) {
        lsn = (struct listener_ctx **)array_at(&ctx->arrlsn, i);
        _iocp_freelsn(*lsn);
    }
    array_free(&ctx->arrlsn);
    spin_free(&ctx->spin);
}

#endif//EV_IOCP
