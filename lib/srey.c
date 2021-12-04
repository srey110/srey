#include "srey.h"
#include "timer.h"

#define MAX_RUNCNT      10
#define RUNTIME_WARING  50
#define ID_STARTAT      1000
#define TASK_DELAYFREE_TIME          50
#define TASK_MAX_DELAYFREE_CNT       20
struct task_ctx
{
    uint32_t freecnt;
    uint32_t session;
    uint32_t global;
    volatile atomic_t stop;
    volatile atomic_t ref;
    void *handle;
    void *udata;
    struct srey_ctx *srey;
    uint64_t id;
    uint64_t cpu_cost;
    struct module_ctx module;
    struct queue_ctx qu;
    mutex_ctx mu_qu;
};
struct srey_ctx
{
    uint32_t accuracy;//计时器精度
    uint32_t workercnt;
    uint32_t waiting;
    uint32_t free_timeout;
    volatile int32_t bgstop;
    volatile int32_t stop;//停止标志
    volatile atomic_t freecnt;
    struct netev_ctx *netev;//网络
    struct thread_ctx *thr_worker;
    struct map_ctx *map_name;
    struct map_ctx *map_id;
    module_msg_release md_msg_free;
    volatile atomic64_t ids;
    struct thread_ctx thr_tw;
    struct timer_ctx timer;//计时器
    struct tw_ctx tw;//时间轮
    mutex_ctx mu_worker;
    cond_ctx cond_worker;
    struct queue_ctx qu;
    mutex_ctx mu_qu;
};
struct map_task_name
{
    struct task_ctx *task;
    char name[NAME_LENS];
};
struct map_task_id
{
    struct task_ctx *task;
    uint64_t id;
};
#define FILL_NAME(psrc, pname) \
    size_t inamlens = strlen(pname); \
    ASSERTAB(inamlens < NAME_LENS, "name lens error."); \
    memcpy(psrc, pname, inamlens); \
    psrc[inamlens] = '\0'
    
static inline uint64_t _hash_name(void *pval)
{
    return hash(((struct map_task_name *)pval)->name, strlen(((struct map_task_name *)pval)->name));
}
static inline int32_t _compare_name(void *pval1, void *pval2, void *pudata)
{
    return strcmp(((struct map_task_name *)pval1)->name, ((struct map_task_name *)pval2)->name);
}
static inline uint64_t _hash_id(void *pval)
{
    return fnv1a_hash((const char *)&((struct map_task_id *)pval)->id, sizeof(((struct map_task_id *)pval)->id));
}
static inline int32_t _compare_id(void *pval1, void *pval2, void *pudata)
{
    return ((struct map_task_id *)pval1)->id == ((struct map_task_id *)pval2)->id ? ERR_OK : ERR_FAILED;
}
static inline uint32_t _cur_tick(struct srey_ctx *pctx)
{
    return (uint32_t)(timer_nanosec(&pctx->timer) / pctx->accuracy);
}
static inline uint64_t _srey_id(void *pparam)
{
    return (uint64_t)ATOMIC64_ADD(&((struct srey_ctx *)pparam)->ids, 1);
}
static inline struct rwlock_ctx *_map_rwlock(struct srey_ctx *pctx)
{
    return map_rwlock(pctx->map_name);
}
static inline void _srey_grab(struct task_ctx *ptask)
{
    ATOMIC_ADD(&ptask->ref, 1);
}
struct srey_ctx *srey_new(uint32_t uiworker, module_msg_release msgfree, uint32_t uifreetimeout)
{
    struct srey_ctx *pctx = MALLOC(sizeof(struct srey_ctx));
    ASSERTAB(NULL != pctx, ERRSTR_MEMORY);
    srand((uint32_t)time(NULL));
    pctx->bgstop = pctx->stop = pctx->freecnt = pctx->waiting = 0;
    pctx->free_timeout = uifreetimeout;
    pctx->ids = ID_STARTAT;
    pctx->accuracy = 1000 * 1000;
    pctx->md_msg_free = msgfree;
    pctx->map_id = map_new(sizeof(struct map_task_id), _hash_id, _compare_id, NULL);
    pctx->map_name = map_new(sizeof(struct map_task_name), _hash_name, _compare_name, NULL);
    queue_init(&pctx->qu, ONEK);
    mutex_init(&pctx->mu_qu);
    pctx->workercnt = (0 == uiworker ? procscnt() * 2 : uiworker);
    pctx->thr_worker = MALLOC(sizeof(struct thread_ctx) * pctx->workercnt);
    ASSERTAB(NULL != pctx->thr_worker, ERRSTR_MEMORY);
    for (uint32_t i = 0; i < pctx->workercnt; i++)
    {
        thread_init(&pctx->thr_worker[i]);
    }
    mutex_init(&pctx->mu_worker);
    cond_init(&pctx->cond_worker);
    thread_init(&pctx->thr_tw);
    timer_init(&pctx->timer);
    tw_init(&pctx->tw, _cur_tick(pctx));
    pctx->netev = netev_new(&pctx->tw, 0, _srey_id, pctx);

    return pctx;
}
static inline void _srey_task_push(struct task_ctx *ptotask, uint64_t srcid, uint32_t uisess, uint32_t uitype, void *pmsg, uint32_t uisz)
{
    struct message_ctx msg;
    msg.flags = uitype;
    msg.session = uisess;
    msg.size = uisz;
    msg.id = srcid;
    msg.data = pmsg;
    int32_t iadd = 0;
    mutex_lock(&ptotask->mu_qu);
    queue_expand(&ptotask->qu);
    queue_push(&ptotask->qu, &msg);
    if (0 == ptotask->global)
    {
        ptotask->global = 1;
        iadd = 1;
    }
    mutex_unlock(&ptotask->mu_qu);
    if (0 != iadd)
    {
        struct srey_ctx *pctx = ptotask->srey;
        struct message_ctx gmsg;
        gmsg.data = ptotask;
        mutex_lock(&pctx->mu_qu);
        queue_expand(&pctx->qu);
        queue_push(&pctx->qu, &gmsg);
        mutex_unlock(&pctx->mu_qu);
        if (pctx->waiting > 0)
        {
            cond_signal(&pctx->cond_worker);
        }
    }
}
static inline int32_t _iter_stop(void *pitem, void *pudata)
{
    struct task_ctx *ptask = ((struct map_task_id *)pitem)->task;
    if (ATOMIC_CAS(&ptask->stop, 0, 1))
    {
        _srey_task_push(ptask, 0, 0, MSG_TYPE_STOP, NULL, 0);
    }
    return ERR_OK;
}
void srey_free(struct srey_ctx *pctx)
{
    pctx->bgstop = 1;
    struct rwlock_ctx *plock = _map_rwlock(pctx);
    rwlock_rdlock(plock);
    ATOMIC_SET(&pctx->freecnt, (atomic_t)_map_size(pctx->map_id));
    _map_iter(pctx->map_id, _iter_stop, NULL);
    rwlock_unlock(plock);
    uint32_t uitimeout = 0;
    while (ATOMIC_GET(&pctx->freecnt) > 0
        && uitimeout < pctx->free_timeout)
    {
        MSLEEP(10);
        uitimeout += 10;
    }

    netev_free(pctx->netev);
    pctx->stop = 1;
    thread_join(&pctx->thr_tw);
    cond_broadcast(&pctx->cond_worker);
    for (uint32_t i = 0; i < pctx->workercnt; i++)
    {
        thread_join(&pctx->thr_worker[i]);
    }
    mutex_free(&pctx->mu_worker);
    cond_free(&pctx->cond_worker);
    tw_free(&pctx->tw);
    map_free(pctx->map_id);
    map_free(pctx->map_name);
    queue_free(&pctx->qu);
    mutex_free(&pctx->mu_qu);
    FREE(pctx->thr_worker);
    FREE(pctx);
}
static inline void _push_global(struct srey_ctx *pctx, struct task_ctx *ptask)
{
    int32_t iadd = 0;
    mutex_lock(&ptask->mu_qu);
    if (0 == queue_size(&ptask->qu))
    {
        ptask->global = 0;
    }
    else
    {
        iadd = 1;
    }
    mutex_unlock(&ptask->mu_qu);
    if (0 != iadd)
    {
        struct message_ctx gmsg;
        gmsg.data = ptask;
        mutex_lock(&pctx->mu_qu);
        queue_expand(&pctx->qu);
        queue_push(&pctx->qu, &gmsg);
        mutex_unlock(&pctx->mu_qu);
    }
}
static inline void _task_msg_free(struct srey_ctx *pctx, void *pmsg)
{
    if (NULL != pctx->md_msg_free
        && NULL != pmsg)
    {
        pctx->md_msg_free(pmsg);
    }
}
static inline void _srey_release(struct ud_ctx *pud)
{
    struct task_ctx *ptask = (struct task_ctx *)pud->handle;
    if (NULL != ptask)
    {
        srey_release(ptask);
    }
}
static inline void _srey_clean(struct srey_ctx *pctx, struct task_ctx *ptask, struct message_ctx *pmsg)
{
    switch (pmsg->flags)
    {
    case MSG_TYPE_TIMEOUT:
        srey_release(ptask);
        break;
    case MSG_TYPE_REQUEST:
    case MSG_TYPE_RESPONSE:
        _task_msg_free(pctx, pmsg->data);
        break;
    case MSG_TYPE_CONNECT:
        if (ERR_OK != pmsg->size)
        {
            sock_free(pmsg->data, NULL);
        }
        srey_release(ptask);
        break;
    case MSG_TYPE_RECVFROM:
        FREE(pmsg->data);
        break;
    case MSG_TYPE_CLOSE:
        sock_free(pmsg->data, _srey_release);
        break;
    }
}
void _task_delay_free(struct ud_ctx *pud)
{
    struct task_ctx *ptask = (struct task_ctx *)pud->handle;
    mutex_lock(&ptask->mu_qu);
    int32_t icnt = queue_size(&ptask->qu);
    mutex_unlock(&ptask->mu_qu);
    if ((int32_t)ATOMIC_GET(&ptask->ref) <= 0
        && 0 == icnt)
    {
        if (0 != ptask->srey->bgstop)
        {
            struct map_task_id mapid;
            mapid.id = ptask->id;
            struct rwlock_ctx *plock = _map_rwlock(ptask->srey);
            rwlock_wrlock(plock);
            if (ERR_OK == _map_remove(ptask->srey->map_id, &mapid, NULL))
            {
                struct map_task_name mapname;
                FILL_NAME(mapname.name, ptask->module.name);
                ASSERTAB(ERR_OK == _map_remove(ptask->srey->map_name, &mapname, NULL), "logic error.");
            }
            rwlock_unlock(plock);
        }
        _srey_task_push(ptask, 0, 0, MSG_TYPE_FREE, NULL, 0);
        return;
    }
    ptask->freecnt++;
    if (ptask->freecnt >= TASK_MAX_DELAYFREE_CNT)
    {
        LOG_WARN("free task %s use long time. ref %d queue size %d.",
            ptask->module.name, ATOMIC_GET(&ptask->ref), icnt);
        ptask->freecnt = 0;
    }
    tw_add(&ptask->srey->tw, TASK_DELAYFREE_TIME, _task_delay_free, pud);
}
static inline void _task_free(struct task_ctx *ptask)
{
    struct srey_ctx *psrey = ptask->srey;
    if (NULL != ptask->module.md_free)
    {
        ptask->module.md_free(ptask, ptask->handle, ptask->udata);
    }
    queue_free(&ptask->qu);
    mutex_free(&ptask->mu_qu);
    FREE(ptask);
    if (ATOMIC_GET(&psrey->freecnt) > 0)
    {
        ATOMIC_ADD(&psrey->freecnt, -1);
    }
}
static inline void _msg_dispatch(struct srey_ctx *pctx, struct timer_ctx *ptimer, struct task_ctx *ptask)
{
    int32_t irtn;
    uint64_t ulcost;
    struct message_ctx msg;
    for (uint32_t i = 0; i < ptask->module.maxcnt; i++)
    {
        mutex_lock(&ptask->mu_qu);
        irtn = queue_pop(&ptask->qu, &msg);
        mutex_unlock(&ptask->mu_qu);
        if (ERR_OK != irtn)
        {
            break;
        }

        switch (msg.flags)
        {
        case MSG_TYPE_STOP:
            if (NULL != ptask->module.md_stop)
            {
                ptask->module.md_stop(ptask, ptask->handle, ptask->udata);
            }
            struct ud_ctx ud;
            ud.handle = (uintptr_t)ptask;
            tw_add(&ptask->srey->tw, TASK_DELAYFREE_TIME, _task_delay_free, &ud);         
            ATOMIC_ADD(&ptask->ref, -1);
            break;
        case MSG_TYPE_FREE:
            _task_free(ptask);
            return;
        default:
            timer_start(ptimer);
            ptask->module.md_run(ptask, msg.flags, msg.id, msg.session, msg.data, msg.size, ptask->udata);
            ulcost = timer_elapsed(ptimer) / (1000 * 1000);
            ptask->cpu_cost += ulcost;
            if (ulcost >= RUNTIME_WARING)
            {
                LOG_WARN("task %s type %d,run long time.", ptask->module.name, msg.flags);
            }
            _srey_clean(pctx, ptask, &msg);
            break;
        }
    }
    _push_global(pctx, ptask);
}
static void _worker(void *pparam)
{
    int32_t irtn;
    struct message_ctx gmsg;
    struct timer_ctx timer;
    struct srey_ctx *pctx = (struct srey_ctx *)pparam;
    timer_init(&timer);
    while (0 == pctx->stop)
    {
        mutex_lock(&pctx->mu_qu);
        irtn = queue_pop(&pctx->qu, &gmsg);
        mutex_unlock(&pctx->mu_qu);
        if (ERR_OK != irtn)
        {
            mutex_lock(&pctx->mu_worker);
            pctx->waiting++;
            if (0 == pctx->stop)
            {
                cond_wait(&pctx->cond_worker, &pctx->mu_worker);
            }
            pctx->waiting--;
            mutex_unlock(&pctx->mu_worker);
        }
        else
        {
            _msg_dispatch(pctx, &timer, gmsg.data);
        }
    }
}
static void _tw_loop(void *pparam)
{
    struct srey_ctx *pctx = (struct srey_ctx *)pparam;
    while (0 == pctx->stop)
    {
        tw_run(&pctx->tw, _cur_tick(pctx));
        USLEEP(1);
    }
}
void srey_loop(struct srey_ctx *pctx)
{
    for (uint32_t i = 0; i < pctx->workercnt; i++)
    {
        thread_creat(&pctx->thr_worker[i], _worker, pctx);
        thread_waitstart(&pctx->thr_worker[i]);
    }
    thread_creat(&pctx->thr_tw, _tw_loop, pctx);
    thread_waitstart(&pctx->thr_tw);
    netev_loop(pctx->netev);
}
struct task_ctx *srey_newtask(struct srey_ctx *pctx, struct module_ctx *pmodule, void *pudata)
{
    ASSERTAB(NULL != pmodule->md_run, ERRSTR_NULLP);
    struct map_task_name mapname;
    FILL_NAME(mapname.name, pmodule->name);
    struct rwlock_ctx *plock = _map_rwlock(pctx);
    rwlock_rdlock(plock);
    int32_t irtn = _map_get(pctx->map_name, &mapname, NULL);
    rwlock_unlock(plock);
    if (ERR_OK == irtn)
    {
        LOG_ERROR("task %s already registered.", pmodule->name);
        return NULL;
    }

    struct task_ctx *ptask = MALLOC(sizeof(struct task_ctx));
    if (NULL == ptask)
    {
        LOG_ERROR("%s", ERRSTR_MEMORY);
        return NULL;
    }
    ZERO(ptask, sizeof(struct task_ctx));
    ptask->ref = 1;
    ptask->srey = pctx;
    uint64_t id = _srey_id(pctx);
    ptask->id = id;
    ptask->session = 1;
    ptask->udata = pudata;
    queue_init(&ptask->qu, ONEK);
    mutex_init(&ptask->mu_qu);
    pmodule->maxcnt = pmodule->maxcnt > MAX_RUNCNT ? MAX_RUNCNT : pmodule->maxcnt;
    memcpy(&ptask->module, pmodule, sizeof(struct module_ctx));
    if (NULL != ptask->module.md_new)
    {
        ptask->handle = ptask->module.md_new(ptask, pudata);
    }
    mapname.task = ptask;
    struct map_task_id mapid;
    mapid.id = id;
    mapid.task = ptask;
    rwlock_wrlock(plock);
    _map_set(pctx->map_name, &mapname);
    _map_set(pctx->map_id, &mapid);
    rwlock_unlock(plock);
    if (NULL != ptask->module.md_init)
    {
        if (ERR_OK != ptask->module.md_init(ptask, ptask->handle, pudata))
        {
            srey_release(ptask);
            LOG_ERROR("init task %s failed.", pmodule->name);
            return NULL;
        }
    }

    return ptask;
}
struct task_ctx *srey_grabnam(struct srey_ctx *pctx, const char *pname)
{
    struct task_ctx *ptask = NULL;
    struct map_task_name mapname;
    FILL_NAME(mapname.name, pname);
    struct rwlock_ctx *plock = _map_rwlock(pctx);
    rwlock_rdlock(plock);
    if (ERR_OK == _map_get(pctx->map_name, &mapname, &mapname))
    {
        ptask = mapname.task;
        _srey_grab(ptask);
    }
    rwlock_unlock(plock);

    return ptask;
}
struct task_ctx *srey_grabid(struct srey_ctx *pctx, uint64_t id)
{
    struct task_ctx *ptask = NULL;
    struct map_task_id mapid;
    mapid.id = id;
    struct rwlock_ctx *plock = _map_rwlock(pctx);
    rwlock_rdlock(plock);
    if (ERR_OK == _map_get(pctx->map_id, &mapid, &mapid))
    {
        ptask = mapid.task;
        _srey_grab(ptask);
    }
    rwlock_unlock(plock);

    return ptask;
}
void srey_release(struct task_ctx *ptask)
{
    if (1 == ATOMIC_GET(&ptask->ref)
        && ATOMIC_CAS(&ptask->stop, 0, 1))
    {
        struct srey_ctx *pctx = ptask->srey;
        struct map_task_id mapid;
        mapid.id = ptask->id;
        struct rwlock_ctx *plock = _map_rwlock(pctx);
        rwlock_wrlock(plock);
        if (ERR_OK == _map_remove(pctx->map_id, &mapid, &mapid))
        {
            struct map_task_name mapname;
            FILL_NAME(mapname.name, ptask->module.name);
            ASSERTAB(ERR_OK == _map_remove(pctx->map_name, &mapname, NULL), "logic error.");
        }
        rwlock_unlock(plock);
        _srey_task_push(ptask, 0, 0, MSG_TYPE_STOP, NULL, 0);
        return;
    }

    ATOMIC_ADD(&ptask->ref, -1);
}
void srey_call(struct task_ctx *ptask, void *pmsg, uint32_t uisz)
{
    _srey_task_push(ptask, 0, 0, MSG_TYPE_REQUEST, pmsg, uisz);
}
void srey_request(struct task_ctx *ptask, uint64_t srcid, uint32_t uisess, void *pmsg, uint32_t uisz)
{
    _srey_task_push(ptask, srcid, uisess, MSG_TYPE_REQUEST, pmsg, uisz);
}
void srey_response(struct task_ctx *ptask, uint32_t uisess, void *pmsg, uint32_t uisz)
{
    _srey_task_push(ptask, 0, uisess, MSG_TYPE_RESPONSE, pmsg, uisz);
}
static inline void _srey_timeout(struct ud_ctx *pud)
{
    struct srey_ctx *pctx = (struct srey_ctx *)pud->handle;
    struct map_task_id mapid;
    mapid.id = pud->id;
    struct rwlock_ctx *plock = _map_rwlock(pctx);
    rwlock_rdlock(plock);
    if (ERR_OK == _map_get(pctx->map_id, &mapid, &mapid))
    {
        _srey_grab(mapid.task);
        _srey_task_push(mapid.task, 0, pud->session, MSG_TYPE_TIMEOUT, NULL, 0);
    }
    rwlock_unlock(plock);
}
void srey_timeout(struct task_ctx *ptask, uint32_t uisess, uint32_t uitimeout)
{
    struct ud_ctx ud;
    ud.handle = (uintptr_t)ptask->srey;
    ud.session = uisess;
    ud.id = ptask->id;
    tw_add(&ptask->srey->tw, uitimeout, _srey_timeout, &ud);
}
static inline void _srey_sock_accept(struct sock_ctx *psock, struct ud_ctx *pud)
{
    _srey_task_push((struct task_ctx *)pud->handle, 0, 0, MSG_TYPE_ACCEPT, psock, 0);
}
struct listener_ctx *srey_listener(struct task_ctx *ptask, const char *phost, uint16_t usport)
{
    struct ud_ctx ud;
    ud.handle = (uintptr_t)ptask;
    struct listener_ctx *plsn = netev_listener(ptask->srey->netev, phost, usport, _srey_sock_accept, &ud);
    if (NULL != plsn)
    {
        _srey_grab(ptask);
    }
    return plsn;
}
void srey_freelsn(struct listener_ctx *plsn)
{
    listener_free(plsn, _srey_release);
}
static inline void _srey_sock_connect(struct sock_ctx *psock, int32_t ierr, struct ud_ctx *pud)
{
    _srey_task_push((struct task_ctx *)pud->handle, 0, pud->session, MSG_TYPE_CONNECT, psock, (uint32_t)ierr);
}
struct sock_ctx *srey_connecter(struct task_ctx *ptask, uint32_t uisess, uint32_t utimeout, const char *phost, uint16_t usport)
{
    struct ud_ctx ud;
    ud.session = uisess;
    ud.handle = (uintptr_t)ptask;
    struct sock_ctx *psock = netev_connecter(ptask->srey->netev, utimeout, phost, usport, _srey_sock_connect, &ud);
    if (NULL != psock)
    {
        _srey_grab(ptask);
    }
    return psock;
}
struct sock_ctx *srey_newsock(struct task_ctx *ptask, SOCKET sock, int32_t itype, int32_t ifamily)
{
    return netev_add_sock(ptask->srey->netev, sock, itype, ifamily);
}
static inline void _srey_sock_recv(struct sock_ctx *psock, size_t uisize, union netaddr_ctx *paddr, struct ud_ctx *pud)
{
    if (SOCK_STREAM == sock_type(psock))
    {
        _srey_task_push((struct task_ctx *)pud->handle, 0, 0, MSG_TYPE_RECV, psock, (uint32_t)uisize);
    }
    else
    {
        struct udp_recv_msg *pmsg = MALLOC(sizeof(struct udp_recv_msg));
        if (NULL == pmsg)
        {
            LOG_ERROR("%s", ERRSTR_MEMORY);
            buffer_drain(sock_buffer_r(psock), uisize);
            return;
        }
        pmsg->sock = psock;
        pmsg->port = netaddr_port(paddr);
        netaddr_ip(paddr, pmsg->ip);
        _srey_task_push((struct task_ctx *)pud->handle, 0, 0, MSG_TYPE_RECVFROM, pmsg, (uint32_t)uisize);
    }
}
static inline void _srey_sock_send(struct sock_ctx *psock, size_t uisize, struct ud_ctx *pud)
{
    _srey_task_push((struct task_ctx *)pud->handle, 0, 0, MSG_TYPE_SEND, psock, (uint32_t)uisize);
}
static inline void _srey_sock_close(struct sock_ctx *psock, struct ud_ctx *pud)
{
    _srey_task_push((struct task_ctx *)pud->handle, 0, 0, MSG_TYPE_CLOSE, psock, 0);
}
int32_t srey_enable(struct task_ctx *ptask, struct sock_ctx *psock, int32_t iwrite)
{
    struct ud_ctx ud;
    ud.handle = (uintptr_t)ptask;
    send_cb scb = NULL;
    if (0 != iwrite)
    {
        scb = _srey_sock_send;
    }
    int32_t irtn = netev_enable_rw(ptask->srey->netev, psock, _srey_sock_recv, scb, _srey_sock_close, &ud);
    if (ERR_OK == irtn)
    {
        _srey_grab(ptask);
    }
    return irtn;
}
uint32_t task_new_session(struct task_ctx *ptask)
{
    if (UINT_MAX == ptask->session)
    {
        ptask->session = 1;
    }
    return ptask->session++;
}
uint64_t task_id(struct task_ctx *ptask)
{
    return ptask->id;
}
const char *task_name(struct task_ctx *ptask)
{
    return ptask->module.name;
}
uint64_t task_cpucost(struct task_ctx *ptask)
{
    return ptask->cpu_cost;
}
