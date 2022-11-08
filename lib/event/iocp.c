#include "event/iocp.h"
#include "hashmap.h"

#ifdef EV_IOCP

#define NOTIFI_EXIT_KEY  ((ULONG_PTR)-1)
exfuncs_ctx _exfuncs;
static volatile atomic_t _init_once = 0;

typedef struct sock_sendcb
{
    SOCKET sock;
    send_cb cb;
    void *ud;
}sock_sendcb;

static inline uint64_t _mapcb_hash(const void *item, uint64_t seed0, uint64_t seed1)
{
    SOCKET sock = ((const sock_sendcb *)item)->sock;
    return hash((const char *)&sock, sizeof(sock));
}
static inline int _mapcb_compare(const void *a, const void *b, void *udata)
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
static inline void _wait_event(ev_ctx *ctx, int32_t *stop)
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
static void _loop_iocp(void *arg)
{
    int32_t stop = 0;
    ev_ctx *ctx = (ev_ctx *)arg;
    while (0 == stop)
    {
        _wait_event(ctx, &stop);
    }
}
watcher_ctx *_get_watcher(ev_ctx *ctx, SOCKET fd)
{
    return (1 == ctx->nthreads) ? &(ctx->watcher[0]) :
        (&ctx->watcher[hash((const char *)&fd, sizeof(fd)) % ctx->nthreads]);
}
void _send_cmd(watcher_ctx *watcher, cmd_ctx *cmd)
{
    mutex_lock(&watcher->sender.mutex);
    qu_cmd_push(&watcher->sender.qucmd, cmd);
    if (watcher->sender.wait > 0)
    {
        cond_signal(&watcher->sender.cond);
    }
    mutex_unlock(&watcher->sender.mutex);
}

void _cmd_add(ev_ctx *ctx, SOCKET sock, send_cb cb, void *ud)
{
    if (INVALID_SOCK == sock)
    {
        return;
    }
    cmd_ctx cmd;
    cmd.cmd = CMD_ADD;
    cmd.sock = sock;
    cmd.data = cb;
    cmd.ud = ud;
    _send_cmd(_get_watcher(ctx, sock), &cmd);
}
static inline void _on_cmd_add(struct hashmap *mapcb, cmd_ctx *cmd)
{
    sock_sendcb cb;
    cb.sock = cmd->sock;
    cb.cb = cmd->data;
    cb.ud = cmd->ud;
    if (NULL != hashmap_set(mapcb, &cb))
    {
        LOG_WARN("%s", "socket add repeatedly.");
    }
}
void _cmd_remove(ev_ctx *ctx, SOCKET sock)
{
    if (INVALID_SOCK == sock)
    {
        return;
    }
    cmd_ctx cmd;
    cmd.cmd = CMD_REMOVE;
    cmd.sock = sock;
    _send_cmd(_get_watcher(ctx, sock), &cmd);
}
static inline void _on_cmd_remove(struct hashmap *mapcb, cmd_ctx *cmd)
{
    sock_sendcb key;
    key.sock = cmd->sock;
    hashmap_delete(mapcb, &key);
    CLOSE_SOCK(cmd->sock);
}
void ev_close(ev_ctx *ctx, SOCKET sock)
{
    if (INVALID_SOCK == sock)
    {
        return;
    }
    cmd_ctx cmd;
    cmd.cmd = CMD_CLOSE;
    cmd.sock = sock;
    _send_cmd(_get_watcher(ctx, sock), &cmd);
}
static inline void _on_cmd_close(watcher_ctx *watcher, struct hashmap *mapcb, cmd_ctx *cmd)
{
    sock_sendcb key;
    key.sock = cmd->sock;
    if (NULL != hashmap_delete(mapcb, &key))
    {
        _post_disconn(watcher->sender.ev, cmd->sock);
    }
    else
    {
        CLOSE_SOCK(cmd->sock);
    }
}
static inline void _on_cmd_send(watcher_ctx *watcher, struct hashmap *mapcb, cmd_ctx *cmd)
{
    sock_sendcb key;
    key.sock = cmd->sock;
    sock_sendcb *cb = hashmap_get(mapcb, &key);
    if (NULL == cb)
    {
        FREE(cmd->data);
        return;
    }
    if (ERR_OK != _post_send(watcher->sender.ev, cb->sock, cb->cb, cmd->data, cmd->len, cb->ud))
    {
        if (NULL != cb->cb)
        {
            cb->cb(watcher->sender.ev, cb->sock, 0, cmd->ud, ERR_FAILED);
        }
        FREE(cmd->data);
    }
}
static inline void _on_cmd(watcher_ctx *watcher, struct hashmap *mapcb, cmd_ctx *cmd)
{
    switch (cmd->cmd)
    {
    case CMD_ADD:
        _on_cmd_add(mapcb, cmd);
        break;
    case CMD_REMOVE:
        _on_cmd_remove(mapcb, cmd);
        break;
    case CMD_SEND:
        _on_cmd_send(watcher, mapcb, cmd);
        break;
    case CMD_CLOSE:
        _on_cmd_close(watcher, mapcb, cmd);
        break;
    default:
        break;
    }
}
static inline void _wait_cmd(sender_ctx *sender, cmd_ctx *cmd)
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
            break;
        default:
            _on_cmd(watcher, mapcb, &cmd);
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
static void _init_exfuncs(ev_ctx *ctx)
{
    if (ATOMIC_CAS(&_init_once, 0, 1))
    {
        SOCKET sock = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, 0);
        ASSERTAB(INVALID_SOCK != sock, ERRORSTR(ERRNO));
        GUID accept_uid = WSAID_ACCEPTEX;
        GUID connect_uid = WSAID_CONNECTEX;
        GUID disconnect_uid = WSAID_DISCONNECTEX;
        _exfuncs.acceptex = _exfunc(sock, &accept_uid);
        _exfuncs.connectex = _exfunc(sock, &connect_uid);
        _exfuncs.disconnectex = _exfunc(sock, &disconnect_uid);
        CLOSE_SOCK(sock);
    }
}
void ev_init(ev_ctx *ctx, uint32_t nthreads)
{
    sock_init();
    ctx->nthreads = (0 == nthreads ? 1 : nthreads);
    ctx->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, ctx->nthreads);
    ASSERTAB(NULL != ctx->iocp, ERRORSTR(ERRNO));
    ctx->nthreads *= 2;
    mutex_init(&ctx->mulsn);
    qu_lsn_init(&ctx->qulsn, 8);
    MALLOC(ctx->watcher, sizeof(watcher_ctx) * ctx->nthreads);
    _init_exfuncs(ctx);
    for (uint32_t i = 0; i < ctx->nthreads; i++)
    {
        watcher_ctx *watcher = &ctx->watcher[i];
        thread_init(&watcher->thread);
        thread_creat(&watcher->thread, _loop_iocp, ctx);
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
        _send_cmd(&ctx->watcher[i], &stop);
    }
    for (i = 0; i < ctx->nthreads; i++)
    {
        sender_ctx *sender = &ctx->watcher[i].sender;
        thread_join(&ctx->watcher[i].thread);        
        thread_join(&sender->thsend);
        _free_sender(sender);
    }    
    FREE(ctx->watcher);
    struct listener_ctx **lsn;
    while (NULL != (lsn = qu_lsn_pop(&ctx->qulsn)))
    {
        _freelsn(*lsn);
    }
    qu_lsn_free(&ctx->qulsn);
    mutex_free(&ctx->mulsn);
    (void)CloseHandle(ctx->iocp);
    sock_clean();
}

#endif //EV_IOCP
