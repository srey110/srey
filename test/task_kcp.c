#include "task_kcp.h"

#define KCP_N_CLIENTS 3 // client 数量
#define KCP_SV_MAXSESS 8 // server 侧会话表容量

// ================= server =================
typedef struct kcp_sv_sess {
    uint16_t cport;     // client UDP 端口(会话 key,echo 时按来源端口匹配)
    kcp_ctx  kcp;       // server 侧该 client 的 kcp 会话句柄
}kcp_sv_sess;

static uint16_t _sv_tcp_port;
static uint16_t _sv_udp_port;
static SOCKET _sv_udp_fd;
static uint64_t _sv_udp_skid;
static name_t _sv_handle;
static uint32_t _sv_conv_seq;
static int32_t _sv_nsess;
static kcp_sv_sess _sv_sess[KCP_SV_MAXSESS];

// TCP 握手:收 client 上报的 UDP 端口 -> 分配唯一 conv -> server 侧 kcp_start -> 回 conv
static void _sv_net_recv(task_ctx *task, sk_id *sk, subtype_t pktype, uint8_t client, uint8_t slice, void *data, size_t size) {
    (void)pktype;
    (void)client;
    (void)slice;
    if (size < sizeof(uint16_t) || _sv_nsess >= KCP_SV_MAXSESS) {
        return;
    }
    uint16_t cport;
    memcpy(&cport, data, sizeof(cport));
    uint32_t conv = ++_sv_conv_seq;
    kcp_sv_sess *s = &_sv_sess[_sv_nsess];
    s->cport = cport;
    kcp_init(&s->kcp, &task->loader->netev, _sv_udp_fd, _sv_udp_skid, conv);
    if (ERR_OK != kcp_start(&s->kcp, _sv_handle, createid(), "127.0.0.1", cport, NULL)) {
        LOG_ERROR("kcp server kcp_start conv %u error.", conv);
        return;
    }
    _sv_nsess++;
    void *out;
    MALLOC(out, sizeof(conv));
    memcpy(out, &conv, sizeof(conv));
    ev_send(&task->loader->netev, sk->fd, sk->skid, out, sizeof(conv), 0);
}
// UDP 侧 kcp 数据:按来源端口找到会话,原样 echo 回该 client
static void _sv_net_recvfrom(task_ctx *task, sk_id *sk, subtype_t pktype, char *ip, uint16_t port, void *data, size_t size) {
    (void)task;
    (void)sk;
    (void)pktype;
    (void)ip;
    int32_t i;
    for (i = 0; i < _sv_nsess; i++) {
        if (_sv_sess[i].cport == port) {
            kcp_send(&_sv_sess[i].kcp, data, size, 1);
            return;
        }
    }
    LOG_WARN("kcp server: no session for client port %u.", port);
}
static void _sv_startup(task_ctx *task) {
    _sv_handle = task->handle;
    _sv_conv_seq = 0;
    _sv_nsess = 0;
    task_recved(task, _sv_net_recv);
    task_recvedfrom(task, _sv_net_recvfrom);
    uint64_t lid;
    if (ERR_OK != task_listen(task, PACK_NONE, NULL, "0.0.0.0", _sv_tcp_port, &lid, 0)) {
        LOG_ERROR("kcp server tcp listen %u error.", _sv_tcp_port);
        return;
    }
    if (ERR_OK != task_udp(task, PACK_UDP_KCP, "0.0.0.0", _sv_udp_port, &_sv_udp_fd, &_sv_udp_skid)) {
        LOG_ERROR("kcp server udp %u error.", _sv_udp_port);
        return;
    }
}
void task_kcp_server_start(loader_ctx *loader, const char *name, uint16_t tcp_port, uint16_t udp_port) {
    _sv_tcp_port = tcp_port;
    _sv_udp_port = udp_port;
    coro_task_register(loader, name, 0, _sv_startup, NULL, NULL, NULL);
}

// ================= client =================
static uint16_t _cli_sv_tcp;
static uint16_t _cli_sv_udp;
static int32_t *_cli_ok;
static atomic_t _cli_success;

static void _cli_worker(task_ctx *task, void *arg) {
    int32_t idx = (int32_t)(intptr_t)arg;
    // 1. 建 UDP socket(OS 分配端口)
    SOCKET ufd;
    uint64_t uskid;
    if (ERR_OK != task_udp(task, PACK_UDP_KCP, "0.0.0.0", 0, &ufd, &uskid)) {
        LOG_ERROR("kcp client %d udp error.", idx);
        return;
    }
    // 2. netaddr_local 取回 OS 分配的本地端口
    netaddr_ctx la;
    if (ERR_OK != netaddr_local(&la, ufd)) {
        LOG_ERROR("kcp client %d netaddr_local error.", idx);
        ev_close(&task->loader->netev, ufd, uskid, 1);
        return;
    }
    uint16_t uport = netaddr_port(&la);
    // 3. TCP 握手:上报 UDP 端口,拿回 conv
    SOCKET tfd;
    uint64_t tskid;
    if (ERR_OK != coro_connect(task, PACK_NONE, NULL, "127.0.0.1", _cli_sv_tcp, NETEV_NONE, NULL, &tfd, &tskid)) {
        LOG_ERROR("kcp client %d connect error.", idx);
        ev_close(&task->loader->netev, ufd, uskid, 1);
        return;
    }
    size_t rsize = 0;
    void *resp = coro_send(task, tfd, tskid, &uport, sizeof(uport), &rsize, 1);
    if (NULL == resp || rsize != sizeof(uint32_t)) {
        LOG_ERROR("kcp client %d handshake error.", idx);
        coro_close(task, tfd, tskid, 1);
        ev_close(&task->loader->netev, ufd, uskid, 1);
        return;
    }
    uint32_t conv;
    memcpy(&conv, resp, sizeof(conv));
    coro_close(task, tfd, tskid, 1);
    // 4. client 侧 kcp 会话(ikcp_update 由 event 线程 tick 自动驱动,业务无需轮询)
    kcp_ctx kcp;
    kcp_init(&kcp, &task->loader->netev, ufd, uskid, conv);
    kcp_config badmtu = { -1, -1, -1, -1, 0, 0, 24 }; // client 0 故意传非法 mtu(<50),验证 maxpack 不会因此归零导致永久发送失败
    if (ERR_OK != kcp_start(&kcp, task->handle, createid(), "127.0.0.1", _cli_sv_udp, 0 == idx ? &badmtu : NULL)) {
        LOG_ERROR("kcp client %d kcp_start error.", idx);
        ev_close(&task->loader->netev, ufd, uskid, 1);
        return;
    }
    // 5. 同步发送并校验 echo
    char msg[32];
    int32_t mlen = SNPRINTF(msg, sizeof(msg), "kcp_hello_%d", idx);
    size_t esize = 0;
    void *echo = kcp_synsend(task, &kcp, msg, (size_t)mlen, 1, &esize);
    if (NULL != echo && esize == (size_t)mlen && 0 == memcmp(echo, msg, (size_t)mlen)) {
        ATOMIC_ADD(&_cli_success, 1);
    } else {
        LOG_ERROR("kcp client %d synsend verify failed.", idx);
    }
    // 6. 收尾:停会话并关闭 UDP socket
    kcp_stop(&kcp);
    ev_close(&task->loader->netev, ufd, uskid, 1);
}
static void _cli_startup(task_ctx *task) {
    ATOMIC_SET(&_cli_success, 0);
    coro_sleep(task, 300);// 等 server TCP listen / UDP 落地
    int32_t i;
    for (i = 0; i < KCP_N_CLIENTS; i++) {
        coro_fork(task, _cli_worker, (void *)(intptr_t)i);
    }
    int32_t poll;
    for (poll = 0; poll < 100; poll++) {
        coro_sleep(task, 50);
        if (KCP_N_CLIENTS == ATOMIC_GET(&_cli_success)) {
            break;
        }
    }
    if (KCP_N_CLIENTS == ATOMIC_GET(&_cli_success)) {
        *_cli_ok = 1;
        LOG_INFO("kcp tested: %d/%d clients synsend echo ok.", KCP_N_CLIENTS, KCP_N_CLIENTS);
    } else {
        LOG_ERROR("kcp test: %d/%d only.", (int32_t)ATOMIC_GET(&_cli_success), KCP_N_CLIENTS);
    }
}
void task_kcp_client_start(loader_ctx *loader, const char *name, uint16_t sv_tcp_port, uint16_t sv_udp_port, int32_t *ok) {
    if (NULL == ok) {
        return;
    }
    _cli_sv_tcp = sv_tcp_port;
    _cli_sv_udp = sv_udp_port;
    _cli_ok = ok;
    coro_task_register(loader, name, 0, _cli_startup, NULL, NULL, NULL);
}

// ================= close 唤醒等待协程 =================
#define KCP_CLOSE_BOGUS_CONV 0xFFFFFFFF // server 从未注册的 conv,_kcp_unpack 查不到会静默丢包,永远不会 echo

static uint16_t _close_sv_udp;
static int32_t *_close_ok;

typedef struct kcp_close_ctx {
    kcp_ctx  *kcp;
    void     *echo;
    size_t   esize;
    uint64_t elapse;
}kcp_close_ctx;

// synsend 阻塞等待 echo;server 永远不会响应,期望被另一协程的 kcp_stop 用 MSG_TYPE_CLOSE 唤醒而非等满超时
static void _close_waiter(task_ctx *task, void *arg) {
    kcp_close_ctx *ctx = arg;
    const char msg[] = "kcp_close";
    uint64_t bgts = nowms();
    ctx->echo = kcp_synsend(task, ctx->kcp, (void *)msg, sizeof(msg) - 1, 1, &ctx->esize);
    ctx->elapse = nowms() - bgts;
}
// 短暂延迟后停止同一会话,验证 _kcp_stop 发出的 MSG_TYPE_CLOSE 能及时唤醒 _close_waiter
static void _close_stopper(task_ctx *task, void *arg) {
    kcp_close_ctx *ctx = arg;
    coro_sleep(task, 200);
    kcp_stop(ctx->kcp);
}
static void _close_startup(task_ctx *task) {
    coro_sleep(task, 300);// 等 server TCP listen / UDP 落地
    SOCKET ufd;
    uint64_t uskid;
    if (ERR_OK != task_udp(task, PACK_UDP_KCP, "0.0.0.0", 0, &ufd, &uskid)) {
        LOG_ERROR("kcp close test udp error.");
        return;
    }
    kcp_ctx kcp;
    kcp_init(&kcp, &task->loader->netev, ufd, uskid, KCP_CLOSE_BOGUS_CONV);
    if (ERR_OK != kcp_start(&kcp, task->handle, createid(), "127.0.0.1", _close_sv_udp, NULL)) {
        LOG_ERROR("kcp close test kcp_start error.");
        ev_close(&task->loader->netev, ufd, uskid, 1);
        return;
    }
    kcp_close_ctx ctx = { &kcp, NULL, 0, 0 };
    void (*funcs[2])(task_ctx *task, void *arg) = { _close_waiter, _close_stopper };
    void *args[2] = { &ctx, &ctx };
    int32_t rtn = coro_fork_wait(task, 2, funcs, args);
    ev_close(&task->loader->netev, ufd, uskid, 1);
    if (ERR_OK != rtn) {
        LOG_ERROR("kcp close test fork_wait error.");
        return;
    }
    if (NULL == ctx.echo && ctx.elapse < 2000) {
        *_close_ok = 1;
        LOG_INFO("kcp close tested: waiter woken by close, elapse %"PRIu64"ms.", ctx.elapse);
    } else {
        LOG_ERROR("kcp close test failed: echo %p elapse %"PRIu64"ms.", ctx.echo, ctx.elapse);
    }
}
void task_kcp_close_start(loader_ctx *loader, const char *name, uint16_t sv_udp_port, int32_t *ok) {
    if (NULL == ok) {
        return;
    }
    _close_sv_udp = sv_udp_port;
    _close_ok = ok;
    coro_task_register(loader, name, 0, _close_startup, NULL, NULL, NULL);
}

// ================= 同一 session 并发 synsend 的 FIFO 正确性 =================
#define KCP_FIFO_N 3 // 并发 synsend 协程数

static uint16_t _fifo_sv_tcp;
static uint16_t _fifo_sv_udp;
static int32_t *_fifo_ok;

typedef struct kcp_fifo_ctx {
    kcp_ctx  *kcp;
    int32_t  idx;
    int32_t  matched;
}kcp_fifo_ctx;

// 多个协程共享同一 kcp_ctx 并发 synsend,验证各自收到的 echo 精确对应自己发送的内容(不串号)
static void _fifo_worker(task_ctx *task, void *arg) {
    kcp_fifo_ctx *ctx = arg;
    char msg[32];
    int32_t mlen = SNPRINTF(msg, sizeof(msg), "kcp_fifo_%d", ctx->idx);
    size_t esize = 0;
    void *echo = kcp_synsend(task, ctx->kcp, msg, (size_t)mlen, 1, &esize);
    ctx->matched = (NULL != echo && esize == (size_t)mlen && 0 == memcmp(echo, msg, (size_t)mlen));
}
static void _fifo_startup(task_ctx *task) {
    coro_sleep(task, 300);// 等 server TCP listen / UDP 落地
    // 1. 建 UDP socket(OS 分配端口)
    SOCKET ufd;
    uint64_t uskid;
    if (ERR_OK != task_udp(task, PACK_UDP_KCP, "0.0.0.0", 0, &ufd, &uskid)) {
        LOG_ERROR("kcp fifo test udp error.");
        return;
    }
    // 2. netaddr_local 取回 OS 分配的本地端口
    netaddr_ctx la;
    if (ERR_OK != netaddr_local(&la, ufd)) {
        LOG_ERROR("kcp fifo test netaddr_local error.");
        ev_close(&task->loader->netev, ufd, uskid, 1);
        return;
    }
    uint16_t uport = netaddr_port(&la);
    // 3. TCP 握手:上报 UDP 端口,拿回 conv
    SOCKET tfd;
    uint64_t tskid;
    if (ERR_OK != coro_connect(task, PACK_NONE, NULL, "127.0.0.1", _fifo_sv_tcp, NETEV_NONE, NULL, &tfd, &tskid)) {
        LOG_ERROR("kcp fifo test connect error.");
        ev_close(&task->loader->netev, ufd, uskid, 1);
        return;
    }
    size_t rsize = 0;
    void *resp = coro_send(task, tfd, tskid, &uport, sizeof(uport), &rsize, 1);
    if (NULL == resp || rsize != sizeof(uint32_t)) {
        LOG_ERROR("kcp fifo test handshake error.");
        coro_close(task, tfd, tskid, 1);
        ev_close(&task->loader->netev, ufd, uskid, 1);
        return;
    }
    uint32_t conv;
    memcpy(&conv, resp, sizeof(conv));
    coro_close(task, tfd, tskid, 1);
    // 4. 同一 kcp 会话供 KCP_FIFO_N 个协程并发 synsend
    kcp_ctx kcp;
    kcp_init(&kcp, &task->loader->netev, ufd, uskid, conv);
    if (ERR_OK != kcp_start(&kcp, task->handle, createid(), "127.0.0.1", _fifo_sv_udp, NULL)) {
        LOG_ERROR("kcp fifo test kcp_start error.");
        ev_close(&task->loader->netev, ufd, uskid, 1);
        return;
    }
    kcp_fifo_ctx ctxs[KCP_FIFO_N];
    void (*funcs[KCP_FIFO_N])(task_ctx *task, void *arg);
    void *args[KCP_FIFO_N];
    int32_t i;
    for (i = 0; i < KCP_FIFO_N; i++) {
        ctxs[i].kcp = &kcp;
        ctxs[i].idx = i;
        ctxs[i].matched = 0;
        funcs[i] = _fifo_worker;
        args[i] = &ctxs[i];
    }
    int32_t rtn = coro_fork_wait(task, KCP_FIFO_N, funcs, args);
    kcp_stop(&kcp);
    ev_close(&task->loader->netev, ufd, uskid, 1);
    if (ERR_OK != rtn) {
        LOG_ERROR("kcp fifo test fork_wait error.");
        return;
    }
    int32_t allok = 1;
    for (i = 0; i < KCP_FIFO_N; i++) {
        if (!ctxs[i].matched) {
            allok = 0;
            LOG_ERROR("kcp fifo test: worker %d echo mismatch.", i);
        }
    }
    if (allok) {
        *_fifo_ok = 1;
        LOG_INFO("kcp fifo tested: %d/%d concurrent synsend matched.", KCP_FIFO_N, KCP_FIFO_N);
    }
}
void task_kcp_fifo_start(loader_ctx *loader, const char *name, uint16_t sv_tcp_port, uint16_t sv_udp_port, int32_t *ok) {
    if (NULL == ok) {
        return;
    }
    _fifo_sv_tcp = sv_tcp_port;
    _fifo_sv_udp = sv_udp_port;
    _fifo_ok = ok;
    coro_task_register(loader, name, 0, _fifo_startup, NULL, NULL, NULL);
}

// ================= kcp_synstart:正常建立/conv 冲突/sess==0 守卫 =================
// kcp_start 本身不经网络(仅在本地会话表登记),以下三组用例都不需要真实对端响应
static uint16_t _syn_sv_udp;
// CLOSE 用例：fork 出的子协程与主协程不同栈，关 socket 所需信息经 static 传递
static ev_ctx *_syn_dead_netev;
static SOCKET _syn_dead_fd;
static uint64_t _syn_dead_skid;
// 子协程：等主协程挂进 kcp_synsend 后关掉 socket，触发 _kcp_udfree 逐会话补 CLOSE
static void _syn_close_fork(task_ctx *task, void *arg) {
    (void)arg;
    coro_sleep(task, 50);
    ev_close(_syn_dead_netev, _syn_dead_fd, _syn_dead_skid, 1);
}
static int32_t *_syn_ok;

static void _syn_startup(task_ctx *task) {
    SOCKET ufd;
    uint64_t uskid;
    if (ERR_OK != task_udp(task, PACK_UDP_KCP, "0.0.0.0", 0, &ufd, &uskid)) {
        LOG_ERROR("kcp synstart test udp error.");
        return;
    }
    // 1. 正常建立:全新 conv,预期成功
    kcp_ctx kcp1;
    kcp_init(&kcp1, &task->loader->netev, ufd, uskid, 100);
    int32_t r1 = kcp_synstart(task, &kcp1, "127.0.0.1", _syn_sv_udp, NULL);
    // 2. conv 冲突:同一 socket 上重复注册相同 conv,预期失败
    kcp_ctx kcp2;
    kcp_init(&kcp2, &task->loader->netev, ufd, uskid, 100);
    int32_t r2 = kcp_synstart(task, &kcp2, "127.0.0.1", _syn_sv_udp, NULL);
    // 3. sess==0 守卫:以 kcp_start(sess=0) 异步建立的会话不能用 kcp_synsend,应立即失败不进 _coro_wait;
    // copy=0 顺带验证守卫分支不漏释放调用方缓冲区(leak 由 MEMORY_CHECK 收尾统计兜底)
    kcp_ctx kcp3;
    kcp_init(&kcp3, &task->loader->netev, ufd, uskid, 200);
    int32_t r3 = kcp_start(&kcp3, task->handle, 0, "127.0.0.1", _syn_sv_udp, NULL);
    void *leak;
    MALLOC(leak, 8);
    size_t esize = 0;
    uint64_t bgts = nowms();
    void *r4 = kcp_synsend(task, &kcp3, leak, 8, 0, &esize);
    uint64_t elapse3 = nowms() - bgts;
    // 4. stop 后立即 synstart 重启:每次生成新 sess,不被上一会话在途的 CLOSE 击穿
    kcp_stop(&kcp1);
    int32_t r5 = kcp_synstart(task, &kcp1, "127.0.0.1", _syn_sv_udp, NULL);
    // 5. stop 后 synstart 被拒:失败路径须置回"无会话"(sess=0 / stopped=1)并还原 maxpack。
    // 先 stop 让 conv 空出给 kcp5,kcp1 再以同 conv 重启必被 event 线程拒;若 stopped 停在 0,句柄就从
    // "已停"变回"在跑",下面的 synsend 会绕过守卫投到已消失的会话,被静默丢弃后空等满超时
    kcp_stop(&kcp1);
    kcp_ctx kcp5;
    kcp_init(&kcp5, &task->loader->netev, ufd, uskid, 100);
    int32_t r8 = kcp_synstart(task, &kcp5, "127.0.0.1", _syn_sv_udp, NULL);
    kcp_config cfg576 = { -1, -1, -1, -1, 0, 0, 576 };// mtu 与默认不同,才能验出 maxpack 被还原
    size_t mp_before = kcp1.maxpack;
    int32_t r9 = kcp_synstart(task, &kcp1, "127.0.0.1", _syn_sv_udp, &cfg576);
    int32_t r10 = (1 == kcp1.stopped && mp_before == kcp1.maxpack) ? ERR_OK : ERR_FAILED;
    uint64_t b11 = nowms();
    void *r11 = kcp_synsend(task, &kcp1, "y", 1, 1, &esize);
    uint64_t elapse11 = nowms() - b11;
    kcp_ctx kcp6;
    kcp_init(&kcp6, &task->loader->netev, ufd, uskid, 400);
    uint64_t s12a = createid();
    int32_t r12 = kcp_start(&kcp6, task->handle, s12a, "127.0.0.1", _syn_sv_udp, NULL);
    int32_t r13 = kcp_synstart(task, &kcp6, "127.0.0.1", _syn_sv_udp, NULL);
    int32_t r14 = (0 == kcp6.stopped && 0 != kcp6.sess && s12a != kcp6.sess) ? ERR_OK : ERR_FAILED;
    kcp_stop(&kcp6);
    kcp_ctx kcp7;
    kcp_init(&kcp7, &task->loader->netev, ufd, uskid, 400);
    int32_t r15 = kcp_synstart(task, &kcp7, "127.0.0.1", _syn_sv_udp, NULL);
    kcp_stop(&kcp7);
    kcp_ctx kcp8;
    kcp_init(&kcp8, &task->loader->netev, ufd, uskid, 100);
    int32_t r16 = kcp_start(&kcp8, task->handle, createid(), "127.0.0.1", _syn_sv_udp, NULL);
    kcp_stop(&kcp5);
    int32_t r17 = kcp_synstart(task, &kcp8, "127.0.0.1", _syn_sv_udp, NULL);
    kcp_stop(&kcp8);
    kcp_stop(&kcp3);
    ev_close(&task->loader->netev, ufd, uskid, 1);
    // 6. CLOSE 唤醒 kcp_synsend:挂起期间 socket 被关,_kcp_udfree 逐会话补 CLOSE;
    // 唤醒后须清 kcp->sess,否则下次 synsend 通过守卫投到已消失的会话,被 _kcp_resolve 静默丢弃后空等满超时。
    // 与 bin/script/lib/kcp.lua 的 ctx:send 同款分支互为镜像
    SOCKET dfd;
    uint64_t dskid;
    int32_t r6 = ERR_FAILED;
    int32_t r7 = ERR_FAILED;
    uint64_t elapse6 = 0;
    kcp_ctx kcp4;
    if (ERR_OK == task_udp(task, PACK_UDP_KCP, "0.0.0.0", 0, &dfd, &dskid)) {
        kcp_init(&kcp4, &task->loader->netev, dfd, dskid, 300);
        if (ERR_OK == kcp_synstart(task, &kcp4, "127.0.0.1", _syn_sv_udp, NULL)) {
            _syn_dead_netev = &task->loader->netev;
            _syn_dead_fd = dfd;
            _syn_dead_skid = dskid;
            // 收窄放在 synstart 成功之后:否则安装步骤自身可能超时,后续断言会以错误理由通过
            task_set_netread_timeout(task, 3000);
            coro_fork(task, _syn_close_fork, NULL);
            uint64_t b6 = nowms();
            // 该会话对端是 kcp server,但本次不等 echo——先被 fork 协程关 socket 触发的 CLOSE 唤醒
            void *r6p = kcp_synsend(task, &kcp4, "x", 1, 1, &esize);
            elapse6 = nowms() - b6;
            r6 = (NULL == r6p) ? ERR_OK : ERR_FAILED;
            r7 = (0 == kcp4.sess) ? ERR_OK : ERR_FAILED;
        }
    }
    if (ERR_OK == r1 && ERR_OK != r2 && ERR_OK == r3 && elapse3 < 1000 && NULL == r4 && ERR_OK == r5
        && ERR_OK == r8 && ERR_OK != r9 && ERR_OK == r10 && NULL == r11 && elapse11 < 1000
        && ERR_OK == r6 && ERR_OK == r7 && elapse6 < 2000
        && ERR_OK == r12 && ERR_OK == r13 && ERR_OK == r14 && ERR_OK == r15
        && ERR_OK == r16 && ERR_OK == r17) {
        *_syn_ok = 1;
        LOG_INFO("kcp synstart tested: normal/restart ok, collision/sess-guard rejected,"
                 " reject clears sess and restores stopped/maxpack, CLOSE clears sess,"
                 " same-handle restart implicitly stops the live session and re-registers conv,"
                 " async handle refused by the event thread stays restartable.");
    } else {
        LOG_ERROR("kcp synstart test failed: r1=%d r2=%d r3=%d elapse3=%"PRIu64" r4=%p r5=%d"
                  " r8=%d r9=%d r10=%d r11=%p elapse11=%"PRIu64" r6=%d r7=%d elapse6=%"PRIu64
                  " r12=%d r13=%d r14=%d r15=%d r16=%d r17=%d.",
                  r1, r2, r3, elapse3, r4, r5, r8, r9, r10, r11, elapse11, r6, r7, elapse6,
                  r12, r13, r14, r15, r16, r17);
    }
}
void task_kcp_synstart_start(loader_ctx *loader, const char *name, uint16_t sv_udp_port, int32_t *ok) {
    if (NULL == ok) {
        return;
    }
    _syn_sv_udp = sv_udp_port;
    _syn_ok = ok;
    coro_task_register(loader, name, 0, _syn_startup, NULL, NULL, NULL);
}
