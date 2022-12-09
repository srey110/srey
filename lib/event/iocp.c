#include "event/iocp.h"
#include "hashmap.h"
#include "netutils.h"
#include "loger.h"
#include "timer.h"

#ifdef EV_IOCP

exfuncs_ctx _exfuncs;
static atomic_t _init_once = 0;
static void(*cmd_cbs[CMD_TOTAL])(watcher_ctx *watcher, cmd_ctx *cmd);

static inline uint64_t _map_hash(const void *item, uint64_t seed0, uint64_t seed1)
{
    return FD_HASH(((const map_element *)item)->fd);
}
static inline int _map_compare(const void *a, const void *b, void *ud)
{
    return (int)(((const map_element *)a)->fd - ((const map_element *)b)->fd);
}
static void *_exfunc(SOCKET fd, GUID  *guid)
{
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
static void _init_callback()
{
    cmd_cbs[CMD_STOP] = _on_cmd_stop;
    cmd_cbs[CMD_DISCONN] = _on_cmd_disconn;
    cmd_cbs[CMD_ADD] = _on_cmd_add;
    cmd_cbs[CMD_ADDACP] = _on_cmd_addacp;
    cmd_cbs[CMD_REMOVE] = _on_cmd_remove;
    cmd_cbs[CMD_SEND] = _on_cmd_send;
}
static void _init_funcs(ev_ctx *ctx)
{
    if (ATOMIC_CAS(&_init_once, 0, 1))
    {
        SOCKET sock = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, 0);
        ASSERTAB(INVALID_SOCK != sock, ERRORSTR(ERRNO));
        GUID accept_uid = WSAID_ACCEPTEX;
        GUID connect_uid = WSAID_CONNECTEX;
        _exfuncs.acceptex = _exfunc(sock, &accept_uid);
        _exfuncs.connectex = _exfunc(sock, &connect_uid);
        CLOSE_SOCK(sock);

        _init_callback();
    }
}
int32_t _join_iocp(watcher_ctx *watcher, SOCKET fd)
{
    if (NULL == CreateIoCompletionPort((HANDLE)fd, watcher->iocp, 0, 1))
    {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    return ERR_OK;
}
static void _on_cmd(watcher_ctx *watcher, sock_ctx *skctx, DWORD bytes)
{
    cmd_ctx cmd;
    int32_t i, nread;
    char cmdbuf[CMD_MAX_NREAD];
    overlap_cmd_ctx *ol = UPCAST(skctx, overlap_cmd_ctx, ol_r);
    do 
    {
        nread = recv(ol->ol_r.fd, cmdbuf, sizeof(cmdbuf), 0);
        if (nread <= 0)
        {
            break;
        }
        for (i = 0; i < nread; i++)
        {
            mutex_lock(&ol->lck);
            cmd = *qu_cmd_pop(&ol->qu);
            mutex_unlock(&ol->lck);
            cmd_cbs[cmd.cmd](watcher, &cmd);
        }
    } while (nread == sizeof(cmdbuf));
    if (0 == watcher->stop)
    {
        ASSERTAB(ERR_OK == _post_recv(&ol->ol_r, &ol->bytes, &ol->flag, &ol->wsabuf, 1), "_post_recv failed.");
    }
}
static inline void _check_delayfree(watcher_ctx *watcher, arr_delay *arr)
{
    delay_ctx *delay;
    int32_t size = (int32_t)arr_delay_size(arr);
    for (int32_t i = size - 1; i >= 0; i--)
    {
        delay = arr_delay_at(arr, i);
        if (0 == _check_canfree(delay->sock))
        {
            if (SOCK_STREAM == delay->sock->type)
            {
                pool_push(&watcher->pool, delay->sock);
            }
            else
            {
                _free_udp(delay->sock);
            }
            arr_delay_del_nomove(arr, i);
        }
        else
        {
            if (watcher->ntime >= delay->timeout)
            {
                if (SOCK_STREAM == delay->sock->type)
                {
                    pool_push(&watcher->pool, delay->sock);
                }
                else
                {
                    _free_udp(delay->sock);
                }
                arr_delay_del_nomove(arr, i);
                LOG_WARN("wait socket free timeout,type: %d", delay->sock->type);
            }
        }
    }
}
static inline void _pool_shrink(watcher_ctx *watcher)
{
    if (watcher->ntime - watcher->lastshrink < SHRINK_TIME / 100)
    {
        return;
    }
    watcher->lastshrink = watcher->ntime;
    pool_shrink(&watcher->pool, hashmap_count(watcher->element) / 2);
}
#if (_WIN32_WINNT >= 0x0600)
static void _loop_event(void *arg)
{
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
    while (0 == watcher->stop)
    {
        if (GetQueuedCompletionStatusEx(watcher->iocp,
                                        overlappeds,
                                        nevent,
                                        &count,
                                        EVENT_WAIT_TIMEOUT,
                                        FALSE))
        {
            for (i = 0; i < count; i++)
            {
                overlap = overlappeds[i].lpOverlapped;
                if (NULL == overlap)
                {
                    continue;
                }
                sock = UPCAST(overlap, sock_ctx, overlapped);
                sock->ev_cb(watcher, sock, overlappeds[i].dwNumberOfBytesTransferred);
            }
            if (0 == watcher->stop
                && count == nevent)
            {
                FREE(overlappeds);
                nevent *= 2;
                MALLOC(overlappeds, sizeof(OVERLAPPED_ENTRY) * nevent);
            }
        }
        else if (WAIT_TIMEOUT != (err = ERRNO))
        {
            LOG_ERROR("%s", ERRORSTR(err));
        }
        if (timer_elapsed_ms(&timer) >= 100)
        {
            watcher->ntime++;
            _pool_shrink(watcher);
            _check_delayfree(watcher, &watcher->delay);
            timer_start(&timer);
        }
    }
    FREE(overlappeds);
}
#else
static void _loop_event(void *arg)
{
    watcher_ctx *watcher = (watcher_ctx *)arg;
    DWORD bytes;
    int32_t err;
    ULONG_PTR key;
    sock_ctx *sock;
    timer_ctx timer;
    OVERLAPPED *overlap;
    timer_init(&timer);
    timer_start(&timer);
    while (0 == watcher->stop)
    {
        GetQueuedCompletionStatus(watcher->iocp,
                                  &bytes,
                                  &key,
                                  &overlap,
                                  EVENT_WAIT_TIMEOUT);
        if (NULL != overlap)
        {
            sock = UPCAST(overlap, sock_ctx, overlapped);
            sock->ev_cb(watcher, sock, bytes);
        }
        else if (WAIT_TIMEOUT != (err = ERRNO))
        {
            LOG_ERROR("%s", ERRORSTR(err));
        }
        if (timer_elapsed_ms(&timer) >= 100)
        {
            watcher->ntime++;
            _pool_shrink(watcher);
            _check_delayfree(watcher, &watcher->delay);
            timer_start(&timer);
        }
    }
}
#endif
static inline void _free_mapitem(void *item)
{
    map_element *el = (map_element *)item;
    if (SOCK_STREAM == el->sock->type)
    {
        _free_sk(el->sock);
    }
    else
    {
        _free_udp(el->sock);
    }
}
static void _init_cmd(watcher_ctx *watcher)
{
    SOCKET pair[2];
    overlap_cmd_ctx *ol;
    MALLOC(watcher->cmd, sizeof(overlap_cmd_ctx) * watcher->ncmd);
    for (uint32_t i = 0; i < watcher->ncmd; i++)
    {
        ol = &watcher->cmd[i];
        ASSERTAB(ERR_OK == sock_pair(pair), "create sock pair failed.");
        ol->ol_r.ev_cb = _on_cmd;
        ol->ol_r.fd = pair[0];
        ol->ol_r.type = 0;
        ol->fd = pair[1];
        qu_cmd_init(&ol->qu, ONEK);
        mutex_init(&ol->lck);
        ol->wsabuf.IOV_PTR_FIELD = NULL;
        ol->wsabuf.IOV_LEN_FIELD = 0;
        ASSERTAB(ERR_OK == _join_iocp(watcher, ol->ol_r.fd), "_join_iocp failed.");
        ASSERTAB(ERR_OK == _post_recv(&ol->ol_r, &ol->bytes, &ol->flag, &ol->wsabuf, 1), "_post_recv failed.");
    }
}
void ev_init(ev_ctx *ctx, uint32_t nthreads)
{
    ctx->nthreads = (0 == nthreads ? 1 : nthreads);
    sock_init();
    _init_funcs(ctx);
    mutex_init(&ctx->qulsnlck);
    qu_lsn_init(&ctx->qulsn, 8);
    MALLOC(ctx->watcher, sizeof(watcher_ctx) * ctx->nthreads);
    watcher_ctx *watcher;
    for (uint32_t i = 0; i < ctx->nthreads; i++)
    {
        watcher = &ctx->watcher[i];
        watcher->index = i;
        watcher->stop = 0;
        watcher->ntime = 0;
        watcher->lastshrink = 0;
        watcher->ncmd = ctx->nthreads * 2;
        watcher->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
        ASSERTAB(NULL != watcher->iocp, ERRORSTR(ERRNO));
        watcher->ev = ctx;
        watcher->element = hashmap_new_with_allocator(_malloc, _realloc, _free,
            sizeof(map_element), ONEK * 2, 0, 0, _map_hash, _map_compare, _free_mapitem, NULL);
        pool_init(&watcher->pool, ONEK);
        arr_delay_init(&watcher->delay, 64);
        _init_cmd(watcher);
        watcher->thevent = thread_creat(_loop_event, watcher);
    }
}
static void _free_cmd(watcher_ctx *watcher)
{
    overlap_cmd_ctx *ol;
    for (uint32_t i = 0; i < watcher->ncmd; i++)
    {
        ol = &watcher->cmd[i];
        CLOSE_SOCK(ol->ol_r.fd);
        CLOSE_SOCK(ol->fd);
        qu_cmd_free(&ol->qu);
        mutex_free(&ol->lck);
    }
    FREE(watcher->cmd);
}
static void _free_delay(arr_delay *arr)
{
    delay_ctx *delay;
    size_t size = arr_delay_size(arr);
    for (size_t i = 0; i < size; i++)
    {
        delay = arr_delay_at(arr, i);
        if (SOCK_STREAM == delay->sock->type)
        {
            _free_sk(delay->sock);
        }
        else
        {
            _free_udp(delay->sock);
        }
    }
    arr_delay_free(arr);
}
void ev_free(ev_ctx *ctx)
{
    uint32_t i;
    cmd_ctx cmd;
    cmd.cmd = CMD_STOP;
    watcher_ctx *watcher;
    for (i = 0; i < ctx->nthreads; i++)
    {
        watcher = &ctx->watcher[i];
        _send_cmd(watcher, watcher->ncmd - 1, &cmd);
    }
    for (i = 0; i < ctx->nthreads; i++)
    {
        thread_join(ctx->watcher[i].thevent);
    }
    for (i = 0; i < ctx->nthreads; i++)
    {
        watcher = &ctx->watcher[i];
        _free_cmd(watcher);
        _free_delay(&watcher->delay);
        hashmap_free(watcher->element);
        pool_free(&watcher->pool);
        (void)CloseHandle(watcher->iocp);
    }
    FREE(ctx->watcher);
    struct listener_ctx **lsn;
    while (NULL != (lsn = qu_lsn_pop(&ctx->qulsn)))
    {
        _freelsn(*lsn);
    }
    qu_lsn_free(&ctx->qulsn);
    mutex_free(&ctx->qulsnlck);
    sock_clean();
}

#endif//EV_IOCP
