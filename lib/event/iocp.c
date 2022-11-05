#include "event/event.h"

#ifdef EV_IOCP

#define NOTIFI_EXIT_KEY  ((ULONG_PTR)-1)

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
static void _wait_event(watcher_ctx *watcher, int32_t *stop)
{
    ULONG_PTR key;
    OVERLAPPED *overlap;
    BOOL rtn = GetQueuedCompletionStatus(watcher->evhandle,
        &watcher->bytes,
        &key,
        &overlap,
        INFINITE);
    if (NOTIFI_EXIT_KEY == key)
    {
        *stop = 1;
        PRINTD("%s", "stop event loop.");
        return;
    }
    if (NULL == overlap)
    {
        return;
    }
    watcher->err = ERR_OK;
    if (!rtn)
    {
        watcher->err = ERRNO;
    }
    sock_ctx *sock = UPCAST(overlap, sock_ctx, overlapped);
    sock->ev_cb(watcher, sock, 0, stop);
}
static void _loop(void *arg)
{
    int32_t stop = 0;
    watcher_ctx *watcher = (watcher_ctx *)arg;
    while (0 == stop)
    {
        _wait_event(watcher, &stop);
    }
}
void ev_init(ev_ctx *ctx, uint32_t nthreads)
{
    sock_init();
    ctx->nthreads = (0 == nthreads ? 1 : nthreads);
    MALLOC(ctx->watcher, sizeof(watcher_ctx) * ctx->nthreads * 2);

    HANDLE iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, ctx->nthreads);
    ASSERTAB(NULL != iocp, ERRORSTR(ERRNO));

    SOCKET sock = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, 0);
    ASSERTAB(INVALID_SOCK != sock, ERRORSTR(ERRNO));
    GUID accept_uid = WSAID_ACCEPTEX;
    GUID connect_uid = WSAID_CONNECTEX;
    GUID disconnect_uid = WSAID_DISCONNECTEX;
    void *acpex = _exfunc(sock, &accept_uid);
    void *connex = _exfunc(sock, &connect_uid);
    void *disconnex = _exfunc(sock, &disconnect_uid);
    CLOSE_SOCK(sock);

    watcher_ctx *watcher;
    for (uint32_t i = 0; i < ctx->nthreads * 2; i++)
    {
        watcher = &ctx->watcher[i];
        watcher->ev = ctx;
        watcher->evhandle = iocp;
        watcher->acceptex = acpex;
        watcher->connectex = connex;
        watcher->disconnectex = disconnex;
        thread_init(&watcher->thread);
        thread_creat(&watcher->thread, _loop, watcher);
    }
}
void ev_free(ev_ctx *ctx)
{
    uint32_t i;
    for (i = 0; i < ctx->nthreads * 2; i++)
    {
        if (!PostQueuedCompletionStatus(ctx->watcher[i].evhandle, 0, NOTIFI_EXIT_KEY, NULL))
        {
            LOG_ERROR("%s", ERRORSTR(ERRNO));
        }
    }
    for (i = 0; i < ctx->nthreads; i++)
    {
        thread_join(&ctx->watcher[i].thread);
    }
    (void)CloseHandle(ctx->watcher[0].evhandle);
    FREE(ctx->watcher);
    sock_clean();
}

#endif //EV_IOCP
