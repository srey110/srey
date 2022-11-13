#include "event/evworker.h"
#include "utils.h"
#include "hashmap.h"
#include "netutils.h"
#include "loger.h"
#include "buffer.h"

#define MAX_CNT_POP      5
typedef enum WORKER_CMDS
{
    WCMD_STOPLOOP = 0x00,
    WCMD_UPDATEUD,
    WCMD_DISCONN,
    WCMD_ADDFD,
    WCMD_SEND,
    WCMD_ACCEPT,
    WCMD_CONNECT,
    WCMD_READ,
    WCMD_WRITE,
    WCMD_ERROR,

    WCMD_TOTAL,
}WORKER_CMDS;
typedef struct sock_cb
{
    recv_cb r_cb;
    send_cb s_cb;
    close_cb c_cb;
}sock_cb;
typedef struct sock_evparam
{
    SOCKET fd;
    sock_cb cb;
    struct sock_ctx *sock;
    ud_cxt ud;
}sock_evparam;
typedef struct cmd_ctx
{
    int32_t cmd;
    SOCKET fd;
    void *data;
    size_t len;
    ud_cxt ud;
    sock_cb cbs;
}cmd_ctx;
QUEUE_DECL(cmd_ctx, qu_cmd);
typedef struct qus_ctx
{
    qu_cmd qu;
    mutex_ctx lck;
}qus_ctx;
QUEUE_DECL(struct sock_ctx *, qu_pool);
typedef struct runner_ctx
{
    volatile int32_t wait;
    uint32_t nqus;
    qus_ctx *qus;
    ev_ctx *ev;
    pthread_t thrunner;
    cond_ctx cond;
    mutex_ctx condlck;
    qu_pool pool;
}runner_ctx;
typedef struct eworker_ctx
{
    uint32_t nthread;
    struct runner_ctx *runner;
}eworker_ctx;
#define QUS_PUSH(ew, cmd)\
do {\
    uint64_t hs = FD_HASH(cmd.fd);\
    runner_ctx *runner = (1 == (ew)->nthread) ? (ew)->runner : (&(ew)->runner[hs % (ew)->nthread]);\
    _qus_push(runner, hs % runner->nqus, &cmd);\
} while (0)

static inline uint64_t _map_hash(const void *item, uint64_t seed0, uint64_t seed1)
{
    SOCKET sock = ((const sock_evparam *)item)->fd;
    return hash((const char *)&sock, sizeof(sock));
}
static inline int _map_compare(const void *a, const void *b, void *udata)
{
    return (int)(((const sock_evparam *)a)->fd - ((const sock_evparam *)b)->fd);
}
static inline sock_evparam *_map_get(struct hashmap *map, SOCKET fd)
{
    sock_evparam key;
    key.fd = fd;
    return hashmap_get(map, &key);
}
static inline void _qus_push(runner_ctx *runner, uint32_t index, cmd_ctx *cmd)
{
    mutex_lock(&runner->qus[index].lck);
    qu_cmd_push(&runner->qus[index].qu, cmd);
    mutex_unlock(&runner->qus[index].lck);
    if (runner->wait > 0)
    {
        cond_signal(&runner->cond);
    }
}
static inline int32_t _qus_pop(runner_ctx *runner, cmd_ctx *cmds)
{
    uint32_t j;
    int32_t index = 0;
    cmd_ctx *cmd;
    for (uint32_t i = 0; i < runner->nqus; i++)
    {
        mutex_lock(&runner->qus[i].lck);
        for (j = 0; j < MAX_CNT_POP; j++)
        {
            cmd = qu_cmd_pop(&runner->qus[i].qu);
            if (NULL == cmd)
            {
                break;
            }
            cmds[index] = *cmd;
            index++;
        }
        mutex_unlock(&runner->qus[i].lck);
    }
    return index;
}
static void _on_req_stoploop(runner_ctx *runner, struct hashmap *map, cmd_ctx *cmd, int32_t *stop)
{
    *stop = 1;
}
void ev_loop(ev_ctx *ctx, SOCKET sock, recv_cb r_cb, close_cb c_cb, send_cb s_cb, ud_cxt *ud)
{
    ASSERTAB(NULL != r_cb && INVALID_SOCK != sock, ERRSTR_INVPARAM);
    cmd_ctx cmd;
    cmd.cmd = WCMD_ADDFD;
    cmd.fd = sock;
    COPY_UD(cmd.ud, ud);
    cmd.cbs.r_cb = r_cb;
    cmd.cbs.c_cb = c_cb;
    cmd.cbs.s_cb = s_cb;
    QUS_PUSH(ctx->worker, cmd);
}
static inline void _on_close(runner_ctx *runner, struct hashmap *map, sock_evparam *param)
{
    struct sock_ctx *sock = param->sock;
    sock_evparam key;
    key.fd = param->fd;
    if (NULL != param->cb.c_cb)
    {
        param->cb.c_cb(runner->ev, param->fd, &param->ud);
    }
    _close_sockctx(sock);
    hashmap_delete(map, &key);
    qu_pool_push(&runner->pool, &sock);
}
static void _on_req_addfd(runner_ctx *runner, struct hashmap *map, cmd_ctx *cmd, int32_t *stop)
{
    sock_evparam param;
    param.cb = cmd->cbs;
    param.fd = cmd->fd;
    param.ud = cmd->ud;
    if (qu_pool_size(&runner->pool) > 0)
    {
        param.sock = *qu_pool_pop(&runner->pool);
        _reset_sockctx(param.sock, cmd->fd);
    }
    else
    {
        param.sock = _new_sockctx(runner->ev, cmd->fd);
    }
    ASSERTAB(NULL == hashmap_set(map, &param), "addfd repeat"); 
    if (ERR_OK != _post_recv(runner->ev, param.sock))
    {
        _on_close(runner, map, &param);
    }
}
void ev_send(ev_ctx *ctx, SOCKET sock, void *data, size_t len, int32_t copy)
{
    ASSERTAB(INVALID_SOCK != sock, ERRSTR_INVPARAM);
    cmd_ctx cmd;
    cmd.cmd = WCMD_SEND;
    cmd.fd = sock;
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
    QUS_PUSH(ctx->worker, cmd);
}
static void _on_req_send(runner_ctx *runner, struct hashmap *map, cmd_ctx *cmd, int32_t *stop)
{
    sock_evparam *param = _map_get(map, cmd->fd);
    if (NULL == param)
    {
        FREE(cmd->data);
        return;
    }
    bufs_ctx buf;
    buf.data = cmd->data;
    buf.len = cmd->len;
    buf.offset = 0;
    qu_bufs *sendbufs = _get_send_buf(param->sock);
    size_t size = qu_bufs_size(sendbufs);
    qu_bufs_push(sendbufs, &buf);
    if (0 == size)
    {
        (void)_post_send(runner->ev, param->sock);
    }
}
void ev_close(struct ev_ctx *ctx, SOCKET sock)
{
    ASSERTAB(INVALID_SOCK != sock, ERRSTR_INVPARAM);
    cmd_ctx cmd;
    cmd.cmd = WCMD_DISCONN;
    cmd.fd = sock;
    QUS_PUSH(ctx->worker, cmd);
}
static void _on_req_disconn(runner_ctx *runner, struct hashmap *map, cmd_ctx *cmd, int32_t *stop)
{
    sock_evparam *param = _map_get(map, cmd->fd);
    shutdown(cmd->fd, SHUT_RD);
    if (NULL == param)
    {
        CLOSE_SOCK(cmd->fd);
    }
}
void ev_updateud(ev_ctx *ctx, SOCKET sock, free_ud f_cb, ud_cxt *ud)
{
    ASSERTAB(INVALID_SOCK != sock, ERRSTR_INVPARAM);
    cmd_ctx cmd;
    cmd.cmd = WCMD_UPDATEUD;
    cmd.fd = sock;
    cmd.data = f_cb;
    COPY_UD(cmd.ud, ud);
    QUS_PUSH(ctx->worker, cmd);
}
static void _on_req_updateud(runner_ctx *runner, struct hashmap *map, cmd_ctx *cmd, int32_t *stop)
{
    sock_evparam *param = _map_get(map, cmd->fd);
    if (NULL == param)
    {
        if (NULL != cmd->data)
        {
            ((free_ud)cmd->data)(&cmd->ud);
        }
        return;
    }
    param->ud = cmd->ud;
}
void ewcmd_accept(eworker_ctx *ctx, SOCKET fd, accept_cb cb, ud_cxt *ud)
{
    cmd_ctx cmd;
    cmd.cmd = WCMD_ACCEPT;
    cmd.fd = fd;
    cmd.data = cb;
    COPY_UD(cmd.ud, ud);
    QUS_PUSH(ctx, cmd);
}
static void _on_ev_accept(runner_ctx *runner, struct hashmap *map, cmd_ctx *cmd, int32_t *stop)
{
    if (ERR_OK != _set_sockops(cmd->fd))
    {
        CLOSE_SOCK(cmd->fd);
        return;
    }
    ((accept_cb)cmd->data)(runner->ev, cmd->fd, &cmd->ud);
}
void ewcmd_connect(eworker_ctx *ctx, SOCKET fd, connect_cb cb, ud_cxt *ud)
{
    cmd_ctx cmd;
    cmd.cmd = WCMD_CONNECT;
    cmd.fd = fd;
    cmd.data = cb;
    COPY_UD(cmd.ud, ud);
    QUS_PUSH(ctx, cmd);
}
static void _on_ev_connect(runner_ctx *runner, struct hashmap *map, cmd_ctx *cmd, int32_t *stop)
{
    if (ERR_OK != sock_checkconn(cmd->fd))
    {
        CLOSE_SOCK(cmd->fd);
        ((connect_cb)cmd->data)(runner->ev, INVALID_SOCK, &cmd->ud);
    }
    else
    {
        ((connect_cb)cmd->data)(runner->ev, cmd->fd, &cmd->ud);
    }
}
void ewcmd_canread(eworker_ctx *ctx, SOCKET fd)
{
    cmd_ctx cmd;
    cmd.cmd = WCMD_READ;
    cmd.fd = fd;
    QUS_PUSH(ctx, cmd);
}
static inline int32_t _sock_read(SOCKET fd, void *buf, size_t len, void *arg)
{
    int32_t rtn = recv(fd, (char*)buf, (int32_t)len, 0);
    if (0 == rtn)
    {
        return ERR_FAILED;
    }
    if (rtn < 0)
    {
        if (!IS_EAGAIN(ERRNO))
        {
            return ERR_FAILED;
        }
        return ERR_OK;
    }
    return rtn;
}
static void _on_ev_canread(runner_ctx *runner, struct hashmap *map, cmd_ctx *cmd, int32_t *stop)
{
    sock_evparam *param = _map_get(map, cmd->fd);
    if (NULL == param)
    {
        return;
    }
    buffer_ctx *buf = _get_recv_buf(param->sock);
    size_t nread;
    //读数据前断线检测。。。
    int32_t rtn = buffer_from_sock(buf, param->fd, &nread, _sock_read, NULL);
    if (0 != nread)
    {
        param->cb.r_cb(runner->ev, param->fd, buf, nread, &param->ud);
    }
    if (ERR_OK == rtn)
    {
        rtn = _post_recv(runner->ev, param->sock);
    }
    if (ERR_OK != rtn)
    {
        _on_close(runner, map, param);
    }
}
void ewcmd_canwrite(eworker_ctx *ctx, SOCKET fd)
{
    cmd_ctx cmd;
    cmd.cmd = WCMD_WRITE;
    cmd.fd = fd;
    QUS_PUSH(ctx, cmd);
}
static void _on_ev_canwrite(runner_ctx *runner, struct hashmap *map, cmd_ctx *cmd, int32_t *stop)
{
    sock_evparam *param = _map_get(map, cmd->fd);
    if (NULL == param)
    {
        return;
    }
    int32_t err = ERR_OK;
    int32_t nsend, size;
    bufs_ctx *buf;
    qu_bufs *sendbufs = _get_send_buf(param->sock);
    while (NULL != (buf = qu_bufs_peek(sendbufs)))
    {
        size = (int32_t)(buf->len - buf->offset);
        nsend = send(param->fd, (char*)buf->data + buf->offset, size, 0);
        if (0 == nsend)
        {
            err = ERR_FAILED;
            break;
        }
        if (0 > nsend)
        {
            if (!IS_EAGAIN(ERRNO))
            {
                err = ERR_FAILED;
            }
            break;
        }
        if (nsend < size)
        {
            buf->offset += nsend;
            break;
        }
        FREE(buf->data);
        qu_bufs_pop(sendbufs);
    }
    if (ERR_OK == err
        && qu_bufs_size(sendbufs) > 0)
    {
        (void)_post_send(runner->ev, param->sock);
    }
}
void ewcmd_error(struct eworker_ctx *ctx, SOCKET fd)
{
    cmd_ctx cmd;
    cmd.cmd = WCMD_ERROR;
    cmd.fd = fd;
    QUS_PUSH(ctx, cmd);
}
static void _on_ev_error(runner_ctx *runner, struct hashmap *map, cmd_ctx *cmd, int32_t *stop)
{
    sock_evparam *param = _map_get(map, cmd->fd);
    if (NULL == param)
    {
        CLOSE_SOCK(cmd->fd);
        return;
    }
    _on_close(runner, map, param);
}
static inline void _free_mapitem(void *item)
{
    sock_evparam *param = (sock_evparam *)item;
    _free_sockctx(param->sock);
}
static void _loop_runner(void *arg)
{
    runner_ctx *runner = (runner_ctx *)arg;
    int32_t cnt, i, stop = 0;
    cmd_ctx *cmds;
    size_t maxcnt = MAX_CNT_POP * runner->nqus;
    MALLOC(cmds, sizeof(cmd_ctx) * maxcnt);    
    struct hashmap *map = hashmap_new_with_allocator(_malloc, _realloc, _free,
        sizeof(sock_evparam), ONEK * 4, 0, 0, _map_hash, _map_compare, _free_mapitem, NULL);

    void(*_on_cmd[WCMD_TOTAL])(runner_ctx *, struct hashmap *, cmd_ctx *, int32_t *);
    _on_cmd[WCMD_STOPLOOP] = _on_req_stoploop;
    _on_cmd[WCMD_UPDATEUD] = _on_req_updateud;
    _on_cmd[WCMD_DISCONN] = _on_req_disconn;
    _on_cmd[WCMD_ADDFD] = _on_req_addfd;
    _on_cmd[WCMD_SEND] = _on_req_send;
    _on_cmd[WCMD_ACCEPT] = _on_ev_accept;
    _on_cmd[WCMD_CONNECT] = _on_ev_connect;
    _on_cmd[WCMD_READ] = _on_ev_canread;
    _on_cmd[WCMD_WRITE] = _on_ev_canwrite;
    _on_cmd[WCMD_ERROR] = _on_ev_error;
    while (!stop)
    {
        mutex_lock(&runner->condlck);
        while (0 == (cnt = _qus_pop(runner, cmds)))
        {
            runner->wait++;
            cond_timedwait(&runner->cond, &runner->condlck, 100);
            runner->wait--;
        }
        mutex_unlock(&runner->condlck);
        do
        {
            for (i = 0; i < cnt; i++)
            {
                _on_cmd[cmds[i].cmd](runner, map, &cmds[i], &stop);
            }
        } while (0 != (cnt = _qus_pop(runner, cmds)));
    }
    FREE(cmds);
    hashmap_free(map);
}
static void _init_qus(runner_ctx *runner)
{
    MALLOC(runner->qus, sizeof(qus_ctx) * runner->nqus);
    for (uint32_t i = 0; i < runner->nqus; i++)
    {
        qu_cmd_init(&runner->qus[i].qu, ONEK * 4);
        mutex_init(&runner->qus[i].lck);
    }
}
eworker_ctx *eworker_init(struct ev_ctx *ev, uint32_t nthread, uint32_t nqus)
{
    eworker_ctx *ctx;
    MALLOC(ctx, sizeof(eworker_ctx));
    ctx->nthread = nthread;
    MALLOC(ctx->runner, sizeof(runner_ctx) * ctx->nthread);
    runner_ctx *runner;
    for (uint32_t i = 0; i < ctx->nthread; i++)
    {
        runner = &ctx->runner[i];
        runner->wait = 0;
        runner->nqus = nqus;
        runner->ev = ev;
        _init_qus(runner);
        cond_init(&runner->cond);
        mutex_init(&runner->condlck);
        qu_pool_init(&runner->pool, ONEK * 4);
        runner->thrunner = thread_creat(_loop_runner, runner);
    }
    return ctx;
}
static void _free_qus(runner_ctx *runner)
{
    for (uint32_t i = 0; i < runner->nqus; i++)
    {
        qu_cmd_free(&runner->qus[i].qu);
        mutex_free(&runner->qus[i].lck);
    }
    FREE(runner->qus);
}
void eworker_free(eworker_ctx *ctx)
{
    uint32_t i;
    runner_ctx *runner;
    cmd_ctx cmd;
    cmd.cmd = WCMD_STOPLOOP;
    for (i = 0; i < ctx->nthread; i++)
    {
        runner = &ctx->runner[i];
        _qus_push(runner, runner->nqus - 1, &cmd);
    }
    for (i = 0; i < ctx->nthread; i++)
    {
        runner = &ctx->runner[i];
        thread_join(runner->thrunner);
    }
    struct sock_ctx **sock;
    for (i = 0; i < ctx->nthread; i++)
    {
        runner = &ctx->runner[i];
        mutex_free(&runner->condlck);
        cond_free(&runner->cond);
        _free_qus(runner);
        while (NULL != (sock = qu_pool_pop(&runner->pool)))
        {
            _free_sockctx(*sock);
        }
        qu_pool_free(&runner->pool);
    }
    FREE(ctx->runner);
    FREE(ctx);
}
