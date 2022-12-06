#include "event/iocp.h"
#include "event/worker.h"
#include "netutils.h"
#include "loger.h"

#ifdef EV_IOCP

exfuncs_ctx _exfuncs;
static atomic_t _init_once = 0;

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
static void _init_exfuncs(ev_ctx *ctx)
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
    }
}
#if (_WIN32_WINNT >= 0x0600)
static void _loop_event(void *arg)
{
    watcher_ctx *watcher = (watcher_ctx *)arg;
    BOOL rtn;
    int32_t err;
    ULONG i;
    ULONG count;
    ULONG nevent = INIT_EVENTS_CNT;
    sock_ctx *sock;
    LPOVERLAPPED overlap;
    LPOVERLAPPED_ENTRY overlappeds;
    MALLOC(overlappeds, sizeof(OVERLAPPED_ENTRY) * nevent);
    while (0 == watcher->ev->stop)
    {
        rtn = GetQueuedCompletionStatusEx(watcher->iocp,
                                          overlappeds,
                                          nevent,
                                          &count,
                                          EVENT_WAIT_TIMEOUT,
                                          FALSE);
        if (rtn)
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
            if (0 == watcher->ev->stop
                && count == nevent
                && nevent < MAX_EVENTS_CNT)
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
    OVERLAPPED *overlap;
    while (0 == watcher->ev->stop)
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
    }
}
#endif
void ev_init(ev_ctx *ctx, uint32_t nthreads)
{
    sock_init();
    ctx->stop = 0;
    ctx->nthreads = (0 == nthreads ? 1 : nthreads);
    _init_exfuncs(ctx);
    HANDLE iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, ctx->nthreads);
    ASSERTAB(NULL != iocp, ERRORSTR(ERRNO));
    ctx->nthreads *= 2;
    mutex_init(&ctx->qulsnlck);
    qu_lsn_init(&ctx->qulsn, 8);
    ctx->worker = worker_init(ctx, ctx->nthreads, ctx->nthreads * 2);
    MALLOC(ctx->watcher, sizeof(watcher_ctx) * ctx->nthreads);
    watcher_ctx *watcher;
    LOG_INFO("event type: EV_IOCP");
    for (uint32_t i = 0; i < ctx->nthreads; i++)
    {
        watcher = &ctx->watcher[i];
        watcher->iocp = iocp;
        watcher->ev = ctx;
        watcher->thevent = thread_creat(_loop_event, watcher);
    }
}
void ev_free(ev_ctx *ctx)
{
    uint32_t i;
    ctx->stop = 1;
    for (i = 0; i < ctx->nthreads; i++)
    {
        if (!PostQueuedCompletionStatus(ctx->watcher[i].iocp, 0, ((ULONG_PTR)-1), NULL))
        {
            LOG_ERROR("%s", ERRORSTR(ERRNO));
        }
    }
    for (i = 0; i < ctx->nthreads; i++)
    {
        thread_join(ctx->watcher[i].thevent);
    }
    worker_free(ctx->worker);
    HANDLE iocp = ctx->watcher[0].iocp;
    FREE(ctx->watcher);
    struct listener_ctx **lsn;
    while (NULL != (lsn = qu_lsn_pop(&ctx->qulsn)))
    {
        _freelsn(*lsn);
    }
    qu_lsn_free(&ctx->qulsn);
    mutex_free(&ctx->qulsnlck);
    (void)CloseHandle(iocp);
    sock_clean();
}

#endif//EV_IOCP
