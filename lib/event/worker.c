#include "event/worker.h"
#include "event/event.h"
#include "thread.h"
#include "hashmap.h"
#include "buffer.h"
#include "cond.h"

#ifdef EV_IOCP

typedef enum WORKER_CMDS
{
    CMD_STOP = 0x00,
    CMD_UPDATE,
    CMD_DISCONN,
    CMD_ADD,
    CMD_REMOVE,
    CMD_SEND,
    CMD_CANREAD,
    CMD_CANWRITE,

    CMD_TOTAL,
}WORKER_CMDS;
typedef struct map_element
{
    SOCKET fd;
    struct sock_ctx *sock;
    rw_cb_ctx rw_cb;
    ud_cxt ud;
}map_element;
//命令
typedef struct cmd_ctx
{
    int32_t cmd;
    SOCKET fd;
    void *data;
    size_t len;
    ud_cxt ud;
    rw_cb_ctx rw_cb;
}cmd_ctx;
QUEUE_DECL(cmd_ctx, qu_cmd);
//队列
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
    struct worker_ctx *worker;
    struct hashmap *element;
    pthread_t thrunner;
    cond_ctx cond;
    mutex_ctx condlck;
}runner_ctx;
typedef struct worker_ctx
{
    uint32_t nthread;
    ev_ctx *ev;
    runner_ctx *runner;
}worker_ctx;
#define QUS_PUSH(worker, cmd)\
do {\
    uint64_t hs = FD_HASH(cmd.fd);\
    runner_ctx *runner = (1 == (worker)->nthread) ? (worker)->runner : (&(worker)->runner[hs % (worker)->nthread]);\
    _qus_push(runner, hs % runner->nqus, &cmd);\
} while (0)

static inline uint64_t _map_hash(const void *item, uint64_t seed0, uint64_t seed1)
{
    SOCKET fd = ((const map_element *)item)->fd;
    return FD_HASH(fd);
}
static inline int _map_compare(const void *a, const void *b, void *ud)
{
    return (int)(((const map_element *)a)->fd - ((const map_element *)b)->fd);
}
static inline map_element *_map_get(struct hashmap *map, SOCKET fd)
{
    map_element key;
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
static inline size_t _qus_size(runner_ctx *runner)
{
    size_t size = 0;
    for (uint32_t i = 0; i < runner->nqus; i++)
    {
        size += qu_cmd_size(&runner->qus[i].qu);
    }
    return size;
}
static void _on_cmd_stop(runner_ctx *runner, cmd_ctx *cmd, int32_t *stop)
{
    *stop = 1;
}
void worker_add(struct worker_ctx *worker, SOCKET fd, struct sock_ctx *skctx, rw_cb_ctx *cbs, ud_cxt *ud)
{
    cmd_ctx cmd;
    cmd.cmd = CMD_ADD;
    cmd.fd = fd;
    cmd.data = skctx;
    cmd.rw_cb = *cbs;
    COPY_UD(cmd.ud, ud);
    QUS_PUSH(worker, cmd);
}
static void _on_cmd_add(runner_ctx *runner, cmd_ctx *cmd, int32_t *stop)
{
    map_element el;
    el.fd = cmd->fd;
    el.sock = cmd->data;
    el.rw_cb = cmd->rw_cb;
    el.ud = cmd->ud;
    ASSERTAB(NULL == hashmap_set(runner->element, &el), "socket repeat.");
}
void worker_remove(struct worker_ctx *worker, SOCKET fd)
{
    cmd_ctx cmd;
    cmd.cmd = CMD_REMOVE;
    cmd.fd = fd;
    QUS_PUSH(worker, cmd);
}
static void _on_cmd_remove(runner_ctx *runner, cmd_ctx *cmd, int32_t *stop)
{
    map_element *el = _map_get(runner->element, cmd->fd);
    if (NULL == el)
    {
        return;
    }
    _free_sockctx(el->sock);
    hashmap_delete(runner->element, el);
}
void ev_send(ev_ctx *ctx, SOCKET fd, void *data, size_t len, int32_t copy)
{
    ASSERTAB(INVALID_SOCK != fd, ERRSTR_INVPARAM);
    cmd_ctx cmd;
    cmd.cmd = CMD_SEND;
    cmd.fd = fd;
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
static void _on_cmd_send(runner_ctx *runner, cmd_ctx *cmd, int32_t *stop)
{
    map_element *el = _map_get(runner->element, cmd->fd);
    if (NULL == el)
    {
        FREE(cmd->data);
        return;
    }
    bufs_ctx buf;
    buf.data = cmd->data;
    buf.len = cmd->len;
    buf.offset = 0;
    qu_bufs *sendbufs = _get_send_buf(el->sock);
    size_t size = qu_bufs_size(sendbufs);
    qu_bufs_push(sendbufs, &buf);
    if (0 == size)
    {
        (void)_post_send(el->sock);
    }
}
void ev_close(struct ev_ctx *ctx, SOCKET fd)
{
    ASSERTAB(INVALID_SOCK != fd, ERRSTR_INVPARAM);
    cmd_ctx cmd;
    cmd.cmd = CMD_DISCONN;
    cmd.fd = fd;
    QUS_PUSH(ctx->worker, cmd);
}
static void _on_cmd_disconn(runner_ctx *runner, cmd_ctx *cmd, int32_t *stop)
{
    map_element *el = _map_get(runner->element, cmd->fd);
    shutdown(cmd->fd, SHUT_RD);
    if (NULL == el)
    {
        CLOSE_SOCK(cmd->fd);
    }
}
void ev_update(ev_ctx *ctx, SOCKET fd,  freeud_cb fud_cb, ud_cxt *ud)
{
    ASSERTAB(INVALID_SOCK != fd, ERRSTR_INVPARAM);
    cmd_ctx cmd;
    cmd.cmd = CMD_UPDATE;
    cmd.fd = fd;
    cmd.data = fud_cb;
    COPY_UD(cmd.ud, ud);
    QUS_PUSH(ctx->worker, cmd);
}
static void _on_cmd_update(runner_ctx *runner, cmd_ctx *cmd, int32_t *stop)
{
    map_element *el = _map_get(runner->element, cmd->fd);
    if (NULL == el)
    {
        if (NULL != cmd->data)
        {
            ((freeud_cb)cmd->data)(&cmd->ud);
        }
        return;
    }
    el->ud = cmd->ud;
}
static inline void _on_close(runner_ctx *runner, map_element *el)
{
    if (NULL != el->rw_cb.c_cb)
    {
        el->rw_cb.c_cb(runner->worker->ev, el->fd, &el->ud);
    }
    _free_sockctx(el->sock);
    hashmap_delete(runner->element, el);
}
void worker_canread(worker_ctx *worker, SOCKET fd)
{
    cmd_ctx cmd;
    cmd.cmd = CMD_CANREAD;
    cmd.fd = fd;
    QUS_PUSH(worker, cmd);
}
static void _on_cmd_canread(runner_ctx *runner, cmd_ctx *cmd, int32_t *stop)
{
    map_element *el = _map_get(runner->element, cmd->fd);
    if (NULL == el)
    {
        return;
    }
    buffer_ctx *buf = _get_recv_buf(el->sock);
    size_t nread;
    //读数据前断线检测。。。
    int32_t rtn = buffer_from_sock(buf, el->fd, &nread, _sock_read, NULL);
    if (0 != nread)
    {
        el->rw_cb.r_cb(runner->worker->ev, el->fd, buf, nread, &el->ud);
    }
    if (ERR_OK == rtn)
    {
        rtn = _post_recv(el->sock);
    }
    if (ERR_OK != rtn)
    {
        _on_close(runner, el);
    }
}
void worker_canwrite(worker_ctx *worker, SOCKET fd)
{
    cmd_ctx cmd;
    cmd.cmd = CMD_CANWRITE;
    cmd.fd = fd;
    QUS_PUSH(worker, cmd);
}
static void _on_cmd_canwrite(runner_ctx *runner, cmd_ctx *cmd, int32_t *stop)
{
    map_element *el = _map_get(runner->element, cmd->fd);
    if (NULL == el)
    {
        return;
    }
    size_t nsend;
    qu_bufs *sendbufs = _get_send_buf(el->sock);
    int32_t rtn = _sock_send(el->fd, sendbufs, &nsend, NULL);
    if (NULL != el->rw_cb.s_cb
        && 0 != nsend)
    {
        el->rw_cb.s_cb(runner->worker->ev, el->fd, nsend, &el->ud);
    }
    if (ERR_OK == rtn
        && qu_bufs_size(sendbufs) > 0)
    {
        (void)_post_send(el->sock);
    }
}
static void _loop_runner(void *arg)
{
    runner_ctx *runner = (runner_ctx *)arg;
    cmd_ctx cmd, *tmp;
    uint32_t i;
    int32_t more, stop = 0;

    void(*_on_cmd[CMD_TOTAL])(runner_ctx *, cmd_ctx *, int32_t *);
    _on_cmd[CMD_STOP] = _on_cmd_stop;
    _on_cmd[CMD_UPDATE] = _on_cmd_update;
    _on_cmd[CMD_DISCONN] = _on_cmd_disconn;
    _on_cmd[CMD_ADD] = _on_cmd_add;
    _on_cmd[CMD_REMOVE] = _on_cmd_remove;
    _on_cmd[CMD_SEND] = _on_cmd_send;
    _on_cmd[CMD_CANREAD] = _on_cmd_canread;
    _on_cmd[CMD_CANWRITE] = _on_cmd_canwrite;

    while (!stop)
    {
        mutex_lock(&runner->condlck);
        while (0 == _qus_size(runner))
        {
            runner->wait++;
            cond_timedwait(&runner->cond, &runner->condlck, 20);
            runner->wait--;
        }
        mutex_unlock(&runner->condlck);
        more = 1;
        do
        {
            more = 0;
            for (i = 0; i < runner->nqus; i++)
            {
                mutex_lock(&runner->qus[i].lck);
                tmp = qu_cmd_pop(&runner->qus[i].qu);
                if (NULL != tmp)
                {
                    cmd = *tmp;
                    more = 1;
                }
                mutex_unlock(&runner->qus[i].lck);
                if (NULL != tmp)
                {
                    _on_cmd[cmd.cmd](runner, &cmd, &stop);
                }
            }
        } while (more);
    }
}
static inline void _free_mapitem(void *item)
{
    map_element *el = (map_element *)item;
    _free_sockctx(el->sock);
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
worker_ctx *worker_init(ev_ctx *ev, uint32_t nthread, uint32_t nqus)
{
    worker_ctx *worker;
    MALLOC(worker, sizeof(worker_ctx));
    worker->nthread = nthread;
    worker->ev = ev;
    MALLOC(worker->runner, sizeof(runner_ctx) * worker->nthread);
    runner_ctx *runner;
    for (uint32_t i = 0; i < worker->nthread; i++)
    {
        runner = &worker->runner[i];
        runner->wait = 0;
        runner->nqus = nqus;
        runner->worker = worker;
        runner->element = hashmap_new_with_allocator(_malloc, _realloc, _free,
            sizeof(map_element), ONEK * 4, 0, 0, _map_hash, _map_compare, _free_mapitem, NULL);
        _init_qus(runner);
        cond_init(&runner->cond);
        mutex_init(&runner->condlck);
        runner->thrunner = thread_creat(_loop_runner, runner);
    }
    return worker;
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
void worker_free(worker_ctx *worker)
{
    uint32_t i;
    runner_ctx *runner;
    cmd_ctx cmd;
    cmd.cmd = CMD_STOP;
    for (i = 0; i < worker->nthread; i++)
    {
        runner = &worker->runner[i];
        _qus_push(runner, runner->nqus - 1, &cmd);
    }
    for (i = 0; i < worker->nthread; i++)
    {
        runner = &worker->runner[i];
        thread_join(runner->thrunner);
    }
    for (i = 0; i < worker->nthread; i++)
    {
        runner = &worker->runner[i];
        mutex_free(&runner->condlck);
        cond_free(&runner->cond);
        _free_qus(runner);
        hashmap_free(runner->element);
    }
    FREE(worker->runner);
    FREE(worker);
}

#endif//EV_IOCP
