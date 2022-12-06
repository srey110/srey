#include "event/worker.h"
#include "event/event.h"
#include "event/skpool.h"
#include "thread.h"
#include "hashmap.h"
#include "buffer.h"
#include "cond.h"
#include "sarray.h"
#include "timer.h"
#include "loger.h"

#ifdef EV_IOCP

#define DELAY_TIMEOUT     1000
typedef enum WORKER_CMDS
{
    CMD_STOP = 0x00,
    CMD_DISCONN,
    CMD_ADD,
    CMD_REMOVE,
    CMD_SEND,

    CMD_TOTAL,
}WORKER_CMDS;
typedef struct map_element
{
    SOCKET fd;
    struct sock_ctx *sock;
}map_element;
//ÃüÁî
typedef struct cmd_ctx
{
    int32_t cmd;
    SOCKET fd;
    void *data;
    size_t len;
}cmd_ctx;
QUEUE_DECL(cmd_ctx, qu_cmd);
typedef struct delay_ctx
{
    int32_t timeout;
    struct sock_ctx *sock;
}delay_ctx;
ARRAY_DECL(delay_ctx, arr_delay);
//¶ÓÁÐ
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
    arr_delay arrdelay;
    skpool_ctx pool;
}runner_ctx;
typedef struct worker_ctx
{
    uint32_t nthread;
    ev_ctx *ev;
    runner_ctx *runner;
    void(*cmd_callback[CMD_TOTAL])(struct runner_ctx *, cmd_ctx *, int32_t *);
}worker_ctx;
#define QUS_PUSH(worker, cmd)\
do {\
    uint64_t hs = FD_HASH(cmd.fd);\
    runner_ctx *runner = GET_PTR((worker)->runner, (worker)->nthread, hs);\
    _qus_push(runner, GET_POS(hs, runner->nqus), &cmd);\
} while (0)

static inline uint64_t _map_hash(const void *item, uint64_t seed0, uint64_t seed1)
{
    return FD_HASH(((const map_element *)item)->fd);
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
runner_ctx *worker_get_runner(worker_ctx *worker, uint64_t hs)
{
    return GET_PTR(worker->runner, worker->nthread, hs);
}
struct sock_ctx *runner_newsk(runner_ctx *runner, SOCKET fd, cbs_ctx *cbs, ud_cxt *ud)
{
    return pool_pop(&runner->pool, fd, cbs, ud);
}
void runner_freesk(runner_ctx *runner, struct sock_ctx *skctx)
{
    pool_push(&runner->pool, skctx);
}
static void _on_cmd_stop(runner_ctx *runner, cmd_ctx *cmd, int32_t *stop)
{
    *stop = 1;
}
void runner_addsk(runner_ctx *runner, SOCKET fd, struct sock_ctx *skctx, uint64_t hs)
{
    cmd_ctx cmd;
    cmd.cmd = CMD_ADD;
    cmd.fd = fd;
    cmd.data = skctx;
    _qus_push(runner, GET_POS(hs, runner->nqus), &cmd);
}
static void _on_cmd_add(runner_ctx *runner, cmd_ctx *cmd, int32_t *stop)
{
    map_element el;
    el.fd = cmd->fd;
    el.sock = cmd->data;
    ASSERTAB(NULL == hashmap_set(runner->element, &el), "socket repeat.");
}
void runner_removesk(struct runner_ctx *runner, SOCKET fd, uint64_t hs)
{
    cmd_ctx cmd;
    cmd.cmd = CMD_REMOVE;
    cmd.fd = fd;
    _qus_push(runner, GET_POS(hs, runner->nqus), &cmd);
}
void worker_removesk(struct worker_ctx *worker, SOCKET fd)
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
    if (0 == _check_canfree(el->sock))
    {
        if (SOCK_STREAM == _sock_type(el->sock))
        {
            pool_push(&runner->pool, el->sock);
        }
        else
        {
            _free_udp(el->sock);
        }
    }
    else
    {
        delay_ctx delay;
        delay.timeout = 0;
        delay.sock = el->sock;
        arr_delay_push_back(&runner->arrdelay, &delay);
    }
    map_element key;
    key.fd = cmd->fd;
    hashmap_delete(runner->element, &key);
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
void ev_sendto(ev_ctx *ctx, SOCKET fd, const char *host, const uint16_t port, void *data, size_t len)
{
    ASSERTAB(INVALID_SOCK != fd, ERRSTR_INVPARAM);
    cmd_ctx cmd;
    cmd.cmd = CMD_SEND;
    cmd.fd = fd;
    cmd.len = len;
    MALLOC(cmd.data, sizeof(netaddr_ctx) + len);
    netaddr_ctx *addr = (netaddr_ctx *)cmd.data;
    if (ERR_OK != netaddr_sethost(addr, host, port))
    {
        FREE(cmd.data);
        LOG_WARN("%s", ERRORSTR(ERRNO));
        return;
    }
    memcpy((char *)cmd.data + sizeof(netaddr_ctx), data, len);
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
    if (SOCK_STREAM == _sock_type(el->sock))
    {
        _add_bufs_trypost(el->sock, &buf);
    }
    else
    {
        _add_bufs_trysendto(el->sock, &buf);
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
    if (NULL == el)
    {
        CLOSE_SOCK(cmd->fd);
        return;
    }
    if (SOCK_STREAM == _sock_type(el->sock))
    {
        _sk_shutdown(el->sock);
        return;
    }
    CLOSE_SOCK(cmd->fd);
}
static void _init_callback(worker_ctx *worker)
{
    worker->cmd_callback[CMD_STOP] = _on_cmd_stop;
    worker->cmd_callback[CMD_DISCONN] = _on_cmd_disconn;
    worker->cmd_callback[CMD_ADD] = _on_cmd_add;
    worker->cmd_callback[CMD_REMOVE] = _on_cmd_remove;
    worker->cmd_callback[CMD_SEND] = _on_cmd_send;
}
static inline void _trywait_cond(runner_ctx *runner)
{
    mutex_lock(&runner->condlck);
    while (0 == _qus_size(runner))
    {
        runner->wait++;
        cond_timedwait(&runner->cond, &runner->condlck, 20);
        runner->wait--;
    }
    mutex_unlock(&runner->condlck);
}
static inline void _loop_cmd(runner_ctx *runner, int32_t *stop)
{
    uint32_t i;
    int32_t have;
    cmd_ctx cmd, *tmp;
    do
    {
        have = 0;
        for (i = 0; i < runner->nqus; i++)
        {
            mutex_lock(&runner->qus[i].lck);
            tmp = qu_cmd_pop(&runner->qus[i].qu);
            if (NULL != tmp)
            {
                cmd = *tmp;
                have = 1;
            }
            mutex_unlock(&runner->qus[i].lck);
            if (NULL != tmp)
            {
                runner->worker->cmd_callback[cmd.cmd](runner, &cmd, stop);
            }
        }
    } while (have);
}
static inline void _check_delayfree(runner_ctx *runner, timer_ctx *timer, arr_delay *arr)
{
    int32_t elapsed = (int32_t)timer_elapsed_ms(timer);
    if (elapsed < 100)
    {
        return;
    }
    timer_start(timer);
    delay_ctx *delay;
    int32_t size = (int32_t)arr_delay_size(arr);
    for (int32_t i = size - 1; i >= 0; i--)
    {
        delay = arr_delay_at(arr, i);
        if (0 == _check_canfree(delay->sock))
        {
            if (SOCK_STREAM == _sock_type(delay->sock))
            {
                pool_push(&runner->pool, delay->sock);
            }
            else
            {
                _free_udp(delay->sock);
            }
            arr_delay_del_nomove(arr, i);
        }
        else
        {
            delay->timeout += elapsed;
            if (delay->timeout >= DELAY_TIMEOUT)
            {
                int32_t type = _sock_type(delay->sock);
                if (SOCK_STREAM == type)
                {
                    pool_push(&runner->pool, delay->sock);
                }
                else
                {
                    _free_udp(delay->sock);
                }
                arr_delay_del_nomove(arr, i);
                LOG_WARN("wait socket free timeout,type: %d", type);
            }
        }
    }
}
static inline void _pool_shrink(runner_ctx *runner, timer_ctx *timer)
{
    uint64_t elapsed = timer_elapsed_ms(timer);
    if (elapsed < SHRINK_TIME)
    {
        return;
    }
    timer_start(timer);
    pool_shrink(&runner->pool, hashmap_count(runner->element) / 2);
}
static void _loop_runner(void *arg)
{
    runner_ctx *runner = (runner_ctx *)arg;
    int32_t stop = 0;
    timer_ctx tmdelay;
    timer_ctx tmshrink;
    timer_init(&tmdelay);
    timer_init(&tmshrink);
    timer_start(&tmdelay);
    timer_start(&tmshrink);
    while (!stop)
    {
        _trywait_cond(runner);
        _loop_cmd(runner, &stop);
        _check_delayfree(runner, &tmdelay, &runner->arrdelay);
        _pool_shrink(runner, &tmshrink);
    }
}
static inline void _free_mapitem(void *item)
{
    map_element *el = (map_element *)item;
    if (SOCK_STREAM == _sock_type(el->sock))
    {
        _free_sk(el->sock);
    }
    else
    {
        _free_udp(el->sock);
    }
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
    _init_callback(worker);
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
        arr_delay_init(&runner->arrdelay, 128);
        pool_init(&runner->pool, ONEK);
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
static void _free_arr_delay(arr_delay *arr)
{
    delay_ctx *delay;
    size_t size = arr_delay_size(arr);
    for (size_t i = 0; i < size; i++)
    {
        delay = arr_delay_at(arr, i);
        if (SOCK_STREAM == _sock_type(delay->sock))
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
        _free_arr_delay(&runner->arrdelay);
        pool_free(&runner->pool);
    }
    FREE(worker->runner);
    FREE(worker);
}

#endif//EV_IOCP
