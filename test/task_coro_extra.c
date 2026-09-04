#include "task_coro_extra.h"

// 并发 coro_sendto 的协程数；payload 固定 2 字节("pN")，靠末位数字回认是哪一份
#define CONCURRENT_N 4
// 等超时唤醒要多给的富余：_coro_timeout_monitor 每 1s 才扫一次到期堆(coro.c)，
// 所以 N 毫秒的 _coro_wait 最坏 N+1000 才被观察到。凡断言超时的地方一律等 N + 本值
#define TIMEOUT_SETTLE_MS 1300

typedef struct task_coro_extra_args {
    uint16_t httpport;
    uint16_t udpport;
    const char *rpcname;
    int32_t *ok;
}task_coro_extra_args;
// CLOSE 排空期间重新注册用：两次 _coro_wait 各自拿到的 mtype
typedef struct reregister_arg {
    uint64_t skid;
    msg_type first;
    msg_type second;
}reregister_arg;
// FIFO 顺序用：两个协程各自把自己的序号写进 order[]
typedef struct fifo_arg {
    uint64_t sess;
    int32_t idx;
    int32_t *n;
    int32_t *order;
}fifo_arg;
// 单个等待者用：只回带醒来时的 mtype
typedef struct waiter_arg {
    uint64_t sess;
    msg_type mtype;
    uint32_t ms;
    msg_type woke;
}waiter_arg;
// 并发 sendto 的每协程入参；nok / seen 指向调用方栈上的共享计数
typedef struct sendto_arg {
    SOCKET fd;
    uint64_t skid;
    uint16_t port;
    int32_t idx;
    int32_t *nok;
    int32_t *seen;
}sendto_arg;

// coro_sleep 1500ms：跨过 tv1（256ms）触发 tv2 cascade 路径
static int32_t _test_sleep_cascade(task_ctx *task) {
    uint64_t t0 = nowms();
    coro_sleep(task, 1500);
    uint64_t diff = nowms() - t0;
    // 容忍 ±150ms 抖动；上界放宽是因为 worker 被其他 task 占用时唤醒会延后
    if (diff < 1450 || diff > 1800) {
        LOG_ERROR("coro_sleep cascade: expected ~1500ms, got %llums.",
                  (unsigned long long)diff);
        return ERR_FAILED;
    }
    return ERR_OK;
}

// coro_connect 到 127.0.0.1:1（保留端口，必拒绝），验证错误返回
static int32_t _test_connect_refused(task_ctx *task) {
    SOCKET fd;
    uint64_t skid;
    int32_t r = coro_connect(task, PACK_HTTP, NULL, "127.0.0.1", 1, 0, NULL, &fd, &skid);
    if (ERR_OK == r) {
        // 不应该连成功；连上了立即关掉再报错
        ev_close(&task->loader->netev, fd, skid);
        LOG_ERROR("coro_connect refused: 127.0.0.1:1 unexpectedly accepted.");
        return ERR_FAILED;
    }
    return ERR_OK;
}

// 先连上 http server，立即 ev_close 后再调 coro_send，验证 send 在断开 fd 上正确失败
static int32_t _test_send_after_close(task_ctx *task, uint16_t httpport) {
    SOCKET fd;
    uint64_t skid;
    if (ERR_OK != coro_connect(task, PACK_HTTP, NULL, "127.0.0.1", httpport, 0, NULL, &fd, &skid)) {
        LOG_ERROR("coro_send-after-close: pre-connect to http_sv failed.");
        return ERR_FAILED;
    }
    ev_close(&task->loader->netev, fd, skid);
    // 等关连接消息穿过事件循环；时间轮粒度 1ms，50ms 足够
    coro_sleep(task, 50);
    // 构造一个最小 HTTP GET 包发送，预期 coro_send 返回 NULL
    const char *req = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    size_t rsize = 0;
    void *resp = coro_send(task, fd, skid, (void *)req, strlen(req), &rsize, 1);
    if (NULL != resp) {
        LOG_ERROR("coro_send-after-close: expected NULL, got resp size=%zu.", rsize);
        return ERR_FAILED;
    }
    return ERR_OK;
}

// dns_lookup 走 PACK_DNS 协议 + coro_sendto/coro_send，覆盖 DNS 协程路径
static int32_t _test_dns_lookup(task_ctx *task) {
    size_t cnt = 0;
    // 走 UDP（udp=1），dns_set_ip("8.8.8.8") 已在 main.c 配置；example.com 是 IANA 保留稳定域名
    dns_ip *ips = dns_lookup(task, "example.com", 0, 1, &cnt);
    if (NULL == ips || 0 == cnt) {
        LOG_ERROR("dns_lookup(example.com): no IPs returned.");
        if (NULL != ips) {
            FREE(ips);
        }
        return ERR_FAILED;
    }
    FREE(ips);
    return ERR_OK;
}

// 向 rpc task 发未知 rtype 请求；rpc 的 default 分支不回包，触发请求超时返回 ERR_FAILED
static int32_t _test_request_timeout(task_ctx *task, const char *rpcname) {
    task_ctx *dst = task_grab(task->loader, task_find_name(task->loader, rpcname));
    if (NULL == dst) {
        LOG_ERROR("coro_request timeout: rpc task %s not found.", rpcname);
        return ERR_FAILED;
    }
    // 默认 timeout_request=3000ms，缩短到 300ms 让用例快速结束
    uint32_t old_to = task_get_request_timeout(task);
    task_set_request_timeout(task, 300);
    int32_t erro = ERR_OK;
    size_t lens = 0;
    (void)coro_request(dst, task, 99, "x", 1, 1, &erro, &lens);
    task_set_request_timeout(task, old_to);
    task_ungrab(dst);
    if (ERR_FAILED != erro) {
        LOG_ERROR("coro_request timeout: expected ERR_FAILED, got erro=%d.", erro);
        return ERR_FAILED;
    }
    return ERR_OK;
}

// 被 CLOSE 广播唤醒后，就在排空的那一轮里重新在同一 sess 上注册
static void _reregister_waiter(task_ctx *task, void *arg) {
    reregister_arg *a = (reregister_arg *)arg;
    // 3000ms 只是失败时的兜底上界，正常路径是被 ev_close 的 CLOSE 广播唤醒
    a->first = _coro_wait(task, a->skid, MSG_TYPE_RECV, 3000)->mtype;
    a->second = _coro_wait(task, a->skid, MSG_TYPE_RECV, 200)->mtype;
}
// _coro_handle_closed 先清 keep、把 waiters 整批挪到局部表再逐个 resume；被唤醒的协程若
// 就在这一轮里重新注册同一个 sess，循环后那句"空了就删"必须看见它。删错了它永远醒不过来——
// 连超时监视器都找不到它（mapco 条目已经没了），表现为 second 一直是 0
static int32_t _test_close_reregister(task_ctx *task, uint16_t httpport) {
    SOCKET fd;
    uint64_t skid;
    if (ERR_OK != coro_connect(task, PACK_HTTP, NULL, "127.0.0.1", httpport, 0, NULL, &fd, &skid)) {
        LOG_ERROR("close-reregister: connect to http_sv failed.");
        return ERR_FAILED;
    }
    // fork 出去的等待者写的是这里的存储，而下面的失败路径会在它还挂着时就 return。
    // 放 static 免掉"每条失败路径都得先把等待者等干净"的时序推理（task 内单线程、用例顺序跑）
    static reregister_arg a;
    a.skid = skid;
    a.first = (msg_type)0;
    a.second = (msg_type)0;
    coro_fork(task, _reregister_waiter, &a);
    // fork 的协程在本条消息 dispatch 末尾才起，先让出一次给它挂上等待
    coro_sleep(task, 30);
    ev_close(&task->loader->netev, fd, skid);
    coro_sleep(task, 200 + TIMEOUT_SETTLE_MS);
    if (MSG_TYPE_CLOSE != a.first) {
        LOG_ERROR("close-reregister: first wait expected CLOSE, got %d.", (int32_t)a.first);
        return ERR_FAILED;
    }
    if (MSG_TYPE_TIMEOUT != a.second) {
        LOG_ERROR("close-reregister: re-registered waiter never woke (mtype %d), mapco entry dropped?",
                  (int32_t)a.second);
        return ERR_FAILED;
    }
    return ERR_OK;
}

static void _sendto_one(task_ctx *task, void *arg) {
    sendto_arg *a = (sendto_arg *)arg;
    char buf[4];
    SNPRINTF(buf, sizeof(buf), "p%d", a->idx);
    size_t rlens = 0;
    void *resp = coro_sendto(task, a->fd, a->skid, "127.0.0.1", a->port, buf, 2, &rlens, 1);
    if (NULL == resp || 2 != rlens) {
        return;
    }
    // 收到的未必是自己那份：FIFO 是"按到达顺序配对给调用序列"，不是"谁发的谁收"，
    // 所以只回认内容属于哪一份、不断言等于自己发的
    int32_t k = ((char *)resp)[1] - '0';
    if (k >= 1 && k <= CONCURRENT_N) {
        a->seen[k - 1] = 1;
    }
    ++(*a->nok);
}
// 同一个 skid 上并发 coro_sendto：N 个协程都挂在 sess=skid 等 RECVFROM，每条数据报唤醒队头。
// 取队头那步若越过队头找、或摘空后把条目删早了，后面的协程就再也醒不过来——表现为 nok < N
static int32_t _test_concurrent_sendto(task_ctx *task, uint16_t udpport) {
    SOCKET fd;
    uint64_t skid;
    if (ERR_OK != task_udp(task, PACK_NONE, "0.0.0.0", 0, &fd, &skid)) {
        LOG_ERROR("concurrent sendto: task_udp failed.");
        return ERR_FAILED;
    }
    coro_sync(task, fd, skid);
    int32_t nok = 0;
    int32_t seen[CONCURRENT_N] = { 0 };
    sendto_arg args[CONCURRENT_N];
    fork_serial_cb funcs[CONCURRENT_N];
    void *argp[CONCURRENT_N];
    int32_t i;
    for (i = 0; i < CONCURRENT_N; i++) {
        args[i].fd = fd;
        args[i].skid = skid;
        args[i].port = udpport;
        args[i].idx = i + 1;
        args[i].nok = &nok;
        args[i].seen = seen;
        funcs[i] = _sendto_one;
        argp[i] = &args[i];
    }
    (void)coro_fork_wait(task, CONCURRENT_N, funcs, argp);
    ev_close(&task->loader->netev, fd, skid);
    if (CONCURRENT_N != nok) {
        LOG_ERROR("concurrent sendto: only %d/%d coroutines got a response.", nok, CONCURRENT_N);
        return ERR_FAILED;
    }
    for (i = 0; i < CONCURRENT_N; i++) {
        if (0 == seen[i]) {
            LOG_ERROR("concurrent sendto: payload p%d never came back.", i + 1);
            return ERR_FAILED;
        }
    }
    return ERR_OK;
}

// 从 coro_dump 的汇总行里抠出 coro_sess 条目数；取不到返回 -1
static int32_t _sessions(task_ctx *task) {
    size_t lens = 0;
    char *dump = coro_dump(task, &lens);
    if (NULL == dump) {
        return -1;
    }
    int32_t n = -1;
    char *p = strstr(dump, " sessions");
    if (NULL != p) {
        // 从 " sessions" 往前退到数字串的开头
        char *s = p;
        while (s > dump && s[-1] >= '0' && s[-1] <= '9') {
            --s;
        }
        if (s < p) {
            n = atoi(s);
        }
    }
    FREE(dump);
    return n;
}

static void _fifo_waiter(task_ctx *task, void *arg) {
    fifo_arg *a = (fifo_arg *)arg;
    (void)_coro_wait(task, a->sess, MSG_TYPE_RESPONSE, 2000);
    a->order[(*a->n)++] = a->idx;
}
// 等待者严格 FIFO：合成 sess + 自发两条 RESPONSE。fork 按入队顺序起协程、两条响应也按序投递，
// 所以顺序是确定的（不像 UDP 那样受到达顺序影响）
static int32_t _test_fifo_order(task_ctx *task) {
    uint64_t sess = createid();
    static int32_t n;// 三个都被 fork 出去的协程写，存储期理由同 _test_close_reregister
    static int32_t order[2];
    static fifo_arg a[2];
    n = 0;
    order[0] = order[1] = 0;
    int32_t i;
    for (i = 0; i < 2; i++) {
        a[i].sess = sess;
        a[i].idx = i + 1;
        a[i].n = &n;
        a[i].order = order;
        coro_fork(task, _fifo_waiter, &a[i]);
    }
    coro_sleep(task, 30);
    task_response(task, 0, sess, ERR_OK, "r1", 2, 1);
    task_response(task, 0, sess, ERR_OK, "r2", 2, 1);
    coro_sleep(task, 150);
    if (2 != n || 1 != order[0] || 2 != order[1]) {
        LOG_ERROR("fifo order: expected 1,2 got %d,%d (n=%d).", order[0], order[1], n);
        return ERR_FAILED;
    }
    return ERR_OK;
}

static void _one_waiter(task_ctx *task, void *arg) {
    waiter_arg *a = (waiter_arg *)arg;
    a->woke = _coro_wait(task, a->sess, a->mtype, a->ms)->mtype;
}
// 队头 mtype 不匹配即视为无等待者，不越过队头去找：队头等 RECV，来的是同 sess 的 RESPONSE，
// 那条响应应落到"没人等"的兜底路径，队头继续等到自己超时
static int32_t _test_head_mtype_gate(task_ctx *task) {
    waiter_arg a;
    a.sess = createid();
    a.mtype = MSG_TYPE_RECV;
    a.ms = 300;
    a.woke = (msg_type)0;
    coro_fork(task, _one_waiter, &a);
    coro_sleep(task, 30);
    task_response(task, 0, a.sess, ERR_OK, "x", 1, 1);
    coro_sleep(task, 300 + TIMEOUT_SETTLE_MS);
    if (MSG_TYPE_TIMEOUT != a.woke) {
        LOG_ERROR("head mtype gate: RECV head woke with mtype %d, expected TIMEOUT.", (int32_t)a.woke);
        return ERR_FAILED;
    }
    return ERR_OK;
}

// RECV 属于 _message_may_keep 为真的六个 mtype，摘空 waiters 后条目应当留着；
// 直到 CLOSE 把 keep 清 false，它才真正可删
static int32_t _test_keep_lifetime(task_ctx *task, uint16_t httpport) {
    SOCKET fd;
    uint64_t skid;
    int32_t s0 = _sessions(task);// 基线要在 connect 之前取，理由见下
    if (ERR_OK != coro_connect(task, PACK_HTTP, NULL, "127.0.0.1", httpport, 0, NULL, &fd, &skid)) {
        LOG_ERROR("keep lifetime: connect to http_sv failed.");
        return ERR_FAILED;
    }
    // coro_connect 自己就在 skid 上等过 CONNECT，而 CONNECT 也在 _message_may_keep 那六个里，
    // 所以走到这里条目已经建好、keep 已经是 true
    if (s0 + 1 != _sessions(task)) {
        LOG_ERROR("keep lifetime: connect left no entry (s0=%d now=%d).", s0, _sessions(task));
        ev_close(&task->loader->netev, fd, skid);
        return ERR_FAILED;
    }
    static waiter_arg a;// 存储期理由同 _test_close_reregister
    a.sess = skid;
    a.mtype = MSG_TYPE_RECV;
    a.ms = 3000;
    a.woke = (msg_type)0;
    coro_fork(task, _one_waiter, &a);
    coro_sleep(task, 30);
    // 追加到已有条目，不新建第二个
    if (s0 + 1 != _sessions(task)) {
        LOG_ERROR("keep lifetime: waiter did not append to the existing entry.");
        ev_close(&task->loader->netev, fd, skid);
        return ERR_FAILED;
    }
    // 发一条 HTTP 请求让服务端回包，走正常摘空而不是超时
    const char *req = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    ev_send(&task->loader->netev, fd, skid, (void *)req, strlen(req), 1);
    coro_sleep(task, 300);
    if (MSG_TYPE_RECV != a.woke) {
        LOG_ERROR("keep lifetime: waiter woke with mtype %d, expected RECV.", (int32_t)a.woke);
        ev_close(&task->loader->netev, fd, skid);
        return ERR_FAILED;
    }
    if (s0 + 1 != _sessions(task)) {
        LOG_ERROR("keep lifetime: keep=true entry dropped after normal drain.");
        ev_close(&task->loader->netev, fd, skid);
        return ERR_FAILED;
    }
    ev_close(&task->loader->netev, fd, skid);
    coro_sleep(task, 300);
    if (s0 != _sessions(task)) {
        LOG_ERROR("keep lifetime: entry survived CLOSE (s0=%d now=%d).", s0, _sessions(task));
        return ERR_FAILED;
    }
    return ERR_OK;
}

// 超时路径无视 keep：同样是 keep=true 的 RECV 条目，上面那条扛过了"摘空"，走超时就该直接删。
// 留着的话该 skid 的 CLOSE 已被消费过，条目再没有任何路径能删掉
static int32_t _test_timeout_ignores_keep(task_ctx *task, uint16_t httpport) {
    SOCKET fd;
    uint64_t skid;
    int32_t s0 = _sessions(task);// 同 _test_keep_lifetime：基线取在 connect 之前
    if (ERR_OK != coro_connect(task, PACK_HTTP, NULL, "127.0.0.1", httpport, 0, NULL, &fd, &skid)) {
        LOG_ERROR("timeout-ignores-keep: connect to http_sv failed.");
        return ERR_FAILED;
    }
    if (s0 + 1 != _sessions(task)) {
        LOG_ERROR("timeout-ignores-keep: connect left no entry.");
        ev_close(&task->loader->netev, fd, skid);
        return ERR_FAILED;
    }
    waiter_arg a;
    a.sess = skid;
    a.mtype = MSG_TYPE_RECV;
    a.ms = 200;
    a.woke = (msg_type)0;
    coro_fork(task, _one_waiter, &a);
    coro_sleep(task, 200 + TIMEOUT_SETTLE_MS);
    int32_t rtn = ERR_OK;
    if (MSG_TYPE_TIMEOUT != a.woke) {
        LOG_ERROR("timeout-ignores-keep: waiter woke with mtype %d, expected TIMEOUT.", (int32_t)a.woke);
        rtn = ERR_FAILED;
    } else if (s0 != _sessions(task)) {
        LOG_ERROR("timeout-ignores-keep: keep=true entry survived the timeout path.");
        rtn = ERR_FAILED;
    }
    ev_close(&task->loader->netev, fd, skid);
    return rtn;
}

static void _startup(task_ctx *task) {
    task_coro_extra_args *arg = (task_coro_extra_args *)coro_get_arg(task);
    if (ERR_OK != _test_sleep_cascade(task)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_connect_refused(task)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_send_after_close(task, arg->httpport)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_request_timeout(task, arg->rpcname)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_close_reregister(task, arg->httpport)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_concurrent_sendto(task, arg->udpport)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_fifo_order(task)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_head_mtype_gate(task)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_keep_lifetime(task, arg->httpport)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != _test_timeout_ignores_keep(task, arg->httpport)) {
        return;
    }
    if (task_isclosing(task)) {
        return;
    }
    // DNS 这条必须排在最后：它是本文件唯一需要真出网（UDP:53 打 8.8.8.8）的用例，
    // 而 _startup 是失败即 return 的串行链。原来它排第 4 个，断网/防火墙拦 DNS 时
    // 后面 7 个用例——全仓唯一覆盖 coro_sess 那五条不变式的地方——一个都不跑，
    // 现象与"不变式真被改坏"完全一样（都只有一行 coro_extra: x）
    if (ERR_OK != _test_dns_lookup(task)) {
        return;
    }
    *(arg->ok) = 1;
    LOG_INFO("coro_extra tested.");
}

void task_coro_extra_start(loader_ctx *loader, const char *name, uint16_t httpport, uint16_t udpport,
                           const char *rpcname, int32_t *ok) {
    if (NULL == ok || 0 == httpport || 0 == udpport) {
        return;
    }
    task_coro_extra_args *arg;
    CALLOC(arg, 1, sizeof(task_coro_extra_args));
    arg->httpport = httpport;
    arg->udpport = udpport;
    arg->rpcname = rpcname;
    arg->ok = ok;
    coro_task_register(loader, name, 0, _startup, NULL, _free, arg);
}
