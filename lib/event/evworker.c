#include "event/evworker.h"
#include "utils.h"
#include "hashmap.h"
#include "netutils.h"
#include "loger.h"
#include "buffer.h"

#define MAX_CNT_POP      16
#define INIT_SENDBUF_LEN 64
typedef enum CMDS
{
    REQ_STOPLOOP = 0x00,
    REQ_UPDATEUD,
    REQ_DISCONN,
    REQ_ADDFD,
    REQ_SEND,
    EV_ACCEPT,
    EV_CONNECT,
    EV_READ,
    EV_WRITE,

    CMD_CNT,
}CMDS;
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
    qu_bufs qubuf;
}sock_evparam;
typedef struct cmd_ctx
{
    int32_t cmd;
    SOCKET fd;
    void *data;
    ud_cxt ud;
    size_t len;
}cmd_ctx;
QUEUE_DECL(cmd_ctx, qu_cmd);
typedef struct qus_ctx
{
    qu_cmd qu;
    mutex_ctx lck;
}qus_ctx;
typedef struct runner_ctx
{
    volatile int32_t wait;
    uint32_t nqus;
    qus_ctx *qus;
    struct ev_ctx *ev;
    pthread_t thrunner;
    cond_ctx cond;
    mutex_ctx condlck;
}runner_ctx;
typedef struct eworker_ctx
{
    uint32_t nthread;
    struct runner_ctx *runner;
}eworker_ctx;
#define FD_HASH(fd) hash((const char *)&(fd), sizeof(fd))
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
static inline sock_evparam *_map_del(struct hashmap *map, SOCKET fd)
{
    sock_evparam key;
    key.fd = fd;
    return hashmap_delete(map, &key);
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
static inline void _free_bufs(qu_bufs *bufs)
{
    bufs_ctx *buf;
    while (NULL != (buf = qu_bufs_pop(bufs)))
    {
        FREE(buf->data);
    }
    qu_bufs_free(bufs);
}
static inline void _on_close(runner_ctx *runner, struct hashmap *map, sock_evparam *param)
{
    if (NULL != param->cb.c_cb)
    {
        param->cb.c_cb(runner->ev, param->fd, &param->ud);
    }
    _free_sockctx(param->sock);
    _free_bufs(&param->qubuf);
    _map_del(map, param->fd);
}
static inline void _free_mapitem(void *item)
{
    sock_evparam *param = (sock_evparam *)item;
    _free_sockctx(param->sock);
    _free_bufs(&param->qubuf);
}
static void _on_req_stoploop(runner_ctx *runner, struct hashmap *map, cmd_ctx *cmd, int32_t *stop)
{
    *stop = 1;
}
void ev_loop(ev_ctx *ctx, SOCKET sock, recv_cb r_cb, close_cb c_cb, send_cb s_cb, ud_cxt *ud)
{
    ASSERTAB(NULL != r_cb && INVALID_SOCK != sock, ERRSTR_INVPARAM);
    cmd_ctx cmd;
    cmd.cmd = REQ_ADDFD;
    cmd.fd = sock;
    COPY_UD(cmd.ud, ud);
    sock_cb *cbs;
    MALLOC(cbs, sizeof(sock_cb));
    cbs->r_cb = r_cb;
    cbs->c_cb = c_cb;
    cbs->s_cb = s_cb;
    cmd.data = cbs;
    QUS_PUSH(ctx->worker, cmd);
}
static void _on_req_addfd(runner_ctx *runner, struct hashmap *map, cmd_ctx *cmd, int32_t *stop)
{
    sock_evparam param;
    param.cb = *(sock_cb *)cmd->data;
    FREE(cmd->data);
    param.fd = cmd->fd;
    param.ud = cmd->ud;
    param.sock = _new_sockctx(runner->ev, cmd->fd);
    if (NULL == param.sock)
    {
        if (NULL != param.cb.c_cb)
        {
            param.cb.c_cb(runner->ev, param.fd, &param.ud);
            CLOSE_SOCK(param.fd);
        }
        return;
    }
    qu_bufs_init(&param.qubuf, INIT_SENDBUF_LEN);
    sock_evparam *old = hashmap_set(map, &param);
    if (NULL != old)
    {
        _invalid_sockctx(old->sock);
        _free_sockctx(old->sock);
        _free_bufs(&old->qubuf);
        LOG_WARN("%s", "socket repeat.");
    }
    if (ERR_OK != _post_recv(param.sock))
    {
        _on_close(runner, map, &param);
    }
}
void ev_send(ev_ctx *ctx, SOCKET sock, void *data, size_t len, int32_t copy)
{
    ASSERTAB(INVALID_SOCK != sock, ERRSTR_INVPARAM);
    cmd_ctx cmd;
    cmd.cmd = REQ_SEND;
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
    size_t size = qu_bufs_size(&param->qubuf);
    qu_bufs_push(&param->qubuf, &buf);
    if (0 == size)
    {
        (void)_post_send(param->sock);
    }
}
void ev_close(struct ev_ctx *ctx, SOCKET sock)
{
    ASSERTAB(INVALID_SOCK != sock, ERRSTR_INVPARAM);
    cmd_ctx cmd;
    cmd.cmd = REQ_DISCONN;
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
    cmd.cmd = REQ_UPDATEUD;
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
    cmd.cmd = EV_ACCEPT;
    cmd.fd = fd;
    cmd.data = cb;
    COPY_UD(cmd.ud, ud);
    QUS_PUSH(ctx, cmd);
}
static void _on_ev_accept(runner_ctx *runner, struct hashmap *map, cmd_ctx *cmd, int32_t *stop)
{
    _set_sockops(cmd->fd);
    ((accept_cb)cmd->data)(runner->ev, cmd->fd, &cmd->ud);
}
void ewcmd_connect(eworker_ctx *ctx, SOCKET fd, connect_cb cb, ud_cxt *ud)
{
    cmd_ctx cmd;
    cmd.cmd = EV_CONNECT;
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
    cmd.cmd = EV_READ;
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
    buffer_ctx *buf = _get_buffer_r(param->sock);
    size_t nread;
    //读数据前断线检测。。。
    int32_t rtn = buffer_from_sock(buf, param->fd, MAX_RECV_SIZE, &nread, _sock_read, NULL);
    if (0 != nread)
    {
        param->cb.r_cb(runner->ev, param->fd, buf, nread, &param->ud);
    }
    if (ERR_OK == rtn)
    {
        rtn = _post_recv(param->sock);
    }
    if (ERR_OK != rtn)
    {
        _on_close(runner, map, param);
    }
}
void ewcmd_canwrite(eworker_ctx *ctx, SOCKET fd)
{
    cmd_ctx cmd;
    cmd.cmd = EV_WRITE;
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
    int32_t nsend;
    size_t total = 0;
    bufs_ctx *buf;
    while (total < MAX_SEND_SIZE
        && NULL != (buf = qu_bufs_peek(&param->qubuf)))
    {
        nsend = send(param->fd, (char*)buf->data + buf->offset, (int32_t)(buf->len - buf->offset), 0);
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
        total += nsend;
        buf->offset += nsend;
        if (buf->offset == buf->len)
        {
            FREE(buf->data);
            qu_bufs_pop(&param->qubuf);
        }
    }
    if (ERR_OK == err
        && qu_bufs_size(&param->qubuf) > 0)
    {
        (void)_post_send(param->sock);
    }
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
    void(*_on_cmd[CMD_CNT])(runner_ctx *, struct hashmap *, cmd_ctx *, int32_t *);
    _on_cmd[REQ_STOPLOOP] = _on_req_stoploop;
    _on_cmd[REQ_UPDATEUD] = _on_req_updateud;
    _on_cmd[REQ_DISCONN] = _on_req_disconn;
    _on_cmd[REQ_ADDFD] = _on_req_addfd;
    _on_cmd[REQ_SEND] = _on_req_send;
    _on_cmd[EV_ACCEPT] = _on_ev_accept;
    _on_cmd[EV_CONNECT] = _on_ev_connect;
    _on_cmd[EV_READ] = _on_ev_canread;
    _on_cmd[EV_WRITE] = _on_ev_canwrite;
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
        for (i = 0; i < cnt; i++)
        {
            _on_cmd[cmds[i].cmd](runner, map, &cmds[i], &stop);
        }
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
    cmd.cmd = REQ_STOPLOOP;
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
    for (i = 0; i < ctx->nthread; i++)
    {
        runner = &ctx->runner[i];
        mutex_free(&runner->condlck);
        cond_free(&runner->cond);
        _free_qus(runner);
    }
    FREE(ctx->runner);
    FREE(ctx);
}
