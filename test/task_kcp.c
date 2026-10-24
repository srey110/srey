#include "task_kcp.h"
#include "task_pub.h"

#define KCP_CLOSE_BOGUS_CONV 0xFFFFFFFF // server 从未注册的 conv,_kcp_unpack 查不到会静默丢包,永远不会 echo
#define KCP_FIFO_N 3 // 并发 synsend 协程数

#define KCP_N_CLIENTS 3 // client 数量
#define KCP_SV_MAXSESS 8 // server 侧会话表容量
#define KCP_NOECHO "noecho" // 以此开头的消息 server 只收不回,验证收包唤醒停表会话后会回 ACK
#define KCP_DL2 "dl2_" // 死链用例里 conv 2 的消息前缀,对端按它挑回显用的会话
#define KCP_SEG_PUSH 81 // ikcp 的 IKCP_CMD_PUSH,手拼数据段用
// _peer_cases 收到的消息按内容记成位,位序同 _peer_msgs 的下标
enum { PEER_INJECT = 1, PEER_AFTER = 2, PEER_SINK = 4, PEER_LOST = 8, PEER_BACK = 16 };
// _peer_cases 的四个 socket:a 上建会话;b 是 a 的 kcp 对端(收到即回显);d 是 a 上 conv 3 登记的对端,
// e 是冒充者,d / e 都不建会话,手拼 PUSH 段发给 a
enum { PEER_A, PEER_B, PEER_D, PEER_E, PEER_NSK };
// _learn_cases 的三个 socket:a 上以 port 0 起学习会话;b 是真对端;e 冒充 b 用同 conv 发给 a
enum { LEARN_A, LEARN_B, LEARN_E, LEARN_NSK };

// ================= server =================
typedef struct kcp_sv_sess {
    uint16_t cport;     // client UDP 端口(会话 key,echo 时按来源端口匹配)
    kcp_ctx  kcp;       // server 侧该 client 的 kcp 会话句柄
}kcp_sv_sess;

static uint16_t _sv_tcp_port;
static uint16_t _sv_udp_port;
static sock_ctx _sv_udp_sk;
static name_t _sv_handle;
static uint32_t _sv_conv_seq;
static int32_t _sv_nsess;
static kcp_sv_sess _sv_sess[KCP_SV_MAXSESS];

// TCP 握手:收 client 上报的 UDP 端口 -> 分配唯一 conv -> server 侧 kcp_start -> 回 conv
static void _sv_net_recv(task_ctx *task, sock_ctx *sk, subtype_t pktype, uint8_t client, uint8_t slice, void *data, size_t size) {
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
    kcp_init(&s->kcp, &task->loader->netev, &_sv_udp_sk, conv);
    if (ERR_OK != kcp_start(&s->kcp, _sv_handle, createid(), "127.0.0.1", cport, NULL)) {
        LOG_ERROR("kcp server kcp_start conv %u error.", conv);
        return;
    }
    _sv_nsess++;
    void *out;
    MALLOC(out, sizeof(conv));
    memcpy(out, &conv, sizeof(conv));
    ev_send(&task->loader->netev, sk, out, sizeof(conv), 0);
}
// UDP 侧 kcp 数据:按来源端口找到会话,原样 echo 回该 client
static void _sv_net_recvfrom(task_ctx *task, sock_ctx *sk, subtype_t pktype, const netaddr_ctx *addr, void *data, size_t size) {
    (void)task;
    (void)sk;
    (void)pktype;
    int32_t i;
    if (size >= sizeof(KCP_NOECHO) - 1 && 0 == memcmp(data, KCP_NOECHO, sizeof(KCP_NOECHO) - 1)) {
        return;// 单向:client 只能靠 server 收包唤醒后的 flush 拿到 ACK
    }
    uint16_t port = netaddr_port(addr);
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
    if (ERR_OK != task_udp(task, PACK_UDP_KCP, "0.0.0.0", _sv_udp_port, 0, &_sv_udp_sk)) {
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

static void _cli_worker(void *owner, void *arg) {
    task_ctx *task = owner;
    int32_t idx = (int32_t)(intptr_t)arg;
    // 1. 建 UDP socket(OS 分配端口)
    sock_ctx usk;
    if (ERR_OK != task_udp(task, PACK_UDP_KCP, "0.0.0.0", 0, 0, &usk)) {
        LOG_ERROR("kcp client %d udp error.", idx);
        return;
    }
    // 2. netaddr_local 取回 OS 分配的本地端口
    netaddr_ctx la;
    if (ERR_OK != netaddr_local(&la, usk.fd)) {
        LOG_ERROR("kcp client %d netaddr_local error.", idx);
        ev_close(&task->loader->netev, &usk);
        return;
    }
    uint16_t uport = netaddr_port(&la);
    // 3. TCP 握手:上报 UDP 端口,拿回 conv
    sock_ctx tsk;
    if (ERR_OK != coro_connect(task, PACK_NONE, NULL, "127.0.0.1", _cli_sv_tcp, NETEV_NONE, NULL, &tsk)) {
        LOG_ERROR("kcp client %d connect error.", idx);
        ev_close(&task->loader->netev, &usk);
        return;
    }
    size_t rsize = 0;
    void *resp = coro_send(task, &tsk, &uport, sizeof(uport), &rsize, 1);
    if (NULL == resp || rsize != sizeof(uint32_t)) {
        LOG_ERROR("kcp client %d handshake error.", idx);
        coro_close(task, &tsk);
        ev_close(&task->loader->netev, &usk);
        return;
    }
    uint32_t conv;
    memcpy(&conv, resp, sizeof(conv));
    coro_close(task, &tsk);
    // 4. client 侧 kcp 会话(ikcp_update 由 event 线程 tick 自动驱动,业务无需轮询)
    kcp_ctx kcp;
    kcp_init(&kcp, &task->loader->netev, &usk, conv);
    kcp_config badmtu = { -1, -1, -1, -1, 0, 0, 24 }; // client 0 故意传非法 mtu(<50),验证 maxpack 不会因此归零导致永久发送失败
    // client 1 传的 1431655742 超过 KCP_MTU_MAX,先被 kcp.c 拒收(告警、保留默认 mtu),会话照常收发。
    // ikcp_setmtu 自身对这个值的回绕守卫由 test_protocol.c 的裸 ikcp 用例直接断言
    kcp_config hugemtu = { -1, -1, -1, -1, 0, 0, 1431655742 };
    if (ERR_OK != kcp_start(&kcp, task->handle, createid(), "127.0.0.1", _cli_sv_udp, 0 == idx ? &badmtu : (1 == idx ? &hugemtu : NULL))) {
        LOG_ERROR("kcp client %d kcp_start error.", idx);
        ev_close(&task->loader->netev, &usk);
        return;
    }
    // 5. 同步发送并校验 echo
    char msg[32];
    int32_t rtn = SNPRINTF(msg, sizeof(msg), "kcp_hello_%d", idx);
    size_t mlen = snprintf_lens(rtn, sizeof(msg));
    size_t esize = 0;
    void *echo = kcp_synsend(task, &kcp, msg, mlen, 1, &esize);
    if (NULL != echo && esize == mlen && 0 == memcmp(echo, msg, mlen)) {
        ATOMIC_ADD(&_cli_success, 1);
    } else {
        LOG_ERROR("kcp client %d synsend verify failed.", idx);
    }
    // 6. 收尾:停会话并关闭 UDP socket
    kcp_stop(&kcp);
    ev_close(&task->loader->netev, &usk);
}
static void _cli_startup_run(task_ctx *task) {
    ATOMIC_SET(&_cli_success, 0);
    coro_sleep(task, 300);// 等 server TCP listen / UDP 落地
    int32_t i;
    for (i = 0; i < KCP_N_CLIENTS; i++) {
        coro_fork(coro_task_co(task), _cli_worker, (void *)(intptr_t)i);
    }
    int32_t poll;
    for (poll = 0; poll < 100; poll++) {
        coro_sleep(task, 50);
        if (KCP_N_CLIENTS == ATOMIC_GET(&_cli_success)) {
            break;
        }
    }
    if (KCP_N_CLIENTS == ATOMIC_GET(&_cli_success)) {
        ATOMIC_SET((atomic_t *)_cli_ok, 1);
        LOG_INFO("kcp tested: %d/%d clients synsend echo ok.", KCP_N_CLIENTS, KCP_N_CLIENTS);
    } else {
        LOG_ERROR("kcp test: %d/%d only.", (int32_t)ATOMIC_GET(&_cli_success), KCP_N_CLIENTS);
    }
}
// 失败出口直接 return、槽停在初值：跑完没置 1 的记成已失败，汇总不必等满超时
static void _cli_startup(task_ctx *task) {
    _cli_startup_run(task);
    test_fail_unfinished(task, _cli_ok);
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
static uint16_t _close_sv_udp;
static int32_t *_close_ok;

typedef struct kcp_close_ctx {
    kcp_ctx  *kcp;
    void     *echo;
    size_t   esize;
    uint64_t elapse;
}kcp_close_ctx;

// synsend 阻塞等待 echo;server 永远不会响应,期望被另一协程的 kcp_stop 用 MSG_TYPE_CLOSE 唤醒而非等满超时
static void _close_waiter(void *owner, void *arg) {
    task_ctx *task = owner;
    kcp_close_ctx *ctx = arg;
    const char msg[] = "kcp_close";
    uint64_t bgts = nowms();
    ctx->echo = kcp_synsend(task, ctx->kcp, (void *)msg, sizeof(msg) - 1, 1, &ctx->esize);
    ctx->elapse = nowms() - bgts;
}
// 短暂延迟后停止同一会话,验证 _kcp_stop 发出的 MSG_TYPE_CLOSE 能及时唤醒 _close_waiter
static void _close_stopper(void *owner, void *arg) {
    task_ctx *task = owner;
    kcp_close_ctx *ctx = arg;
    coro_sleep(task, 200);
    kcp_stop(ctx->kcp);
}
static void _close_startup_run(task_ctx *task) {
    coro_sleep(task, 300);// 等 server TCP listen / UDP 落地
    sock_ctx usk;
    if (ERR_OK != task_udp(task, PACK_UDP_KCP, "0.0.0.0", 0, 0, &usk)) {
        LOG_ERROR("kcp close test udp error.");
        return;
    }
    kcp_ctx kcp;
    kcp_init(&kcp, &task->loader->netev, &usk, KCP_CLOSE_BOGUS_CONV);
    if (ERR_OK != kcp_start(&kcp, task->handle, createid(), "127.0.0.1", _close_sv_udp, NULL)) {
        LOG_ERROR("kcp close test kcp_start error.");
        ev_close(&task->loader->netev, &usk);
        return;
    }
    kcp_close_ctx ctx = { &kcp, NULL, 0, 0 };
    coro_fn funcs[2] = { _close_waiter, _close_stopper };
    void *args[2] = { &ctx, &ctx };
    int32_t rtn = coro_fork_wait(coro_task_co(task), funcs, args, 2);
    ev_close(&task->loader->netev, &usk);
    if (ERR_OK != rtn) {
        LOG_ERROR("kcp close test fork_wait error.");
        return;
    }
    // 下界和上界都要：只卡上界的话，synsend 压根没等就返回 NULL 也算过，
    // 而那证明不了"是 CLOSE 把它唤醒的"。stopper 先 sleep 200ms 才 kcp_stop，
    // 所以真被唤醒的 waiter 必然等过 200ms 上下（留些余量取 150）
    if (NULL == ctx.echo && ctx.elapse >= 150 && ctx.elapse < 2000) {
        ATOMIC_SET((atomic_t *)_close_ok, 1);
        LOG_INFO("kcp close tested: waiter woken by close, elapse %"PRIu64"ms.", ctx.elapse);
    } else {
        LOG_ERROR("kcp close test failed: echo %p elapse %"PRIu64"ms.", ctx.echo, ctx.elapse);
    }
}
// 失败出口直接 return、槽停在初值：跑完没置 1 的记成已失败，汇总不必等满超时
static void _close_startup(task_ctx *task) {
    _close_startup_run(task);
    test_fail_unfinished(task, _close_ok);
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
static uint16_t _fifo_sv_tcp;
static uint16_t _fifo_sv_udp;
static int32_t *_fifo_ok;

typedef struct kcp_fifo_ctx {
    kcp_ctx  *kcp;
    int32_t  idx;
    int32_t  matched;
}kcp_fifo_ctx;

// 多个协程共享同一 kcp_ctx 并发 synsend,验证各自收到的 echo 精确对应自己发送的内容(不串号)
static void _fifo_worker(void *owner, void *arg) {
    task_ctx *task = owner;
    kcp_fifo_ctx *ctx = arg;
    char msg[32];
    int32_t rtn = SNPRINTF(msg, sizeof(msg), "kcp_fifo_%d", ctx->idx);
    size_t mlen = snprintf_lens(rtn, sizeof(msg));
    size_t esize = 0;
    void *echo = kcp_synsend(task, ctx->kcp, msg, mlen, 1, &esize);
    ctx->matched = (NULL != echo && esize == mlen && 0 == memcmp(echo, msg, mlen));
}
static void _fifo_startup_run(task_ctx *task) {
    coro_sleep(task, 300);// 等 server TCP listen / UDP 落地
    // 1. 建 UDP socket(OS 分配端口)
    sock_ctx usk;
    if (ERR_OK != task_udp(task, PACK_UDP_KCP, "0.0.0.0", 0, 0, &usk)) {
        LOG_ERROR("kcp fifo test udp error.");
        return;
    }
    // 2. netaddr_local 取回 OS 分配的本地端口
    netaddr_ctx la;
    if (ERR_OK != netaddr_local(&la, usk.fd)) {
        LOG_ERROR("kcp fifo test netaddr_local error.");
        ev_close(&task->loader->netev, &usk);
        return;
    }
    uint16_t uport = netaddr_port(&la);
    // 3. TCP 握手:上报 UDP 端口,拿回 conv
    sock_ctx tsk;
    if (ERR_OK != coro_connect(task, PACK_NONE, NULL, "127.0.0.1", _fifo_sv_tcp, NETEV_NONE, NULL, &tsk)) {
        LOG_ERROR("kcp fifo test connect error.");
        ev_close(&task->loader->netev, &usk);
        return;
    }
    size_t rsize = 0;
    void *resp = coro_send(task, &tsk, &uport, sizeof(uport), &rsize, 1);
    if (NULL == resp || rsize != sizeof(uint32_t)) {
        LOG_ERROR("kcp fifo test handshake error.");
        coro_close(task, &tsk);
        ev_close(&task->loader->netev, &usk);
        return;
    }
    uint32_t conv;
    memcpy(&conv, resp, sizeof(conv));
    coro_close(task, &tsk);
    // 4. 同一 kcp 会话供 KCP_FIFO_N 个协程并发 synsend
    kcp_ctx kcp;
    kcp_init(&kcp, &task->loader->netev, &usk, conv);
    if (ERR_OK != kcp_start(&kcp, task->handle, createid(), "127.0.0.1", _fifo_sv_udp, NULL)) {
        LOG_ERROR("kcp fifo test kcp_start error.");
        ev_close(&task->loader->netev, &usk);
        return;
    }
    kcp_fifo_ctx ctxs[KCP_FIFO_N];
    coro_fn funcs[KCP_FIFO_N];
    void *args[KCP_FIFO_N];
    int32_t i;
    for (i = 0; i < KCP_FIFO_N; i++) {
        ctxs[i].kcp = &kcp;
        ctxs[i].idx = i;
        ctxs[i].matched = 0;
        funcs[i] = _fifo_worker;
        args[i] = &ctxs[i];
    }
    int32_t rtn = coro_fork_wait(coro_task_co(task), funcs, args, KCP_FIFO_N);
    kcp_stop(&kcp);
    ev_close(&task->loader->netev, &usk);
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
        ATOMIC_SET((atomic_t *)_fifo_ok, 1);
        LOG_INFO("kcp fifo tested: %d/%d concurrent synsend matched.", KCP_FIFO_N, KCP_FIFO_N);
    }
}
// 失败出口直接 return、槽停在初值：跑完没置 1 的记成已失败，汇总不必等满超时
static void _fifo_startup(task_ctx *task) {
    _fifo_startup_run(task);
    test_fail_unfinished(task, _fifo_ok);
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
static sock_ctx _syn_dead_sk;
// 子协程：等主协程挂进 kcp_synsend 后关掉 socket，触发 _kcp_udfree 逐会话补 CLOSE
static void _syn_close_fork(void *owner, void *arg) {
    task_ctx *task = owner;
    (void)arg;
    coro_sleep(task, 50);
    ev_close(_syn_dead_netev, &_syn_dead_sk);
}
static int32_t *_syn_ok;
static int32_t _peer_cases(task_ctx *task);// 定义在下面"对端行为"一节,借本组跑
static int32_t _learn_cases(task_ctx *task);// 定义在下面"学习模式"一节,同上

static void _syn_startup_run(task_ctx *task) {
    sock_ctx usk;
    if (ERR_OK != task_udp(task, PACK_UDP_KCP, "0.0.0.0", 0, 0, &usk)) {
        LOG_ERROR("kcp synstart test udp error.");
        return;
    }
    // 1. 正常建立:全新 conv,预期成功
    kcp_ctx kcp1;
    kcp_init(&kcp1, &task->loader->netev, &usk, 100);
    int32_t r1 = kcp_synstart(task, &kcp1, "127.0.0.1", _syn_sv_udp, NULL);
    // 2. conv 冲突:同一 socket 上重复注册相同 conv,预期失败
    kcp_ctx kcp2;
    kcp_init(&kcp2, &task->loader->netev, &usk, 100);
    int32_t r2 = kcp_synstart(task, &kcp2, "127.0.0.1", _syn_sv_udp, NULL);
    // 3. sess==0 守卫:以 kcp_start(sess=0) 异步建立的会话不能用 kcp_synsend,应立即失败不进 _coro_wait;
    // copy=0 顺带验证守卫分支不漏释放调用方缓冲区(leak 由 MEMORY_CHECK 收尾统计兜底)
    kcp_ctx kcp3;
    kcp_init(&kcp3, &task->loader->netev, &usk, 200);
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
    // 5. stop 后 synstart 被拒:失败路径须置 stopped=1 并还原 maxpack(sess 留着,关闭回调按它认会话)。
    // 先 stop 让 conv 空出给 kcp5,kcp1 再以同 conv 重启必被 event 线程拒;若 stopped 停在 0,句柄就从
    // "已停"变回"在跑",下面的 synsend 会绕过守卫投到已消失的会话,被静默丢弃后空等满超时
    kcp_stop(&kcp1);
    kcp_ctx kcp5;
    kcp_init(&kcp5, &task->loader->netev, &usk, 100);
    int32_t r8 = kcp_synstart(task, &kcp5, "127.0.0.1", _syn_sv_udp, NULL);
    kcp_config cfg576 = { -1, -1, -1, -1, 0, 0, 576 };// mtu 与默认不同,才能验出 maxpack 被还原
    size_t mp_before = kcp1.maxpack;
    int32_t r9 = kcp_synstart(task, &kcp1, "127.0.0.1", _syn_sv_udp, &cfg576);
    int32_t r10 = (1 == kcp1.stopped && mp_before == kcp1.maxpack) ? ERR_OK : ERR_FAILED;
    uint64_t b11 = nowms();
    void *r11 = kcp_synsend(task, &kcp1, "y", 1, 1, &esize);
    uint64_t elapse11 = nowms() - b11;
    kcp_ctx kcp6;
    kcp_init(&kcp6, &task->loader->netev, &usk, 400);
    uint64_t s12a = createid();
    int32_t r12 = kcp_start(&kcp6, task->handle, s12a, "127.0.0.1", _syn_sv_udp, NULL);
    int32_t r13 = kcp_synstart(task, &kcp6, "127.0.0.1", _syn_sv_udp, NULL);
    int32_t r14 = (0 == kcp6.stopped && 0 != kcp6.sess && s12a != kcp6.sess) ? ERR_OK : ERR_FAILED;
    kcp_stop(&kcp6);
    kcp_ctx kcp7;
    kcp_init(&kcp7, &task->loader->netev, &usk, 400);
    int32_t r15 = kcp_synstart(task, &kcp7, "127.0.0.1", _syn_sv_udp, NULL);
    kcp_stop(&kcp7);
    kcp_ctx kcp8;
    kcp_init(&kcp8, &task->loader->netev, &usk, 100);
    int32_t r16 = kcp_start(&kcp8, task->handle, createid(), "127.0.0.1", _syn_sv_udp, NULL);
    kcp_stop(&kcp5);
    int32_t r17 = kcp_synstart(task, &kcp8, "127.0.0.1", _syn_sv_udp, NULL);
    kcp_stop(&kcp8);
    kcp_stop(&kcp3);
    ev_close(&task->loader->netev, &usk);
    // 6. CLOSE 唤醒 kcp_synsend:挂起期间 socket 被关,_kcp_udfree 逐会话补 CLOSE;
    // 唤醒后句柄须置 stopped=1、sess 不动:stopped 不置则下次 synsend 投到已消失的会话、空等满超时,
    // 之后的 kcp_start 也会被当成重启存活会话;sess 留着,关闭回调按它认会话。
    // 与 bin/script/lib/kcp.lua 的 ctx:send 同款分支互为镜像
    sock_ctx dsk;
    int32_t r6 = ERR_FAILED;
    int32_t r7 = ERR_FAILED;
    uint64_t elapse6 = 0;
    kcp_ctx kcp4;
    if (ERR_OK == task_udp(task, PACK_UDP_KCP, "0.0.0.0", 0, 0, &dsk)) {
        kcp_init(&kcp4, &task->loader->netev, &dsk, 300);
        if (ERR_OK == kcp_synstart(task, &kcp4, "127.0.0.1", _syn_sv_udp, NULL)) {
            uint64_t s7 = kcp4.sess;
            _syn_dead_netev = &task->loader->netev;
            _syn_dead_sk = dsk;
            // 收窄放在 synstart 成功之后:否则安装步骤自身可能超时,后续断言会以错误理由通过
            task_set_netread_timeout(task, 3000);
            coro_fork(coro_task_co(task), _syn_close_fork, NULL);
            uint64_t b6 = nowms();
            // 该会话对端是 kcp server,但本次不等 echo——先被 fork 协程关 socket 触发的 CLOSE 唤醒
            void *r6p = kcp_synsend(task, &kcp4, "x", 1, 1, &esize);
            elapse6 = nowms() - b6;
            r6 = (NULL == r6p) ? ERR_OK : ERR_FAILED;
            r7 = (s7 == kcp4.sess && 1 == kcp4.stopped) ? ERR_OK : ERR_FAILED;
        } else {
            // 成功路径的 close 在 _syn_close_fork 里，安装失败就没人关它了
            ev_close(&task->loader->netev, &dsk);
        }
    }
    int32_t r18 = _peer_cases(task);
    int32_t r19 = _learn_cases(task);
    if (ERR_OK == r1 && ERR_OK != r2 && ERR_OK == r3 && elapse3 < 1000 && NULL == r4 && ERR_OK == r5
        && ERR_OK == r8 && ERR_OK != r9 && ERR_OK == r10 && NULL == r11 && elapse11 < 1000
        && ERR_OK == r6 && ERR_OK == r7 && elapse6 < 2000
        && ERR_OK == r12 && ERR_OK == r13 && ERR_OK == r14 && ERR_OK == r15
        && ERR_OK == r16 && ERR_OK == r17 && ERR_OK == r18 && ERR_OK == r19) {
        ATOMIC_SET((atomic_t *)_syn_ok, 1);
        LOG_INFO("kcp synstart tested: normal/restart ok, collision/sess-guard rejected,"
                 " reject sets stopped and restores maxpack, CLOSE keeps sess and sets stopped,"
                 " same-handle restart implicitly stops the live session and re-registers conv,"
                 " async handle refused by the event thread stays restartable, peer and learn cases ok.");
    } else {
        LOG_ERROR("kcp synstart test failed: r1=%d r2=%d r3=%d elapse3=%"PRIu64" r4=%p r5=%d"
                  " r8=%d r9=%d r10=%d r11=%p elapse11=%"PRIu64" r6=%d r7=%d elapse6=%"PRIu64
                  " r12=%d r13=%d r14=%d r15=%d r16=%d r17=%d r18=%d r19=%d.",
                  r1, r2, r3, elapse3, r4, r5, r8, r9, r10, r11, elapse11, r6, r7, elapse6,
                  r12, r13, r14, r15, r16, r17, r18, r19);
    }
}
// 失败出口直接 return、槽停在初值：跑完没置 1 的记成已失败，汇总不必等满超时
static void _syn_startup(task_ctx *task) {
    _syn_startup_run(task);
    test_fail_unfinished(task, _syn_ok);
}
void task_kcp_synstart_start(loader_ctx *loader, const char *name, uint16_t sv_udp_port, int32_t *ok) {
    if (NULL == ok) {
        return;
    }
    _syn_sv_udp = sv_udp_port;
    _syn_ok = ok;
    coro_task_register(loader, name, 0, _syn_startup, NULL, NULL, NULL);
}

// ================= 新用例共用:建 UDP / 握手拿 conv =================
// 建 UDP(OS 分配端口)并取回端口;失败时已关好
static int32_t _kcp_test_udp(task_ctx *task, sock_ctx *usk, uint16_t *port) {
    netaddr_ctx la;
    if (ERR_OK != task_udp(task, PACK_UDP_KCP, "0.0.0.0", 0, 0, usk)) {
        return ERR_FAILED;
    }
    if (ERR_OK != netaddr_local(&la, usk->fd)) {
        ev_close(&task->loader->netev, usk);
        return ERR_FAILED;
    }
    *port = netaddr_port(&la);
    return ERR_OK;
}
// 建 UDP 后经 TCP 上报端口,拿回 server 分配的 conv;失败时 UDP 已关好
static int32_t _kcp_test_conv(task_ctx *task, uint16_t sv_tcp, sock_ctx *usk, uint32_t *conv) {
    uint16_t uport;
    sock_ctx tsk;
    size_t rsize = 0;
    void *resp;
    if (ERR_OK != _kcp_test_udp(task, usk, &uport)) {
        return ERR_FAILED;
    }
    if (ERR_OK != coro_connect(task, PACK_NONE, NULL, "127.0.0.1", sv_tcp, NETEV_NONE, NULL, &tsk)) {
        ev_close(&task->loader->netev, usk);
        return ERR_FAILED;
    }
    resp = coro_send(task, &tsk, &uport, sizeof(uport), &rsize, 1);
    if (NULL == resp || rsize != sizeof(uint32_t)) {
        coro_close(task, &tsk);
        ev_close(&task->loader->netev, usk);
        return ERR_FAILED;
    }
    memcpy(conv, resp, sizeof(*conv));// resp 只到下次 coro_* 前有效
    coro_close(task, &tsk);
    return ERR_OK;
}

// ================= 对端行为:maxpack 大消息 / 源地址校验 / 0 长度消息 / 改投与目标退出 =================
// 借 kcp_synstart 那组跑(_syn_startup 末尾调),四个 socket 的分工见文件头的 PEER_A..PEER_E
static const char *_peer_msgs[] = { "inject", "after", "sink", "lost", "back" };
static sock_ctx _peer_bsk;
static kcp_ctx _peer_kb;// b 上的会话,回显用
static atomic_t _peer_self;// 本 task 收到的消息位
static atomic_t _peer_sink;// 临时 task 收到的消息位

// 认出是 _peer_msgs 里哪一条,返回它的位;认不出返回最高位
static int32_t _peer_bit(const void *data, size_t size) {
    for (int32_t i = 0; i < (int32_t)ARRAY_SIZE(_peer_msgs); i++) {
        if (strlen(_peer_msgs[i]) == size && 0 == memcmp(data, _peer_msgs[i], size)) {
            return 1 << i;
        }
    }
    return 1 << 30;
}
static void _peer_recvfrom(task_ctx *task, sock_ctx *sk, subtype_t pktype, const netaddr_ctx *addr, void *data, size_t size) {
    (void)task;
    (void)pktype;
    (void)addr;
    if (sk->skid == _peer_bsk.skid) {
        kcp_send(&_peer_kb, data, size, 1);// b 上原样回显
        return;
    }
    ATOMIC_ADD(&_peer_self, _peer_bit(data, size));
}
static void _peer_sink_recvfrom(task_ctx *task, sock_ctx *sk, subtype_t pktype, const netaddr_ctx *addr, void *data, size_t size) {
    (void)task;
    (void)sk;
    (void)pktype;
    (void)addr;
    ATOMIC_ADD(&_peer_sink, _peer_bit(data, size));
}
// 手拼一个 PUSH 段(24 字节头 + 数据,小端,字段顺序同 ikcp 的段编码),从 sk 发到本机 port
static void _peer_push(task_ctx *task, sock_ctx *sk, uint16_t port, uint32_t conv, uint32_t sn, const char *data) {
    char seg[64];
    size_t lens = strlen(data);
    write_le32(seg, conv);
    seg[4] = KCP_SEG_PUSH;
    seg[5] = 0;// frg
    write_le16(seg + 6, 128);// wnd
    write_le32(seg + 8, 0);// ts
    write_le32(seg + 12, sn);
    write_le32(seg + 16, 0);// una
    write_le32(seg + 20, (uint32_t)lens);
    memcpy(seg + 24, data, lens);
    ev_sendto(&task->loader->netev, sk, "127.0.0.1", port, seg, 24 + lens, 1);
}
// 每 20ms 看一次,至多 2 秒,等 got 里出现 bits
static int32_t _peer_wait(task_ctx *task, atomic_t *got, int32_t bits) {
    for (int32_t i = 0; i < 100; i++) {
        if (bits == ((int32_t)ATOMIC_GET(got) & bits)) {
            return 1;
        }
        coro_sleep(task, 20);
    }
    return 0;
}
// 从 coro_dump 末行抠出 sess 条目数;取不到返回 -1(同 test_coro.c 的同名函数)
static int32_t _co_sessions(coro_ctx *co) {
    size_t lens = 0;
    char *dump = coro_dump(co, &lens);
    int32_t n = -1;
    char *p = strstr(dump, " sessions");
    if (NULL != p) {
        while (p > dump && ' ' != p[-1]) {
            p--;
        }
        n = atoi(p);
    }
    FREE(dump);
    return n;
}
// 1) a / b 互为对端,恰好 maxpack(127 片)的消息端到端回显,多 1 字节本地即拒;
// 2) mtu 恰为 KCP_MTU_MAX 生效、大 1 保持默认(只在本地算 maxpack,不发包);
// 3) a 上 conv 3 登记 d 为对端:e 冒充发的 sn 0 须被丢;d 先发 0 长度消息再发 after,前者不能堵住后者;
// 4) 改投到临时 task 后消息归它;它退出后到的 lost 被取出丢掉,改回本 task 后不会再冒出来
static int32_t _peer_cases(task_ctx *task) {
    ev_ctx *netev = &task->loader->netev;
    sock_ctx sks[PEER_NSK];
    uint16_t ports[PEER_NSK];
    int32_t nsk = 0;
    while (nsk < PEER_NSK && ERR_OK == _kcp_test_udp(task, &sks[nsk], &ports[nsk])) {
        nsk++;
    }
    if (PEER_NSK != nsk) {
        for (int32_t i = 0; i < nsk; i++) {
            ev_close(netev, &sks[i]);
        }
        LOG_ERROR("kcp peer test udp error.");
        return ERR_FAILED;
    }
    ATOMIC_SET(&_peer_self, 0);
    ATOMIC_SET(&_peer_sink, 0);
    _peer_bsk = sks[PEER_B];
    task_recvedfrom(task, _peer_recvfrom);
    task_set_netread_timeout(task, 10000);
    // 1) 两端都关流控、10ms 一轮,127 片几轮就走完
    kcp_config fast = { 1, 10, -1, 1, 0, 0, 0 };
    kcp_ctx ka;
    int32_t big_ok = 0;
    int32_t over_ok = 0;
    kcp_init(&ka, netev, &sks[PEER_A], 1);
    kcp_init(&_peer_kb, netev, &sks[PEER_B], 1);
    if (ERR_OK == kcp_synstart(task, &ka, "127.0.0.1", ports[PEER_B], &fast)
        && ERR_OK == kcp_synstart(task, &_peer_kb, "127.0.0.1", ports[PEER_A], &fast)) {
        size_t esize = 0;
        char *big;
        MALLOC(big, ka.maxpack + 1);
        for (size_t i = 0; i <= ka.maxpack; i++) {
            big[i] = (char)(i * 31 + 7);
        }
        over_ok = (ERR_FAILED == kcp_send(&ka, big, ka.maxpack + 1, 1));
        void *echo = kcp_synsend(task, &ka, big, ka.maxpack, 1, &esize);
        big_ok = (NULL != echo && esize == ka.maxpack && 0 == memcmp(echo, big, esize));
        FREE(big);
    }
    // 2)
    kcp_ctx km;
    kcp_init(&km, netev, &sks[PEER_A], 500);
    size_t mp_def = km.maxpack;
    kcp_config mtucfg = { -1, -1, -1, -1, 0, 0, KCP_MTU_MAX };
    int32_t mtu_ok = (ERR_OK == kcp_start(&km, task->handle, 0, "127.0.0.1", ports[PEER_B], &mtucfg)
        && (size_t)127 * (KCP_MTU_MAX - 24) == km.maxpack);
    kcp_stop(&km);
    mtucfg.mtu = KCP_MTU_MAX + 1;
    mtu_ok = mtu_ok && ERR_OK == kcp_start(&km, task->handle, 0, "127.0.0.1", ports[PEER_B], &mtucfg)
        && mp_def == km.maxpack;
    kcp_stop(&km);
    // 3) 4)
    kcp_ctx kc;
    int32_t raw_ok = 0;
    int32_t sink_ok = 0;
    int32_t back_ok = 0;
    kcp_init(&kc, netev, &sks[PEER_A], 3);
    if (ERR_OK == kcp_synstart(task, &kc, "127.0.0.1", ports[PEER_D], NULL)) {
        _peer_push(task, &sks[PEER_E], ports[PEER_A], 3, 0, "inject");
        coro_sleep(task, 100);// 冒充的先到:校验失效的话它会占走 sn 0 被投出来
        _peer_push(task, &sks[PEER_D], ports[PEER_A], 3, 0, "");
        _peer_push(task, &sks[PEER_D], ports[PEER_A], 3, 1, "after");
        raw_ok = _peer_wait(task, &_peer_self, PEER_AFTER);
        task_ctx *sink = task_new(task->loader, NULL, 0, NULL, NULL, NULL);
        name_t hsink = sink->handle;
        task_recvedfrom(sink, _peer_sink_recvfrom);
        (void)task_register(sink, NULL, NULL);// 匿名 task 不会重名
        // kcp_synstart 留下一条 keep 条目,转交后 CLOSE 投给 sink,kcp_bind_task 须当场放掉它
        int32_t s0 = _co_sessions(coro_task_co(task));
        sink_ok = (ERR_OK == kcp_bind_task(task, &kc, hsink));
        int32_t s1 = _co_sessions(coro_task_co(task));
        if (sink_ok && s0 - 1 != s1) {
            LOG_ERROR("kcp peer: bind_task should release the session entry, sessions %d -> %d.", s0, s1);
            sink_ok = 0;
        }
        coro_sleep(task, 100);// 等改投在 a 的 event 线程上落地
        _peer_push(task, &sks[PEER_D], ports[PEER_A], 3, 2, "sink");
        sink_ok = sink_ok && _peer_wait(task, &_peer_sink, PEER_SINK);
        // 挂起期间 loader 可能已在关闭、sink 已被释放：按句柄重新取，不碰前面的裸指针
        sink = task_grab(task->loader, hsink);
        if (NULL != sink) {
            task_close(sink);
            task_ungrab(sink);
        }
        for (int32_t i = 0; i < 100 && task_exists(task->loader, hsink); i++) {
            coro_sleep(task, 20);
        }
        _peer_push(task, &sks[PEER_D], ports[PEER_A], 3, 3, "lost");
        coro_sleep(task, 300);// lost 须在改回之前到 a
        back_ok = (ERR_OK == kcp_handle(&kc, task->handle));
        coro_sleep(task, 100);
        _peer_push(task, &sks[PEER_D], ports[PEER_A], 3, 4, "back");
        back_ok = back_ok && _peer_wait(task, &_peer_self, PEER_BACK);
    }
    kcp_stop(&ka);
    kcp_stop(&_peer_kb);
    kcp_stop(&kc);
    for (int32_t i = 0; i < PEER_NSK; i++) {
        ev_close(netev, &sks[i]);
    }
    // 本 task 只该收到 after 与 back:多出 inject 是源地址校验失效,多出 lost 是目标退出时消息没被取走
    int32_t self = (int32_t)ATOMIC_GET(&_peer_self);
    int32_t sinkgot = (int32_t)ATOMIC_GET(&_peer_sink);
    if (big_ok && over_ok && mtu_ok && raw_ok && sink_ok && back_ok
        && (PEER_AFTER | PEER_BACK) == self && PEER_SINK == sinkgot) {
        LOG_INFO("kcp peer tested: maxpack echo, mtu bound, source check, empty message, redirect and gone target ok.");
        return ERR_OK;
    }
    LOG_ERROR("kcp peer test failed: big %d over %d mtu %d raw %d sink %d back %d self %d sink got %d.",
              big_ok, over_ok, mtu_ok, raw_ok, sink_ok, back_ok, self, sinkgot);
    return ERR_FAILED;
}

// ================= 空闲停表后的唤醒 =================
// 前提:两端会话闲够一个 interval 就停表(判据由 test_protocol.c 的 test_kcp_idle 锁住)。
// 判据写错成永不成立时本组照样通过:它只验"停了表的会话能被唤醒、能被 stop"
static uint16_t _idle_sv_tcp;
static uint16_t _idle_sv_udp;
static int32_t *_idle_ok;

// synsend 一条并校验 echo,返回耗时(ms);失败返回 UINT64_MAX
static uint64_t _idle_echo(task_ctx *task, kcp_ctx *kcp, const char *msg) {
    size_t mlen = strlen(msg);
    size_t esize = 0;
    uint64_t bgts = nowms();
    void *echo = kcp_synsend(task, kcp, (void *)msg, mlen, 1, &esize);
    if (NULL == echo || esize != mlen || 0 != memcmp(echo, msg, mlen)) {
        return UINT64_MAX;
    }
    return nowms() - bgts;
}
static void _idle_startup_run(task_ctx *task) {
    sock_ctx usk;
    uint32_t conv;
    kcp_ctx kcp;
    uint64_t ea, eb;
    coro_sleep(task, 300);// 等 server TCP listen / UDP 落地
    if (ERR_OK != _kcp_test_conv(task, _idle_sv_tcp, &usk, &conv)) {
        LOG_ERROR("kcp idle test handshake error.");
        return;
    }
    kcp_init(&kcp, &task->loader->netev, &usk, conv);
    if (ERR_OK != kcp_start(&kcp, task->handle, createid(), "127.0.0.1", _idle_sv_udp, NULL)) {
        LOG_ERROR("kcp idle test kcp_start error.");
        ev_close(&task->loader->netev, &usk);
        return;
    }
    task_set_netread_timeout(task, 3000);// 漏唤醒会等满它(监控 1 秒粒度,实际 3~4 秒),与正常的几百毫秒分得开
    coro_sleep(task, 500);// 默认 interval 100:两端会话都已闲过一个 interval,停了表
    // A 收包唤醒:server 只收不回,只能靠它收包挂回后 flush 出 ACK;收不到 ACK 时 client 拥塞窗口停在 1,
    // 紧跟的 idle_a 搬不进在途队列,等满超时
    kcp_send(&kcp, KCP_NOECHO "_a", sizeof(KCP_NOECHO "_a") - 1, 1);
    ea = _idle_echo(task, &kcp, "idle_a");
    // B 发送唤醒:再闲 1.5 秒两端都已停表,只能靠 client 发送时挂回(清了 parked 却漏了入堆也卡在这里)
    coro_sleep(task, 1500);
    eb = _idle_echo(task, &kcp, "idle_b");
    // C 停一个已停表的会话:它不在堆里,误删即弄坏堆(本 socket 上只有它,size 从 0 回绕,ASan 下当场报)
    coro_sleep(task, 500);
    kcp_stop(&kcp);
    ev_close(&task->loader->netev, &usk);
    // 上界:按格点唤醒最坏约 4 个 interval + RTT,留足余量;server 侧停表的会话随 loader_free 走 _kcp_udfree
    if (ea < 2000 && eb < 2000) {
        ATOMIC_SET((atomic_t *)_idle_ok, 1);
        LOG_INFO("kcp idle tested: recv wakeup %"PRIu64"ms, send wakeup %"PRIu64"ms, stop parked ok.", ea, eb);
    } else {
        LOG_ERROR("kcp idle test failed: recv wakeup %"PRIu64"ms, send wakeup %"PRIu64"ms.", ea, eb);
    }
}
// 失败出口直接 return、槽停在初值：跑完没置 1 的记成已失败，汇总不必等满超时
static void _idle_startup(task_ctx *task) {
    _idle_startup_run(task);
    test_fail_unfinished(task, _idle_ok);
}
void task_kcp_idle_start(loader_ctx *loader, const char *name, uint16_t sv_tcp_port, uint16_t sv_udp_port, int32_t *ok) {
    if (NULL == ok) {
        return;
    }
    _idle_sv_tcp = sv_tcp_port;
    _idle_sv_udp = sv_udp_port;
    _idle_ok = ok;
    coro_task_register(loader, name, 0, _idle_startup, NULL, NULL, NULL);
}

// ================= 学习模式:kcp_start 的 port 为 0,首包来源定为对端 =================
// 借 kcp_synstart 那组跑(_syn_startup 末尾调),三个 socket 的分工见文件头的 LEARN_A..LEARN_E,端口都由 OS 分配
static sock_ctx _learn_ask;
static kcp_ctx _learn_ka;// a 上的学习会话,收到即回显
static atomic_t _learn_hits;// a 上收到的消息数

static void _learn_recvfrom(task_ctx *task, sock_ctx *sk, subtype_t pktype, const netaddr_ctx *addr, void *data, size_t size) {
    (void)task;
    (void)pktype;
    (void)addr;
    if (sk->skid != _learn_ask.skid) {
        return;
    }
    ATOMIC_ADD(&_learn_hits, 1);
    kcp_send(&_learn_ka, data, size, 1);
}
// a 上以 ip:0 起 conv 的学习会话:b 先发、拿到回显,说明学到了 b 的端口且能回发;之后 e 冒充同 conv 发的
// 被丢(e 等满超时),b 再发照常回显,a 一共只收到 b 的两条
static int32_t _learn_one(task_ctx *task, sock_ctx *sks, uint16_t aport, uint32_t conv, const char *ip) {
    ev_ctx *netev = &task->loader->netev;
    kcp_ctx kb, ke;
    size_t esize = 0;
    int32_t ok = 0;
    ATOMIC_SET(&_learn_hits, 0);
    kcp_init(&_learn_ka, netev, &sks[LEARN_A], conv);
    kcp_init(&kb, netev, &sks[LEARN_B], conv);
    kcp_init(&ke, netev, &sks[LEARN_E], conv);
    if (ERR_OK == kcp_start(&_learn_ka, task->handle, createid(), ip, 0, NULL)
        && ERR_OK == kcp_synstart(task, &kb, "127.0.0.1", aport, NULL)
        && ERR_OK == kcp_synstart(task, &ke, "127.0.0.1", aport, NULL)) {
        ok = (UINT64_MAX != _idle_echo(task, &kb, "learn_1"));
        task_set_netread_timeout(task, 500);
        ok = ok && NULL == kcp_synsend(task, &ke, "learn_x", 7, 1, &esize);
        task_set_netread_timeout(task, 10000);
        ok = ok && UINT64_MAX != _idle_echo(task, &kb, "learn_2") && 2 == (int32_t)ATOMIC_GET(&_learn_hits);
    }
    kcp_stop(&_learn_ka);
    kcp_stop(&kb);
    kcp_stop(&ke);
    return ok;
}
// 给定 ip 只学端口(conv 1);通配 ip 连 ip 一起学(conv 2)
static int32_t _learn_cases(task_ctx *task) {
    ev_ctx *netev = &task->loader->netev;
    sock_ctx sks[LEARN_NSK];
    uint16_t ports[LEARN_NSK];
    int32_t nsk = 0;
    int32_t port_ok = 0;
    int32_t all_ok = 0;
    while (nsk < LEARN_NSK && ERR_OK == _kcp_test_udp(task, &sks[nsk], &ports[nsk])) {
        nsk++;
    }
    if (LEARN_NSK == nsk) {
        _learn_ask = sks[LEARN_A];
        task_recvedfrom(task, _learn_recvfrom);
        port_ok = _learn_one(task, sks, ports[LEARN_A], 1, "127.0.0.1");
        all_ok = _learn_one(task, sks, ports[LEARN_A], 2, "0.0.0.0");
    }
    for (int32_t i = 0; i < nsk; i++) {
        ev_close(netev, &sks[i]);
    }
    if (port_ok && all_ok) {
        LOG_INFO("kcp learn tested: port-only and any-ip sessions bind the first sender, impostor dropped.");
        return ERR_OK;
    }
    LOG_ERROR("kcp learn test failed: udp %d port-only %d any-ip %d.", nsk, port_ok, all_ok);
    return ERR_FAILED;
}

// ================= 死链:对端失联后在途数据重传超限,会话被动拆除并投 CLOSE =================
static int32_t *_dead_ok;
static int32_t _dead_abort;// 收到过死链会话的 kcp CLOSE(erro 为 CLOSE_TYPE_ABORT,sess 为 _dead_sess)
static uint64_t _dead_sess;// conv 1 会话的 sess,synstart 内部生成,建好后从句柄上取
static sock_ctx _dead_bsk;// 对端 socket b:只在它上面回显
static kcp_ctx _dead_peer;// conv 1 的对端会话:收到即原样回,只用于热身
static kcp_ctx _dead_peer2;// conv 2 的对端会话:conv 1 拆掉后才建,回显 KCP_DL2 开头的消息

// 只在 b 上回显:a 上没人等的回显(conv 2 的第二条)也会落到这里,弹回去就来回打乒乓
static void _dead_recvfrom(task_ctx *task, sock_ctx *sk, subtype_t pktype, const netaddr_ctx *addr, void *data, size_t size) {
    (void)task;
    (void)pktype;
    (void)addr;
    if (sk->skid != _dead_bsk.skid) {
        return;
    }
    if (size >= sizeof(KCP_DL2) - 1 && 0 == memcmp(data, KCP_DL2, sizeof(KCP_DL2) - 1)) {
        kcp_send(&_dead_peer2, data, size, 1);
    } else {
        kcp_send(&_dead_peer, data, size, 1);
    }
}
// 会话级关闭的 sess 是会话的 sess,不是 socket 的 skid;认不出是哪个会话的 ABORT 不算
static void _dead_closed(task_ctx *task, sock_ctx *sk, subtype_t pktype, uint8_t client, int32_t erro, uint64_t sess) {
    (void)task;
    (void)client;
    if (PACK_UDP_KCP == pktype && CLOSE_TYPE_ABORT == erro
        && sess == _dead_sess && sess != sk->skid) {
        _dead_abort = 1;
    }
}
// 本 task 内两个 UDP socket 互为对端:先热身一次 echo 让 rx_rto 落到 30ms 附近,再停掉对端会话,
// 之后的 synsend 只能等某段重传到 dead_link(默认 20)次后的 CLOSE 唤醒。
// nodelay=2 让重传间隔每次只加 rx_rto/2(不翻倍),20 次约 3.1 秒;不停表也靠这个:停了就不重传、等满超时。
// 同一 socket a 上另挂 conv 2:对端先不建,它发的一条收不到确认,用库默认配置一直重传(间隔逐次翻倍,判死链要几分钟),
// conv 1 被拆时它还在堆里;之后再建它的对端,synsend 能拿到回显,说明拆会话没把同 socket 上别的会话弄出堆
static void _dead_startup_run(task_ctx *task) {
    kcp_config fast = { 2, 10, -1, -1, 0, 0, 0 };
    sock_ctx ask, bsk;
    uint16_t aport, bport;
    kcp_ctx kcp, kcp2;
    size_t esize = 0;
    void *warm = NULL;
    void *dead = NULL;
    void *alive = NULL;
    int32_t pre = 0;
    int32_t closed = 0;
    int32_t survived = 0;
    uint64_t bgts, elapse = 0;
    _dead_abort = 0;
    _dead_sess = 0;
    task_recvedfrom(task, _dead_recvfrom);
    task_closed(task, _dead_closed);
    if (ERR_OK != _kcp_test_udp(task, &ask, &aport)) {
        LOG_ERROR("kcp dead link test udp error.");
        return;
    }
    if (ERR_OK != _kcp_test_udp(task, &bsk, &bport)) {
        LOG_ERROR("kcp dead link test udp error.");
        ev_close(&task->loader->netev, &ask);
        return;
    }
    _dead_bsk = bsk;
    kcp_init(&kcp, &task->loader->netev, &ask, 1);
    kcp_init(&kcp2, &task->loader->netev, &ask, 2);
    kcp_init(&_dead_peer, &task->loader->netev, &bsk, 1);
    kcp_init(&_dead_peer2, &task->loader->netev, &bsk, 2);
    if (ERR_OK == kcp_synstart(task, &kcp, "127.0.0.1", bport, &fast)
        && ERR_OK == kcp_synstart(task, &_dead_peer, "127.0.0.1", aport, &fast)) {
        _dead_sess = kcp.sess;
        warm = kcp_synsend(task, &kcp, "dl_warm", 7, 1, &esize);
    }
    if (NULL != warm) {
        // synstart 只等本地登记,不要对端;b 上还没有 conv 2,这条一直重传、不停表
        pre = (ERR_OK == kcp_synstart(task, &kcp2, "127.0.0.1", bport, NULL)
            && ERR_OK == kcp_send(&kcp2, KCP_DL2 "pre", sizeof(KCP_DL2 "pre") - 1, 1));
        kcp_stop(&_dead_peer);// 之后发过去的包在对端查不到 conv,被丢,不回 ACK
        coro_sleep(task, 200);// 等 stop 在对端 event 线程上落地
        task_set_netread_timeout(task, 120000);// 判死链约 3 秒;rx_rto 跟着热身那次往返走,ASan / 重负载下要 40 秒往上
        bgts = nowms();
        dead = kcp_synsend(task, &kcp, "dl_x", 4, 1, &esize);
        elapse = nowms() - bgts;
        // CLOSE 唤醒与超时(走 kcp_stop)都只置 stopped=1、sess 不动
        closed = (NULL == dead && _dead_sess == kcp.sess && 1 == kcp.stopped);
        coro_sleep(task, 100);// close 回调协程排在被唤醒的等待者之后跑
    }
    if (closed && pre
        && ERR_OK == kcp_synstart(task, &_dead_peer2, "127.0.0.1", aport, &fast)) {
        // 先到的回显可能是在途那条 pre(它没确认前后面这条发不出去),哪条都算
        alive = kcp_synsend(task, &kcp2, KCP_DL2 "y", sizeof(KCP_DL2 "y") - 1, 1, &esize);
        survived = (NULL != alive && esize >= sizeof(KCP_DL2) - 1
            && 0 == memcmp(alive, KCP_DL2, sizeof(KCP_DL2) - 1));
    }
    kcp_stop(&kcp);
    kcp_stop(&kcp2);
    kcp_stop(&_dead_peer);
    kcp_stop(&_dead_peer2);
    ev_close(&task->loader->netev, &ask);
    ev_close(&task->loader->netev, &bsk);
    // 下界:rx_rto 不低于 30,20 次发送至少隔 19 段线性增长的 rto,约 3.1 秒,取 2000 证明是重传超限而非别的原因
    if (closed && _dead_abort && elapse >= 2000 && survived) {
        ATOMIC_SET((atomic_t *)_dead_ok, 1);
        LOG_INFO("kcp dead link tested: session closed with abort after %"PRIu64"ms of retransmits,"
                 " the other session on the same socket still driven.", elapse);
    } else {
        LOG_ERROR("kcp dead link test failed: warm %p pre %d dead %p closed %d abort %d elapse %"PRIu64"ms"
                  " survived %d.", warm, pre, dead, closed, _dead_abort, elapse, survived);
    }
}
// 失败出口直接 return、槽停在初值：跑完没置 1 的记成已失败，汇总不必等满超时
static void _dead_startup(task_ctx *task) {
    _dead_startup_run(task);
    test_fail_unfinished(task, _dead_ok);
}
void task_kcp_dead_start(loader_ctx *loader, const char *name, int32_t *ok) {
    if (NULL == ok) {
        return;
    }
    _dead_ok = ok;
    coro_task_register(loader, name, 0, _dead_startup, NULL, NULL, NULL);
}
