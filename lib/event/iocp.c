#include "event/iocp.h"
#include "hashmap.h"

#ifdef EV_IOCP

#define NOTIFI_EXIT_KEY  ((ULONG_PTR)-1)

typedef struct sock_sendcb
{
    SOCKET sock;
    send_cb cb;
}sock_sendcb;

static uint64_t _mapcb_hash(const void *item, uint64_t seed0, uint64_t seed1)
{
    SOCKET sock = ((const sock_sendcb *)item)->sock;
    return hash((const char *)&sock, sizeof(sock));
}
static int _mapcb_compare(const void *a, const void *b, void *udata)
{
    return (int)(((const sock_sendcb *)a)->sock - ((const sock_sendcb *)b)->sock);
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
static void _wait_event(ev_ctx *ctx, int32_t *stop)
{
    DWORD bytes;
    ULONG_PTR key;
    OVERLAPPED *overlap;
    BOOL rtn = GetQueuedCompletionStatus(ctx->iocp,
        &bytes,
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
    int32_t err = ERR_OK;
    if (!rtn)
    {
        err = ERRNO;
    }
    sock_ctx *sock = UPCAST(overlap, sock_ctx, overlapped);
    sock->ev_cb(ctx, err, bytes, sock);
}
static void _loop_event(void *arg)
{
    int32_t stop = 0;
    ev_ctx *ctx = (ev_ctx *)arg;
    while (0 == stop)
    {
        _wait_event(ctx, &stop);
    }
}
static void _iocp_cmd(watcher_ctx *watcher, cmd_ctx *cmd)
{
    mutex_lock(&watcher->sender.mutex);
    qu_cmd_push(&watcher->sender.qucmd, cmd);
    if (watcher->sender.wait > 0)
    {
        cond_signal(&watcher->sender.cond);
    }
    mutex_unlock(&watcher->sender.mutex);
}
static watcher_ctx *_iocp_watcher(ev_ctx *ctx, SOCKET fd)
{
    return (1 == ctx->nthreads) ? &(ctx->watcher[0]) :
        (&ctx->watcher[hash((const char *)&fd, sizeof(fd)) % ctx->nthreads]);
}
void _sender_add(ev_ctx *ctx, SOCKET sock, send_cb cb)
{
    if (INVALID_SOCK == sock)
    {
        return;
    }
    cmd_ctx cmd;
    cmd.cmd = CMD_ADD;
    cmd.sock = sock;
    cmd.data = cb;
    _iocp_cmd(_iocp_watcher(ctx, sock), &cmd);
}
static void _on_cmd_add(struct hashmap *mapcb, cmd_ctx *cmd)
{
    sock_sendcb cb;
    cb.sock = cmd->sock;
    cb.cb = cmd->data;
    hashmap_set(mapcb, &cb);
}
void _sender_remove(ev_ctx *ctx, SOCKET sock)
{
    if (INVALID_SOCK == sock)
    {
        return;
    }
    cmd_ctx cmd;
    cmd.cmd = CMD_REMOVE;
    cmd.sock = sock;
    _iocp_cmd(_iocp_watcher(ctx, sock), &cmd);
}
static void _on_cmd_remove(struct hashmap *mapcb, cmd_ctx *cmd)
{
    sock_sendcb key;
    key.sock = cmd->sock;
    hashmap_delete(mapcb, &key);
}
void ev_send(ev_ctx *ctx, SOCKET sock, void *data, size_t len, char copy, void *ud)
{
    if (INVALID_SOCK == sock)
    {
        return;
    }
    cmd_ctx cmd;
    cmd.cmd = CMD_SEND;
    cmd.sock = sock;
    cmd.len = len;
    if (copy)
    {
        MALLOC(cmd.data, len);
        memcpy(cmd.data, data, len);
    }
    else
    {
        cmd.data = data;
    }
    cmd.ud = ud;
    _iocp_cmd(_iocp_watcher(ctx, sock), &cmd);
}
static void _on_cmd_send(watcher_ctx *watcher, struct hashmap *mapcb, cmd_ctx *cmd)
{
    sock_sendcb key;
    key.sock = cmd->sock;
    sock_sendcb *cb = hashmap_get(mapcb, &key);
    if (NULL == cb)
    {
        FREE(cmd->data);
        return;
    }
    if (ERR_OK != _iocp_send(watcher->sender.ev, cb->sock, cb->cb, cmd->data, cmd->len, cmd->ud))
    {
        if (NULL != cb->cb)
        {
            cb->cb(cb->sock, cmd->data, cmd->len, cmd->ud, ERR_FAILED);
        }
        FREE(cmd->data);
    }
}
static void _onsender_cmd(watcher_ctx *watcher, struct hashmap *mapcb, cmd_ctx *cmd)
{
    switch (cmd->cmd)
    {
    case CMD_ADD:
        PRINTD("%s", "CMD_ADD");
        _on_cmd_add(mapcb, cmd);
        break;
    case CMD_REMOVE:
        PRINTD("%s", "CMD_REMOVE");
        _on_cmd_remove(mapcb, cmd);
        break;
    case CMD_SEND:
        PRINTD("%s", "CMD_SEND");
        _on_cmd_send(watcher, mapcb, cmd);
        break;
    default:
        break;
    }
}
static void _wait_cmd(sender_ctx *sender, cmd_ctx *cmd)
{
    mutex_lock(&sender->mutex);
    while(0 == qu_cmd_size(&sender->qucmd))
    {
        sender->wait++;
        cond_wait(&sender->cond, &sender->mutex);
        sender->wait--;
    }
    *cmd = *qu_cmd_pop(&sender->qucmd);
    mutex_unlock(&sender->mutex);
}
static void _loop_sender(void *arg)
{
    char stop = 0;
    cmd_ctx cmd;
    watcher_ctx *watcher = (watcher_ctx *)arg;
    struct hashmap *mapcb = hashmap_new_with_allocator(_malloc, _realloc, _free, 
        sizeof(sock_sendcb), ONEK * 4, 0, 0, 
        _mapcb_hash, _mapcb_compare, NULL, NULL);
    while (0 == stop)
    {
        _wait_cmd(&watcher->sender, &cmd);
        switch (cmd.cmd)
        {
        case CMD_STOP:
            stop = 1;
            PRINTD("%s", "stop sender loop.");
            break;
        default:
            _onsender_cmd(watcher, mapcb, &cmd);
            break;
        }
    }
    hashmap_free(mapcb);
}
static void _start_sender(ev_ctx *ctx, watcher_ctx *watcher)
{
    sender_ctx *sender = &watcher->sender;
    sender->wait = 0;
    sender->ev = ctx;
    qu_cmd_init(&sender->qucmd, ONEK * 4);
    cond_init(&sender->cond);
    mutex_init(&sender->mutex);
    thread_init(&sender->thsend);
    thread_creat(&sender->thsend, _loop_sender, watcher);
    thread_wait(&sender->thsend);
}
static void _init_exfunc(ev_ctx *ctx)
{
    SOCKET sock = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, 0);
    ASSERTAB(INVALID_SOCK != sock, ERRORSTR(ERRNO));
    GUID accept_uid = WSAID_ACCEPTEX;
    GUID connect_uid = WSAID_CONNECTEX;
    GUID disconnect_uid = WSAID_DISCONNECTEX;
    ctx->acceptex = _exfunc(sock, &accept_uid);
    ctx->connectex = _exfunc(sock, &connect_uid);
    ctx->disconnectex = _exfunc(sock, &disconnect_uid);
    CLOSE_SOCK(sock);
}
void ev_init(ev_ctx *ctx, uint32_t nthreads)
{
    sock_init();
    ctx->nthreads = (0 == nthreads ? 1 : nthreads);
    ctx->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, ctx->nthreads);
    ASSERTAB(NULL != ctx->iocp, ERRORSTR(ERRNO));
    ctx->nthreads *= 2;
    qu_lsn_init(&ctx->qulsn, 8);
    MALLOC(ctx->watcher, sizeof(watcher_ctx) * ctx->nthreads);
    _init_exfunc(ctx);
    for (uint32_t i = 0; i < ctx->nthreads; i++)
    {
        watcher_ctx *watcher = &ctx->watcher[i];
        thread_init(&watcher->thread);
        thread_creat(&watcher->thread, _loop_event, ctx);
        thread_wait(&watcher->thread);
        _start_sender(ctx, watcher);
    }
}
static void _free_sender(sender_ctx *sender)
{
    mutex_free(&sender->mutex);
    cond_free(&sender->cond);
    qu_cmd_free(&sender->qucmd);
}
void ev_free(ev_ctx *ctx)
{
    uint32_t i;
    cmd_ctx stop;
    stop.cmd = CMD_STOP;
    for (i = 0; i < ctx->nthreads; i++)
    {
        if (!PostQueuedCompletionStatus(ctx->iocp, 0, NOTIFI_EXIT_KEY, NULL))
        {
            LOG_ERROR("%s", ERRORSTR(ERRNO));
        }
        _iocp_cmd(&ctx->watcher[i], &stop);
    }
    for (i = 0; i < ctx->nthreads; i++)
    {
        sender_ctx *sender = &ctx->watcher[i].sender;
        thread_join(&ctx->watcher[i].thread);        
        thread_join(&sender->thsend);
        _free_sender(sender);
    }
    (void)CloseHandle(ctx->iocp);
    FREE(ctx->watcher);
    struct listener_ctx **lsn;
    while (NULL != (lsn = qu_lsn_pop(&ctx->qulsn)))
    {
        _iocp_freelsn(*lsn);
    }
    qu_lsn_free(&ctx->qulsn);
    sock_clean();
}

#endif //EV_IOCP
