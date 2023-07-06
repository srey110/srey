#include "event/iocp.h"
#include "hashmap.h"
#include "netutils.h"
#include "timer.h"

#ifdef EV_IOCP

exfuncs_ctx _exfuncs;
static atomic_t _init_once = 0;
static void(*cmd_cbs[CMD_TOTAL])(watcher_ctx *watcher, cmd_ctx *cmd);

static inline uint64_t _map_hash(const void *item, uint64_t seed0, uint64_t seed1) {
    return hash((const char *)&((*(const sock_ctx **)item)->fd), sizeof(SOCKET));
}
static inline int _map_compare(const void *a, const void *b, void *ud) {
    return (int)((*(const sock_ctx **)a)->fd - (*(const sock_ctx **)b)->fd);
}
static void *_exfunc(SOCKET fd, GUID  *guid) {
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
static void _init_callback(void) {
    cmd_cbs[CMD_STOP] = _on_cmd_stop;
    cmd_cbs[CMD_DISCONN] = _on_cmd_disconn;
    cmd_cbs[CMD_ADD] = _on_cmd_add;
    cmd_cbs[CMD_ADDACP] = _on_cmd_addacp;
    cmd_cbs[CMD_REMOVE] = _on_cmd_remove;
    cmd_cbs[CMD_SEND] = _on_cmd_send;
    cmd_cbs[CMD_SETUD] = _on_cmd_setud;
}
static void _init_funcs(ev_ctx *ctx) {
    if (ATOMIC_CAS(&_init_once, 0, 1)) {
        SOCKET fd = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, 0);
        ASSERTAB(INVALID_SOCK != fd, ERRORSTR(ERRNO));
        GUID accept_uid = WSAID_ACCEPTEX;
        GUID connect_uid = WSAID_CONNECTEX;
        _exfuncs.acceptex = _exfunc(fd, &accept_uid);
        _exfuncs.connectex = _exfunc(fd, &connect_uid);
        CLOSE_SOCK(fd);
        _init_callback();
    }
}
int32_t _join_iocp(watcher_ctx *watcher, SOCKET fd) {
    if (NULL == CreateIoCompletionPort((HANDLE)fd, watcher->iocp, 0, 1)) {
        return ERR_FAILED;
    }
    return ERR_OK;
}
static void _on_cmd(watcher_ctx *watcher, sock_ctx *skctx, DWORD bytes) {
    cmd_ctx cmd;
    int32_t i, nread;
    char cmdbuf[CMD_MAX_NREAD];
    overlap_cmd_ctx *olcmd = UPCAST(skctx, overlap_cmd_ctx, ol_r);
    do {
        nread = recv(olcmd->ol_r.fd, cmdbuf, sizeof(cmdbuf), 0);
        if (nread <= 0) {
            break;
        }
        for (i = 0; i < nread; i++) {
            spin_lock(&olcmd->spin);
            cmd = *qu_cmd_pop(&olcmd->qu);
            spin_unlock(&olcmd->spin);
            cmd_cbs[cmd.cmd](watcher, &cmd);
        }
    } while (nread == sizeof(cmdbuf));
    if (0 == watcher->stop) {
        ASSERTAB(ERR_OK == _post_recv(&olcmd->ol_r, &olcmd->bytes, &olcmd->flag, &olcmd->wsabuf, 1), ERRORSTR(ERRNO));
    }
}
static inline void _pool_shrink(watcher_ctx *watcher, timer_ctx *timer) {
    if (timer_elapsed_ms(timer) < SHRINK_TIME) {
        return;
    }
    timer_start(timer);
    pool_shrink(&watcher->pool, hashmap_count(watcher->element) / 2);
}
#if (_WIN32_WINNT >= 0x0600)
static void _loop_event(void *arg) {
    watcher_ctx *watcher = (watcher_ctx *)arg;
    int32_t err;
    ULONG i;
    ULONG count;
    ULONG nevent = INIT_EVENTS_CNT;
    sock_ctx *sock;
    timer_ctx timer;
    LPOVERLAPPED overlap;
    LPOVERLAPPED_ENTRY overlappeds;
    MALLOC(overlappeds, sizeof(OVERLAPPED_ENTRY) * nevent);
    timer_init(&timer);
    timer_start(&timer);
    while (0 == watcher->stop) {
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
            if (0 == watcher->stop
                && count == nevent) {
                FREE(overlappeds);
                nevent *= 2;
                MALLOC(overlappeds, sizeof(OVERLAPPED_ENTRY) * nevent);
            }
        } else if (WAIT_TIMEOUT != (err = ERRNO)) {
            LOG_ERROR("%s", ERRORSTR(err));
        }
        _pool_shrink(watcher, &timer);
    }
    FREE(overlappeds);
}
static void _loop_acpex(void *arg) {
    acceptex_ctx *acpex = (acceptex_ctx *)arg;
    int32_t err;
    ULONG i;
    ULONG count;
    ULONG nevent = INIT_EVENTS_CNT;
    sock_ctx *sock;
    LPOVERLAPPED overlap;
    LPOVERLAPPED_ENTRY overlappeds;
    MALLOC(overlappeds, sizeof(OVERLAPPED_ENTRY) * nevent);
    while (0 == acpex->stop) {
        if (GetQueuedCompletionStatusEx(acpex->iocp,
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
                sock->ev_cb(acpex, sock, overlappeds[i].dwNumberOfBytesTransferred);
            }
            if (0 == acpex->stop
                && count == nevent) {
                FREE(overlappeds);
                nevent *= 2;
                MALLOC(overlappeds, sizeof(OVERLAPPED_ENTRY) * nevent);
            }
        } else if (WAIT_TIMEOUT != (err = ERRNO)) {
            LOG_ERROR("%s", ERRORSTR(err));
        }
    }
    FREE(overlappeds);
}
#else
static void _loop_event(void *arg) {
    watcher_ctx *watcher = (watcher_ctx *)arg;
    DWORD bytes;
    int32_t err;
    ULONG_PTR key;
    sock_ctx *sock;
    timer_ctx timer;
    OVERLAPPED *overlap;
    timer_init(&timer);
    timer_start(&timer);
    while (0 == watcher->stop) {
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
        _pool_shrink(watcher, &timer);
    }
}
static void _loop_acpex(void *arg) {
    acceptex_ctx *acpex = (acceptex_ctx *)arg;
    DWORD bytes;
    int32_t err;
    ULONG_PTR key;
    sock_ctx *sock;
    OVERLAPPED *overlap;
    while (0 == acpex->stop) {
        GetQueuedCompletionStatus(acpex->iocp,
                                  &bytes,
                                  &key,
                                  &overlap,
                                  EVENT_WAIT_TIMEOUT);
        if (NULL != overlap) {
            sock = UPCAST(overlap, sock_ctx, overlapped);
            sock->ev_cb(acpex, sock, bytes);
        } else if (WAIT_TIMEOUT != (err = ERRNO)) {
            LOG_ERROR("%s", ERRORSTR(err));
        }
    }
}
#endif
static inline void _free_element(void *item) {
    sock_ctx *sock = *((sock_ctx **)item);
    if (SOCK_STREAM == sock->type) {
        _free_sk(sock);
    } else {
        _free_udp(sock);
    }
}
static void _init_cmd(watcher_ctx *watcher) {
    SOCKET pair[2];
    overlap_cmd_ctx *olcmd;
    MALLOC(watcher->cmd, sizeof(overlap_cmd_ctx) * watcher->ncmd);
    for (uint32_t i = 0; i < watcher->ncmd; i++) {
        olcmd = &watcher->cmd[i];
        ASSERTAB(ERR_OK == sock_pair(pair), ERRORSTR(ERRNO));
        olcmd->ol_r.ev_cb = _on_cmd;
        olcmd->ol_r.fd = pair[0];
        olcmd->ol_r.type = 0;
        olcmd->fd = pair[1];
        qu_cmd_init(&olcmd->qu, ONEK);
        spin_init(&olcmd->spin, SPIN_CNT_CMD);
        olcmd->wsabuf.IOV_PTR_FIELD = NULL;
        olcmd->wsabuf.IOV_LEN_FIELD = 0;
        ASSERTAB(ERR_OK == _join_iocp(watcher, olcmd->ol_r.fd), ERRORSTR(ERRNO));
        ASSERTAB(ERR_OK == _post_recv(&olcmd->ol_r, &olcmd->bytes, &olcmd->flag, &olcmd->wsabuf, 1), ERRORSTR(ERRNO));
    }
}
void ev_init(ev_ctx *ctx, uint32_t nthreads) {
    ctx->nthreads = (0 == nthreads ? 1 : nthreads);
    ctx->nacpex = ctx->nthreads;
    sock_init();
    _init_funcs(ctx);

    MALLOC(ctx->watcher, sizeof(watcher_ctx) * ctx->nthreads);
    watcher_ctx *watcher;
    uint32_t i;
    for (i = 0; i < ctx->nthreads; i++) {
        watcher = &ctx->watcher[i];
        watcher->index = i;
        watcher->stop = 0;
        watcher->ncmd = ctx->nthreads * 2;
        watcher->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
        ASSERTAB(NULL != watcher->iocp, ERRORSTR(ERRNO));
        watcher->ev = ctx;
        watcher->element = hashmap_new_with_allocator(_malloc, _realloc, _free,
                                                      sizeof(sock_ctx *), ONEK * 2, 0, 0, 
                                                      _map_hash, _map_compare, _free_element, NULL);
        pool_init(&watcher->pool, ONEK);
        _init_cmd(watcher);
        watcher->thevent = thread_creat(_loop_event, watcher);
    }

    spin_init(&ctx->spin, SPIN_CNT_LSN);
    arr_lsn_init(&ctx->arrlsn, ARRAY_INIT_SIZE);
    HANDLE iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, ctx->nacpex);
    ASSERTAB(NULL != iocp, ERRORSTR(ERRNO));
    MALLOC(ctx->acpex, sizeof(acceptex_ctx) * ctx->nacpex);
    acceptex_ctx *acpex;
    for (i = 0; i < ctx->nacpex; i++) {
        acpex = &ctx->acpex[i];
        acpex->index = i;
        acpex->stop = 0;
        acpex->ev = ctx;
        acpex->iocp = iocp;
        acpex->thacp = thread_creat(_loop_acpex, acpex);
    }
    LOG_INFO("event: IOCP");
}
static void _free_cmd(watcher_ctx *watcher) {
    cmd_ctx *cmd;
    void *data;
    overlap_cmd_ctx *olcmd;
    for (uint32_t i = 0; i < watcher->ncmd; i++) {
        olcmd = &watcher->cmd[i];
        while (NULL != (cmd = qu_cmd_pop(&olcmd->qu))) {
            if (CMD_SEND == cmd->cmd) {
                data = (void *)cmd->arg;
                FREE(data);
            }
        }
        CLOSE_SOCK(olcmd->ol_r.fd);
        CLOSE_SOCK(olcmd->fd);
        qu_cmd_free(&olcmd->qu);
        spin_free(&olcmd->spin);
    }
    FREE(watcher->cmd);
}
void ev_free(ev_ctx *ctx) {
    uint32_t i;
    for (i = 0; i < ctx->nacpex; i++) {
        ctx->acpex[i].stop = 1;
        (void)PostQueuedCompletionStatus(ctx->acpex[i].iocp, 0, ((ULONG_PTR)-1), NULL);
    }
    for (i = 0; i < ctx->nacpex; i++) {
        thread_join(ctx->acpex[i].thacp);
    }
    cmd_ctx cmd;
    cmd.cmd = CMD_STOP;
    watcher_ctx *watcher;
    for (i = 0; i < ctx->nthreads; i++) {
        watcher = &ctx->watcher[i];
        _send_cmd(watcher, watcher->ncmd - 1, &cmd);
    }
    for (i = 0; i < ctx->nthreads; i++) {
        thread_join(ctx->watcher[i].thevent);
    }
    for (i = 0; i < ctx->nthreads; i++) {
        watcher = &ctx->watcher[i];
        _free_cmd(watcher);
        hashmap_free(watcher->element);
        pool_free(&watcher->pool);
        (void)CloseHandle(watcher->iocp);
    }
    FREE(ctx->watcher);
    struct listener_ctx **lsn;
    size_t nlsn = arr_lsn_size(&ctx->arrlsn);
    for (i = 0; i < (uint32_t)nlsn; i++) {
        lsn = arr_lsn_at(&ctx->arrlsn, i);
        _freelsn(*lsn);
    }
    arr_lsn_free(&ctx->arrlsn);
    spin_free(&ctx->spin);
    (void)CloseHandle(ctx->acpex[0].iocp);
    FREE(ctx->acpex);
    sock_clean();
}

#endif//EV_IOCP
