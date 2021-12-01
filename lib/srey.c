#include "srey.h"
#include "timer.h"

#define MAX_RUNCNT      10
#define RUNTIME_WARING  50
#define ID_STARTAT      1000
struct task_ctx
{
    uint32_t freecnt;
    uint32_t unreg;
    uint32_t session;
    uint32_t global;
    volatile atomic_t ref;
    void *handle;
    void *udata;
    struct srey_ctx *srey;
    sid_t id;
    struct module_ctx module;
    struct queue_ctx qu;
    mutex_ctx mu_qu;
};
struct srey_ctx
{
    uint32_t accuracy;//计时器精度
    uint32_t workercnt;
    uint32_t waiting;
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
    sid_t id;
};
struct msg_broadcast
{
    uint32_t size;
    volatile atomic_t ref;
    struct srey_ctx *srey;
    void *msg;
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
static inline sid_t _srey_id(void *pparam)
{
    return (sid_t)ATOMIC64_ADD(&((struct srey_ctx *)pparam)->ids, 1);
}
struct srey_ctx *srey_new(uint32_t uiworker, module_msg_release msgfree)
{
    struct srey_ctx *pctx = MALLOC(sizeof(struct srey_ctx));
    ASSERTAB(NULL != pctx, ERRSTR_MEMORY);
    srand((uint32_t)time(NULL));
    pctx->stop = pctx->freecnt = pctx->waiting = 0;
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
static inline struct rwlock_ctx *_map_rwlock(struct srey_ctx *pctx)
{
    return map_rwlock(pctx->map_name);
}
void srey_free(struct srey_ctx *pctx)
{
    struct queue_ctx quid;
    queue_init(&quid, 128);
    srey_allid(pctx, &quid);
    ATOMIC_SET(&pctx->freecnt, queue_size(&quid));
    struct message_ctx msgid;
    while (ERR_OK == queue_pop(&quid, &msgid))
    {
        srey_unregister(pctx, msgid.id);
    }
    queue_free(&quid);
    uint32_t uitimeout = 0;
    while (ATOMIC_GET(&pctx->freecnt) > 0
        && uitimeout <= 5000)
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
static inline void _msg_clean(struct srey_ctx *pctx, struct message_ctx *pmsg)
{
    switch (pmsg->flags)
    {
    case MSG_TYPE_CALL:
    case MSG_TYPE_REQUEST:
    case MSG_TYPE_RESPONSE:
        _task_msg_free(pctx, pmsg->data);
        break;
    case MSG_TYPE_BROADCAST:
        {
            struct msg_broadcast *pbc = pmsg->data;
            if (ATOMIC_CAS(&pbc->ref, 1, 0))
            {
                _task_msg_free(pctx, pbc->msg);
                FREE(pbc);
            }
            else
            {
                ATOMIC_ADD(&pbc->ref, -1);
            }
        }
        break;
    case MSG_TYPE_CONNECT:
        if (ERR_OK != pmsg->size)
        {
            sock_free(pmsg->data);
        }
        break;
    case MSG_TYPE_RECVFROM:
        FREE(pmsg->data);
        break;
    case MSG_TYPE_CLOSE:
        sock_free(pmsg->data);
        break;
    }
}
static inline void _task_free(struct task_ctx *ptask)
{
    struct srey_ctx *psrey = ptask->srey;
    if (NULL != ptask->module.release)
    {
        ptask->module.release(ptask, ptask->handle, ptask->udata);
    }
    queue_free(&ptask->qu);
    mutex_free(&ptask->mu_qu);
    FREE(ptask);
    if (ATOMIC_GET(&psrey->freecnt) > 0)
    {
        ATOMIC_ADD(&psrey->freecnt, -1);
    }
}
void _task_delay_free(struct ud_ctx *pud)
{
    struct task_ctx *ptask = (struct task_ctx *)pud->handle;
    if (0 == ATOMIC_GET(&ptask->ref))
    {
        _task_free(ptask);
        return;
    }
    ptask->freecnt++;
    if (ptask->freecnt >= MAX_DELAYFREE_CNT)
    {
        LOG_WARN("free task %s use long time.", ptask->module.name);
        ptask->freecnt = 0;
    }
    tw_add(&ptask->srey->tw, DELAYFREE_TIME, _task_delay_free, pud);
}
static inline void _msg_dispatch(struct srey_ctx *pctx, struct timer_ctx *ptimer, struct task_ctx *ptask)
{
    int32_t irtn;
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
        case MSG_TYPE_UNREG:
            {
                if (0 == ATOMIC_GET(&ptask->ref))
                {
                    _task_free(ptask);
                }
                else
                {
                    struct ud_ctx ud;
                    ud.handle = (uintptr_t)ptask;
                    tw_add(&ptask->srey->tw, DELAYFREE_TIME, _task_delay_free, &ud);
                }
            }
            return;
        default:
            timer_start(ptimer);
            ptask->module.run(ptask, msg.flags, msg.id, msg.session,
                (MSG_TYPE_BROADCAST == msg.flags ? ((struct msg_broadcast *)msg.data)->msg : msg.data),
                (MSG_TYPE_BROADCAST == msg.flags ? ((struct msg_broadcast *)msg.data)->size : msg.size),
                ptask->udata);
            if (timer_elapsed(ptimer) / (1000 * 1000) >= RUNTIME_WARING)
            {
                LOG_WARN("task %s type %d,run long time.", ptask->module.name, msg.flags);
            }
            _msg_clean(pctx, &msg);
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
sid_t srey_register(struct srey_ctx *pctx, struct module_ctx *pmodule, void *pudata)
{
    ASSERTAB(NULL != pmodule->run, ERRSTR_NULLP);
    struct map_task_name mapname;
    FILL_NAME(mapname.name, pmodule->name);
    struct rwlock_ctx *plock = _map_rwlock(pctx);
    rwlock_rdlock(plock);
    int32_t irtn = _map_get(pctx->map_name, &mapname, NULL);
    rwlock_unlock(plock);
    if (ERR_OK == irtn)
    {
        LOG_ERROR("task %s already registered.", pmodule->name);
        return 0;
    }
    struct task_ctx *ptask = MALLOC(sizeof(struct task_ctx));
    if (NULL == ptask)
    {
        LOG_ERROR("%s", ERRSTR_MEMORY);
        return 0;
    }

    ptask->ref = ptask->unreg = ptask->freecnt = ptask->global = 0;
    ptask->srey = pctx;
    sid_t id = _srey_id(pctx);
    ptask->id = id;
    ptask->session = 1;
    ptask->udata = pudata;
    queue_init(&ptask->qu, ONEK);
    mutex_init(&ptask->mu_qu);
    pmodule->maxcnt = pmodule->maxcnt > MAX_RUNCNT ? MAX_RUNCNT : pmodule->maxcnt;
    memcpy(&ptask->module, pmodule, sizeof(struct module_ctx));
    ptask->handle = NULL;
    if (NULL != ptask->module.create)
    {
        ptask->handle = ptask->module.create(ptask, pudata);
    }

    mapname.task = ptask;
    struct map_task_id mapid;
    mapid.id = id;
    mapid.task = ptask;
    ATOMIC_ADD(&ptask->ref, 1);
    rwlock_wrlock(plock);
    _map_set(pctx->map_name, &mapname);
    _map_set(pctx->map_id, &mapid);
    rwlock_unlock(plock);
    if (NULL != ptask->module.init)
    {
        irtn = ptask->module.init(ptask, ptask->handle, pudata);
        if (ERR_OK != irtn)
        {
            srey_unregister(pctx, id);
            ATOMIC_ADD(&ptask->ref, -1);
            LOG_ERROR("init task %s failed. return code %d.", pmodule->name, irtn);
            return 0;
        }
    }
    ATOMIC_ADD(&ptask->ref, -1);

    return id;
}
sid_t srey_queryid(struct srey_ctx *pctx, const char *pname)
{
    sid_t id = 0;
    struct map_task_name mapname;
    FILL_NAME(mapname.name, pname);
    struct rwlock_ctx *plock = _map_rwlock(pctx);
    rwlock_rdlock(plock);
    if (ERR_OK == _map_get(pctx->map_name, &mapname, &mapname))
    {
        id = mapname.task->id;
    }
    rwlock_unlock(plock);

    return id;
}
static inline int32_t _iter_allid(void *pitem, void *pudata)
{
    struct message_ctx msg;
    msg.id = ((struct map_task_id *)pitem)->task->id;
    queue_expand(pudata);
    queue_push(pudata, &msg);
    return ERR_OK;
}
void srey_allid(struct srey_ctx *pctx, struct queue_ctx *pqu)
{
    struct rwlock_ctx *plock = _map_rwlock(pctx);
    rwlock_rdlock(plock);
    _map_iter(pctx->map_id, _iter_allid, pqu);
    rwlock_unlock(plock);
}
static inline int32_t _srey_task_msg_push(struct srey_ctx *pctx, struct task_ctx *ptotask, 
    sid_t srcid, uint32_t uisess, uint32_t uitype, void *pmsg, uint32_t uisz)
{
    struct message_ctx msg;
    msg.flags = uitype;
    msg.session = uisess;
    msg.size = uisz;
    msg.id = srcid;
    msg.data = pmsg;
    int32_t iadd = 0;
    mutex_lock(&ptotask->mu_qu);
    if (0 != ptotask->unreg)
    {
        mutex_unlock(&ptotask->mu_qu);
        return ERR_FAILED;
    }
    if (MSG_TYPE_UNREG == uitype)
    {
        ptotask->unreg = 1;
    }
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

    return ERR_OK;
}
void srey_unregister(struct srey_ctx *pctx, sid_t id)
{
    struct task_ctx *ptask = NULL;
    struct map_task_id mapid;
    mapid.id = id;
    struct rwlock_ctx *plock = _map_rwlock(pctx);
    rwlock_wrlock(plock);
    if (ERR_OK == _map_remove(pctx->map_id, &mapid, &mapid))
    {
        ptask = mapid.task;
        ATOMIC_ADD(&ptask->ref, 1);
        struct map_task_name mapname;
        FILL_NAME(mapname.name, ptask->module.name);        
        ASSERTAB(ERR_OK == _map_remove(pctx->map_name, &mapname, NULL), "logic error.");
    }
    rwlock_unlock(plock);

    if (NULL != ptask)
    {
        if (ERR_OK != _srey_task_msg_push(pctx, ptask, 0, 0, MSG_TYPE_UNREG, NULL, 0))
        {
            LOG_WARN("task %s already unregister.", ptask->module.name);
        }
        ATOMIC_ADD(&ptask->ref, -1);
    }
}
uint32_t task_new_session(struct task_ctx *ptask)
{
    if (UINT_MAX == ptask->session)
    {
        ptask->session = 1;
    }
    return ptask->session++;
}
sid_t task_id(struct task_ctx *ptask)
{
    return ptask->id;
}
const char *task_name(struct task_ctx *ptask)
{
    return ptask->module.name;
}
static inline int32_t _srey_task_msg_trypush(struct srey_ctx *pctx, sid_t toid,
    sid_t srcid, uint32_t uisess, uint32_t uitype, void *pmsg, uint32_t uisz)
{
    int32_t irtn = ERR_FAILED;
    struct task_ctx *ptask = NULL;
    struct map_task_id mapid;
    mapid.id = toid;
    struct rwlock_ctx *plock = _map_rwlock(pctx);
    rwlock_rdlock(plock);
    if (ERR_OK == _map_get(pctx->map_id, &mapid, &mapid))
    {
        ptask = mapid.task;
        ATOMIC_ADD(&ptask->ref, 1);
    }
    rwlock_unlock(plock);
    if (NULL != ptask)
    {
        irtn = _srey_task_msg_push(pctx, ptask, srcid, uisess, uitype, pmsg, uisz);
        ATOMIC_ADD(&ptask->ref, -1);
    }
    return irtn;
}
int32_t srey_call(struct srey_ctx *pctx, sid_t toid, void *pmsg, uint32_t uisz)
{
    int32_t irtn = _srey_task_msg_trypush(pctx, toid, 0, 0, MSG_TYPE_CALL, pmsg, uisz);
    if (ERR_OK != irtn)
    {
        _task_msg_free(pctx, pmsg);
    }
    return irtn;
}
int32_t srey_request(struct srey_ctx *pctx, sid_t toid, 
    sid_t srcid, uint32_t uisess, void *pmsg, uint32_t uisz)
{
    int32_t irtn = _srey_task_msg_trypush(pctx, toid, srcid, uisess, MSG_TYPE_REQUEST, pmsg, uisz);
    if (ERR_OK != irtn)
    {
        _task_msg_free(pctx, pmsg);
    }
    return irtn;
}
int32_t srey_response(struct srey_ctx *pctx, sid_t toid, uint32_t uisess, void *pmsg, uint32_t uisz)
{
    int32_t irtn = _srey_task_msg_trypush(pctx, toid, 0, uisess, MSG_TYPE_RESPONSE, pmsg, uisz);
    if (ERR_OK != irtn)
    {
        _task_msg_free(pctx, pmsg);
    }
    return irtn;
}
void srey_broadcast(struct srey_ctx *pctx, void *pmsg, uint32_t uisz)
{
    struct msg_broadcast *pbc = MALLOC(sizeof(struct msg_broadcast));
    if (NULL == pbc)
    {
        LOG_ERROR("%s", ERRSTR_MEMORY);
        return;
    }
    pbc->msg = pmsg;
    pbc->size = uisz;
    pbc->srey = pctx;
    struct queue_ctx quid;
    queue_init(&quid, 128);
    srey_allid(pctx, &quid);
    int32_t icnt = queue_size(&quid);
    pbc->ref = (atomic_t)icnt;
    struct message_ctx msgid;
    while (ERR_OK == queue_pop(&quid, &msgid))
    {
        if (ERR_OK != _srey_task_msg_trypush(pbc->srey, msgid.id, 0, 0, MSG_TYPE_BROADCAST, pbc, 0))
        {
            ATOMIC_ADD(&pbc->ref, -1);
        }
    }
    queue_free(&quid);
    if (0 == icnt)
    {
        _task_msg_free(pctx, pbc->msg);
        FREE(pbc);
    }
}
static inline void _srey_timeout(struct ud_ctx *pud)
{
    (void)_srey_task_msg_trypush((struct srey_ctx *)pud->handle, pud->id, 
        0, pud->session, MSG_TYPE_TIMEOUT, NULL, 0);
}
void srey_timeout(struct srey_ctx *pctx, sid_t ownerid, uint32_t uisess, const uint32_t uitimeout)
{
    struct ud_ctx ud;
    ud.handle = (uintptr_t)pctx;
    ud.id = ownerid;
    ud.session = uisess;
    tw_add(&pctx->tw, uitimeout, _srey_timeout, &ud);
}
static inline void _srey_sock_accept(struct sock_ctx *psock, struct ud_ctx *pud)
{
    if (ERR_OK != _srey_task_msg_trypush((struct srey_ctx *)pud->handle, pud->id, 
        0, 0, MSG_TYPE_ACCEPT, psock, 0))
    {
        sock_free(psock);
    }
}
struct listener_ctx *srey_listener(struct srey_ctx *pctx, sid_t ownerid, const char *phost, const uint16_t usport)
{
    struct ud_ctx ud;
    ud.id = ownerid;
    ud.handle = (uintptr_t)pctx;
    return netev_listener(pctx->netev, phost, usport, _srey_sock_accept, &ud);
}
static inline void _srey_sock_connect(struct sock_ctx *psock, int32_t ierr, struct ud_ctx *pud)
{
    if (ERR_OK != _srey_task_msg_trypush((struct srey_ctx *)pud->handle, pud->id, 
        0, pud->session, MSG_TYPE_CONNECT, psock, (ERR_OK == ierr ? ERR_OK : 1)))
    {
        sock_free(psock);
    }
}
struct sock_ctx *srey_connecter(struct srey_ctx *pctx, sid_t ownerid, uint32_t uisess,
    uint32_t utimeout, const char *phost, const uint16_t usport)
{
    struct ud_ctx ud;
    ud.id = ownerid;
    ud.session = uisess;
    ud.handle = (uintptr_t)pctx;
    return netev_connecter(pctx->netev, utimeout, phost, usport, _srey_sock_connect, &ud);
}
struct sock_ctx *srey_addsock(struct srey_ctx *pctx, SOCKET sock, int32_t itype, int32_t ifamily)
{
    return netev_add_sock(pctx->netev, sock, itype, ifamily);
}
static inline void _srey_sock_recv(struct sock_ctx *psock, size_t uisize, union netaddr_ctx *paddr, struct ud_ctx *pud)
{
    if (SOCK_STREAM == sock_type(psock))
    {
        if (ERR_OK != _srey_task_msg_trypush((struct srey_ctx *)pud->handle, pud->id,
            0, 0, MSG_TYPE_RECV, psock, (uint32_t)uisize))
        {
            sock_close(psock);
        }
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
        if (ERR_OK != _srey_task_msg_trypush((struct srey_ctx *)pud->handle, pud->id, 
            0, 0, MSG_TYPE_RECVFROM, pmsg, (uint32_t)uisize))
        {
            FREE(pmsg);
            sock_close(psock);
        }
    }
}
static inline void _srey_sock_send(struct sock_ctx *psock, size_t uisize, struct ud_ctx *pud)
{
    if (ERR_OK != _srey_task_msg_trypush((struct srey_ctx *)pud->handle, pud->id, 
        0, 0, MSG_TYPE_SEND, psock, (uint32_t)uisize))
    {
        sock_close(psock);
    }
}
static inline void _srey_sock_close(struct sock_ctx *psock, struct ud_ctx *pud)
{
    if (ERR_OK != _srey_task_msg_trypush((struct srey_ctx *)pud->handle, pud->id,
        0, 0, MSG_TYPE_CLOSE, psock, 0))
    {
        sock_free(psock);
    }
}
int32_t srey_enable(struct srey_ctx *pctx, sid_t ownerid, struct sock_ctx *psock, int32_t iwrite)
{
    struct ud_ctx ud;
    ud.id = ownerid;
    ud.handle = (uintptr_t)pctx;
    send_cb scb = NULL;
    if (0 != iwrite)
    {
        scb = _srey_sock_send;
    }
    return netev_enable_rw(pctx->netev, psock, _srey_sock_recv, scb, _srey_sock_close, &ud);
}
