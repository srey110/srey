#include "protocol/kcp/kcp.h"
#include "containers/hashmap.h"
#include "utils/timer.h"
#if KCP_TICK_HEAP
#include "containers/heap.h"
#endif

#define KCP_MIN_OVERHEAD 24
// 须与 ikcp.c 的 IKCP_MTU_DEF / IKCP_WND_RCV 一致(ikcp.h 未导出这两个常量)
#define KCP_MTU_DEF 1400
#define KCP_WND_RCV 128

typedef struct kcp_element {
#if KCP_TICK_HEAP
    heap_node hnode;     // 按 next_update 排序的最小堆节点,必须在首位,供 UPCAST 使用
#endif
    uint8_t warned;
    uint32_t conv;
    IUINT32 next_update; // 下次应调用 ikcp_update 的时刻(ms),由 ikcp_check 算出;<=now 才真正 update
    struct watcher_ctx *watcher; // 所属 event 线程,_kcp_start 时赋值,_kcp_output 用于取 skctx
    ikcpcb *ikcp;
    name_t handle;
    uint64_t sess;
    sk_id sk;
    netaddr_ctx addr;
}kcp_element;
#if KCP_TICK_HEAP
// 从堆节点指针还原 kcp_element 指针
#define _KEL_FROM_HNODE(n) UPCAST(n, kcp_element, hnode)
#else
typedef struct kcp_tick_arg {
    IUINT32  now;   // 本轮驱动时刻
    uint32_t next;  // 所有会话中距下次 ikcp_check 最近的间隔(ms)
}kcp_tick_arg;
#endif
// kcp_send 发送缓冲：copy=1 时 payload 内联在同一块分配里(1 次 MALLOC，data 指回 payload)；
// copy=0 时 data 指向调用方转移所有权的外部缓冲，payload[] 不使用
typedef struct kcp_send_buf {
    uint32_t conv;
    size_t lens;
    void *data;
    char payload[];
} kcp_send_buf;
// kcp_handle 命令携带的 conv + sess，用于 _kcp_handle 校验会话是否已被复用（同 _kcp_stop）
typedef struct kcp_handle_arg {
    uint32_t conv;
    uint64_t sess;
} kcp_handle_arg;
typedef struct kcp_ud_ctx {
    struct watcher_ctx *watcher; // 所属 event 线程(注销 tick 用)
    struct hashmap *mapkcp;      // conv -> kcp_element 会话表
#if KCP_TICK_HEAP
    heap_ctx heap_due;           // 按 next_update 排序的最小堆(节点为 kcp_element.hnode),tick 早退用
#endif
    ev_tick tick;                // 注册到 watcher->ticks 的周期驱动节点
}kcp_ud_ctx;

static prot_emit *g_emit;

static uint64_t _kcp_map_hash(const void *item, uint64_t seed0, uint64_t seed1) {
    (void)seed0;
    (void)seed1;
    return hash_u64((uint64_t)(*(const kcp_element **)item)->conv);
}
static int _kcp_map_compare(const void *a, const void *b, void *ud) {
    (void)ud;
    uint32_t kela = (*(const kcp_element **)a)->conv;
    uint32_t kelb = (*(const kcp_element **)b)->conv;
    return (kela < kelb) ? -1 : (kela > kelb) ? 1 : 0; // 三路比较，避免 UINT_PTR 相减截断为 int 溢出
}
#if KCP_TICK_HEAP
// 最小堆比较函数：next_update 小的优先(堆顶是最早到期的);有符号减法防 32 位时间戳回绕
static int _kcp_due_cmp(const heap_node *lhs, const heap_node *rhs) {
    return (IINT32)(_KEL_FROM_HNODE(lhs)->next_update - _KEL_FROM_HNODE(rhs)->next_update) < 0;
}
#endif
static kcp_element *_kcp_map_get(kcp_ud_ctx *ctx, uint32_t conv) {
    kcp_element key;
    key.conv = conv;
    kcp_element *pkey = &key;
    void **tmp = (void **)hashmap_get(ctx->mapkcp, &pkey);
    return NULL == tmp ? NULL : *tmp;
}
static void _kcp_map_add(kcp_ud_ctx *ctx, kcp_element *kel) {
    ASSERTAB(NULL == hashmap_set(ctx->mapkcp, &kel), "kcp conv repeat.");
    ASSERTAB(!hashmap_oom(ctx->mapkcp), "hashmap oom.");
#if KCP_TICK_HEAP
    heap_insert(&ctx->heap_due, &kel->hnode);
#endif
}
static void _kcp_map_remove(kcp_ud_ctx *ctx, kcp_element *kel) {
    kcp_element *pkey = kel;
    hashmap_delete(ctx->mapkcp, &pkey);
#if KCP_TICK_HEAP
    heap_remove(&ctx->heap_due, &kel->hnode);
#endif
}
void _kcp_init(prot_emit *emit) {
    g_emit = emit;
}
// 向 kel->handle 所属 task 发一条 MSG_TYPE_CLOSE,排干所有挂在 kel->sess 上的等待协程(synsend)
static void _kcp_notify_closed(ud_cxt *ud, kcp_element *kel) {
    void *target = g_emit->begin(ud->loader, kel->handle);
    if (NULL == target) {
        return;
    }
    message_ctx msg = { 0 };
    msg.mtype = MSG_TYPE_CLOSE;
    msg.subtype = PACK_UDP_KCP;
    msg.sk = kel->sk;
    msg.sess = kel->sess;
    g_emit->emit(target, &msg);
    g_emit->end(target);
}
// 向 kel->handle 所属 task 发一条 MSG_TYPE_HANDSHAKED,erro 携带 kcp_start 在 event 线程上的实际结果(ERR_OK/ERR_FAILED)
static void _kcp_notify_handshaked(ud_cxt *ud, kcp_element *kel, int32_t erro) {
    void *target = g_emit->begin(ud->loader, kel->handle);
    if (NULL == target) {
        return;
    }
    message_ctx msg = { 0 };
    msg.mtype = MSG_TYPE_HANDSHAKED;
    msg.subtype = PACK_UDP_KCP;
    msg.sk = kel->sk;
    msg.sess = kel->sess;
    msg.erro = erro;
    g_emit->emit(target, &msg);
    g_emit->end(target);
}
static bool _kcp_notify_closed_iter(const void *item, void *udata) {
    kcp_element *kel = *(kcp_element *const *)item;
    _kcp_notify_closed((ud_cxt *)udata, kel);
    return true;
}
void _kcp_udfree(ud_cxt *ud) {
    if (NULL == ud->context) {
        return;
    }
    kcp_ud_ctx *ctx = ud->context;
    _evpub_tick_remove(ctx->watcher, &ctx->tick);
    hashmap_scan(ctx->mapkcp, _kcp_notify_closed_iter, ud);
    hashmap_free(ctx->mapkcp);
    FREE(ctx);
    ud->context = NULL;
}
void _kcp_unpack(SOCKET fd, uint64_t skid,
                 char *buf, size_t size, netaddr_ctx *addr, ud_cxt *ud) {
    if (size < KCP_MIN_OVERHEAD
        || NULL == ud->context) {
        return;
    }
    kcp_ud_ctx *ctx = ud->context;
    uint32_t conv = ikcp_getconv(buf);
    kcp_element *kel = _kcp_map_get(ctx, conv);
    if (NULL == kel) {
        LOG_WARN("kcp get conv %u error.", conv);
        return;
    }
    (void)addr;
    //地址验证，暂时未做
    ikcp_input(kel->ikcp, buf, (long)size);
    int32_t peek = ikcp_peeksize(kel->ikcp);
    if (peek <= 0) {
        return;
    }
    void *target = g_emit->begin(ud->loader, kel->handle);
    if (NULL == target) {
        LOG_WARN("kcp get target %"PRIu64" error.", kel->handle);
        return;
    }
    message_ctx msg = { 0 };
    msg.mtype = MSG_TYPE_RECVFROM;
    msg.subtype = ud->pktype;
    msg.sk.fd = fd;
    msg.sk.skid = skid;
    int32_t rtn;
    recvfrom_ctx *umsg;
    for (;;) {
        MALLOC(umsg, sizeof(recvfrom_ctx) + peek);
        rtn = ikcp_recv(kel->ikcp, umsg->data, peek);
        if (rtn < 0) {
            FREE(umsg);
            break;
        }
        umsg->len = (size_t)rtn;
        umsg->addr = kel->addr;
        msg.data = umsg;
        msg.size = umsg->len;
        msg.sess = kel->sess;
        g_emit->emit(target, &msg);
        peek = ikcp_peeksize(kel->ikcp);
        if (peek <= 0) {
            break;
        }
    }
    g_emit->end(target);
}
static int _kcp_output(const char *buf, int len, ikcpcb *ikcp, void *user) {
    (void)ikcp;
    if (len <= 0) {
        LOG_WARN("kcp output invalid len %d.", len);
        return ERR_FAILED;
    }
    kcp_element *kel = user;
    struct watcher_ctx *watcher = kel->watcher;
    struct sock_ctx *skctx = _evpub_sockel_get(watcher, kel->sk.fd);
    if (NULL == skctx
        || ERR_OK != _evpub_checkid(skctx, kel->sk.skid)) {
        if (!kel->warned) {
            LOG_WARN("maybe forgot stop kcp.");
            kel->warned = 1;
        }
        return ERR_FAILED;
    }
    // 非iocp且发送队列为空，尝试直接发送。
    if (ERR_OK == _evpub_try_sendto(watcher, skctx, buf, (size_t)len, &kel->addr)) {
        return ERR_OK;
    }
    sendto_ctx sbuf;
    MALLOC(sbuf.data, len);
    memcpy(sbuf.data, buf, len);
    sbuf.addr = kel->addr;
    sbuf.len = len;
    _evpub_add_bufs_sendto(watcher, skctx, &sbuf, 1);
    return ERR_OK;
}
// 单次 kcp_send 消息上限 = 最大分片数 × mss;mtu<=0 按默认
static size_t _kcp_maxpack(int32_t mtu) {
    if (mtu <= 0) {
        mtu = KCP_MTU_DEF;
    }
    return (size_t)(KCP_WND_RCV - 1) * (size_t)(mtu - KCP_MIN_OVERHEAD);
}
void kcp_init(kcp_ctx *kcp, ev_ctx *netev, SOCKET fd, uint64_t skid, uint32_t conv, uint64_t sess) {
    kcp->stopped = 0;
    kcp->conv = conv;
    kcp->sess = sess;
    kcp->netev = netev;
    kcp->maxpack = _kcp_maxpack(0);
    kcp->sk.fd = fd;
    kcp->sk.skid = skid;
}
static void _kcp_element_free(void *arg) {
    if (NULL == arg) {
        return;
    }
    kcp_element *kel = arg;
    if (NULL != kel->ikcp) {
        ikcp_release(kel->ikcp);
        kel->ikcp = NULL;
    }
    FREE(kel);
}
// hashmap elfree 回调:遍历传入的是指向存储槽的指针(kcp_element **),须解引用取真正的 kel 再释放
static void _kcp_map_elfree(void *item) {
    _kcp_element_free(*(kcp_element **)item);
}
#if KCP_TICK_HEAP
// ev_tick 回调:堆顶到期(<=now)才 update,更新后按新 next_update 重新入堆;
// 堆为空或堆顶未到期直接返回,避免每轮对全部会话做 hashmap_scan
static uint32_t _kcp_tick_update(kcp_ud_ctx *ctx, uint64_t now_ms) {
    IUINT32 now = (IUINT32)now_ms;
    uint32_t remain = ctx->heap_due.nelts;// 至多处理本轮已有会话数,防 ikcp_check 异常返回<=now 时死循环
    kcp_element *kel;
    while (remain-- > 0 && NULL != ctx->heap_due.root) {
        kel = _KEL_FROM_HNODE(ctx->heap_due.root);
        if ((IINT32)(now - kel->next_update) < 0) {// 堆顶未到期,防回绕
            break;
        }
        heap_remove(&ctx->heap_due, &kel->hnode);
        ikcp_update(kel->ikcp, now);
        kel->next_update = ikcp_check(kel->ikcp, now);
        heap_insert(&ctx->heap_due, &kel->hnode);
    }
    if (NULL == ctx->heap_due.root) {
        return EVENT_WAIT_TIMEOUT;
    }
    IINT32 diff = (IINT32)(_KEL_FROM_HNODE(ctx->heap_due.root)->next_update - now);
    return (diff > 0) ? (uint32_t)diff : 0;
}
#else
static bool _kcp_tick_iter(const void *item, void *udata) {
    kcp_element *kel = *(kcp_element *const *)item;
    kcp_tick_arg *a = udata;
    if ((IINT32)(a->now - kel->next_update) >= 0) {// 到期,防回绕
        ikcp_update(kel->ikcp, a->now);
        kel->next_update = ikcp_check(kel->ikcp, a->now);
    }
    uint32_t d = (kel->next_update > a->now) ? (uint32_t)(kel->next_update - a->now) : 0;
    if (d < a->next) {
        a->next = d;
    }
    return true;
}
// ev_tick 回调:驱动本 socket 所有会话 ikcp_update,返回距下次最近的 ikcp_check 间隔(ms)
static uint32_t _kcp_tick_update(kcp_ud_ctx *ctx, uint64_t now_ms) {
    kcp_tick_arg a = { (IUINT32)now_ms, EVENT_WAIT_TIMEOUT };
    hashmap_scan(ctx->mapkcp, _kcp_tick_iter, &a);
    return a.next;
}
#endif
static uint32_t _kcp_tick(void *ud, uint64_t now_ms) {
    kcp_ud_ctx *ctx = ud;
    return _kcp_tick_update(ctx, now_ms);
}
static kcp_element *kcp_element_init(kcp_ctx *kcp, name_t handle, const char *ip, uint16_t port, const kcp_config *cfg) {
    kcp_element *kel;
    CALLOC(kel, 1, sizeof(kcp_element));
    kel->ikcp = ikcp_create(kcp->conv, kel);
    if (NULL == kel->ikcp) {
        _kcp_element_free(kel);
        return NULL;
    }
    ikcp_setoutput(kel->ikcp, _kcp_output);
    if (NULL != cfg) {
        if (cfg->mtu > 0 && 0 != ikcp_setmtu(kel->ikcp, cfg->mtu)) {
            LOG_WARN("kcp invalid mtu %d, keep %u.", cfg->mtu, kel->ikcp->mtu);
        }
        if (cfg->sndwnd > 0 || cfg->rcvwnd > 0) {
            ikcp_wndsize(kel->ikcp, cfg->sndwnd, cfg->rcvwnd);
        }
        ikcp_nodelay(kel->ikcp, cfg->nodelay, cfg->interval, cfg->resend, cfg->nc);
    }
    if (ERR_OK != netaddr_set(&kel->addr, ip, port)) {
        _kcp_element_free(kel);
        return NULL;
    }
    kel->conv = kcp->conv;
    kel->sess = kcp->sess;
    kel->handle = handle;
    kel->sk.fd = kcp->sk.fd;
    kel->sk.skid = kcp->sk.skid;
    return kel;
}
static int32_t _kcp_start(struct watcher_ctx *watcher, struct sock_ctx *skctx,
    void *data, uint64_t number) {
    (void)number;
    ud_cxt *ud = _evpub_get_ud(skctx);
    kcp_element *kel = data;
    if (SOCK_DGRAM != _evpub_sock_type(skctx) || PACK_UDP_KCP != ud->pktype) {
        LOG_ERROR("kcp_start called on non-UDP_KCP fd %d, drop.", (int32_t)kel->sk.fd);
        _kcp_notify_handshaked(ud, kel, ERR_FAILED);
        return 1;
    }
    kel->watcher = watcher;
    kel->next_update = (IUINT32)timer_cur_ms(_evpub_watcher_timer(watcher));
    kcp_ud_ctx *ctx = ud->context;
    if (NULL == ctx) {
        MALLOC(ctx, sizeof(kcp_ud_ctx));
        ctx->mapkcp = hashmap_new_with_allocator(_malloc, _realloc, _free,
                                                 sizeof(kcp_element *), ONEK, 0, 0,
                                                 _kcp_map_hash, _kcp_map_compare, _kcp_map_elfree, NULL);
#if KCP_TICK_HEAP
        heap_init(&ctx->heap_due, _kcp_due_cmp);
#endif
        ctx->watcher = watcher;
        ctx->tick.cb = _kcp_tick;
        ctx->tick.ud = ctx;
        _evpub_tick_add(watcher, &ctx->tick);
        ud->context = ctx;
    } else if (NULL != _kcp_map_get(ctx, kel->conv)) {
        LOG_WARN("kcp conv %u repeat, ignore.", kel->conv);
        _kcp_notify_handshaked(ud, kel, ERR_FAILED);
        return 1;
    }
    _kcp_map_add(ctx, kel);
    _kcp_notify_handshaked(ud, kel, ERR_OK);
    return 0;
}
int32_t kcp_start(kcp_ctx *kcp, name_t handle, const char *ip, uint16_t port, const kcp_config *cfg) {
    kcp_element *kel = kcp_element_init(kcp, handle, ip, port, cfg);
    if (NULL == kel) {
        return ERR_FAILED;
    }
    kcp->stopped = 0;
    kcp->maxpack = _kcp_maxpack((int32_t)kel->ikcp->mtu);
    return ev_props(kcp->netev, kcp->sk.fd, kcp->sk.skid, _kcp_start, _kcp_element_free, kel, 0);
}
static kcp_element *_kcp_resolve(ud_cxt *ud, uint32_t conv, uint64_t sess) {
    if (NULL == ud->context) {
        LOG_WARN("kcp context not init.");
        return NULL;
    }
    kcp_element *kel = _kcp_map_get((kcp_ud_ctx *)ud->context, conv);
    if (NULL == kel || kel->sess != sess) {
        LOG_WARN("can't find conv %u.", conv);
        return NULL;
    }
    return kel;
}
static int32_t _kcp_stop(struct watcher_ctx *watcher, struct sock_ctx *skctx,
    void *data, uint64_t number) {
    (void)watcher;
    ud_cxt *ud = _evpub_get_ud(skctx);
    kcp_element *kel = _kcp_resolve(ud, (uint32_t)(uintptr_t)data, number);
    if (NULL == kel) {
        return 0;
    }
    _kcp_notify_closed(ud, kel);
    _kcp_map_remove((kcp_ud_ctx *)ud->context, kel);
    _kcp_element_free(kel);
    return 0;
}
void kcp_stop(kcp_ctx *kcp) {
    if (kcp->stopped) {
        return;
    }
    kcp->stopped = 1;
    (void)ev_props(kcp->netev, kcp->sk.fd, kcp->sk.skid, _kcp_stop, NULL, (void *)(uintptr_t)kcp->conv, kcp->sess);
}
// number 校验 sess，防止 conv 被 stop 后以新 sess 重建期间，持旧 kcp_ctx 副本的 stale 调用改到新会话的推送目标（同 _kcp_stop）
static int32_t _kcp_handle(struct watcher_ctx *watcher, struct sock_ctx *skctx,
    void *data, uint64_t number) {
    (void)watcher;
    ud_cxt *ud = _evpub_get_ud(skctx);
    kcp_handle_arg *arg = data;
    kcp_element *kel = _kcp_resolve(ud, arg->conv, arg->sess);
    if (NULL != kel) {
        kel->handle = (name_t)number;
    }
    return 1;
}
int32_t kcp_handle(kcp_ctx *kcp, name_t handle) {
    if (kcp->stopped) {
        return ERR_FAILED;
    }
    kcp_handle_arg *arg;
    MALLOC(arg, sizeof(kcp_handle_arg));
    arg->conv = kcp->conv;
    arg->sess = kcp->sess;
    return ev_props(kcp->netev, kcp->sk.fd, kcp->sk.skid, _kcp_handle, _free, arg, handle);
}
static void _kcp_send_free(void *arg) {
    if (NULL == arg) {
        return;
    }
    kcp_send_buf *buf = arg;
    if (buf->data != buf->payload) {// data 指向外部缓冲(copy=0)才需要单独释放；指回 payload(copy=1)随 buf 一起释放
        FREE(buf->data);
    }
    FREE(buf);
}
// number 校验 sess，防止 conv 被 stop 后以新 sess 重建期间，持旧 kcp_ctx 副本的 stale 调用把数据注入新会话（同 _kcp_stop）
static int32_t _kcp_send(struct watcher_ctx *watcher, struct sock_ctx *skctx,
    void *data, uint64_t number) {
    (void)watcher;
    ud_cxt *ud = _evpub_get_ud(skctx);
    kcp_send_buf *buf = data;
    kcp_element *kel = _kcp_resolve(ud, buf->conv, number);
    if (NULL != kel) {
        ikcp_send(kel->ikcp, buf->data, (int32_t)buf->lens);
    }
    return 1;
}
int32_t kcp_send(kcp_ctx *kcp, void *data, size_t lens, int32_t copy) {
    if (kcp->stopped) {
        if (!copy) {
            FREE(data);
        }
        return ERR_FAILED;
    }
    if (lens > kcp->maxpack) {
        LOG_WARN("kcp send lens %zu exceeds max pack %zu.", lens, kcp->maxpack);
        if (!copy) {
            FREE(data);
        }
        return ERR_FAILED;
    }
    kcp_send_buf *buf;//todo 可以用池
    if (copy) {
        MALLOC(buf, sizeof(kcp_send_buf) + lens);
        memcpy(buf->payload, data, lens);
        buf->data = buf->payload;
    } else {
        MALLOC(buf, sizeof(kcp_send_buf));
        buf->data = data;
    }
    buf->conv = kcp->conv;
    buf->lens = lens;
    return ev_props(kcp->netev, kcp->sk.fd, kcp->sk.skid, _kcp_send, _kcp_send_free, buf, kcp->sess);
}
