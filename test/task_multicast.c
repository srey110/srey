#include "task_multicast.h"

#define N_CLIENTS 5
#define MSG_BROADCAST "BROADCAST_HELLO"
#define MSG_LEN 15

typedef struct task_multicast_args {
    int32_t *ok;
    uint16_t port;
}task_multicast_args;

// 测试共享状态：跨多个回调累计计数与连接收集
static sock_ctx _server_sks[N_CLIENTS];
static uint16_t _peer_ports[N_CLIENTS];// accept 时拿到的对端端口
static uint16_t _client_ports[N_CLIENTS];// client 连上后自己的本地端口，与上面逐个对得上才算地址给对了
static atomic_t _accepted_count;
static atomic_t _connected_count;
static atomic_t _peer_bad;// 对端地址缺失或不是 127.0.0.1 的次数
static atomic_t _received_count;

// accept 端回调：累积 server-side 连接到数组,等满 N 个由主协程统一广播。
// 注意 ATOMIC_ADD 是 fetch_and_add 返回旧值,首次返回 0 即为本次写入的 idx
static void _net_accept(task_ctx *task, sock_ctx *sk, subtype_t pktype, const netaddr_ctx *peer) {
    (void)task; (void)pktype;
    int32_t idx = (int32_t)ATOMIC_ADD(&_accepted_count, 1);
    if (idx >= N_CLIENTS) {
        return;
    }
    _server_sks[idx] = *sk;
    // listen 带了 NETEV_PEERADDR：地址必须在，且就是 client 那头
    char ip[IP_LENS];
    if (NULL == peer
        || ERR_OK != netaddr_ip((netaddr_ctx *)peer, ip)
        || 0 != strcmp(ip, "127.0.0.1")) {
        ATOMIC_ADD(&_peer_bad, 1);
        return;
    }
    _peer_ports[idx] = netaddr_port((netaddr_ctx *)peer);
}

// 任意 fd 收到数据回调：client=1 时是 outgoing 连接收到 server 广播,验证内容并计数
static void _net_recv(task_ctx *task, sock_ctx *sk, subtype_t pktype, uint8_t client,
                      uint8_t slice, void *data, size_t size) {
    (void)task; (void)sk; (void)pktype; (void)slice;
    // client 非 0 即 outgoing 连接(收到 server 广播)
    if (0 != client && MSG_LEN == size && 0 == memcmp(data, MSG_BROADCAST, MSG_LEN)) {
        ATOMIC_ADD(&_received_count, 1);
    }
}

static void _client_worker(void *owner, void *arg) {
    task_ctx *task = owner;
    uint16_t port = *(uint16_t *)arg;
    sock_ctx sk;
    if (ERR_OK != coro_connect(task, PACK_NONE, NULL, "127.0.0.1", port,
                               NETEV_NONE, NULL, &sk)) {
        LOG_ERROR("multicast: client coro_connect failed.");
        return;
    }
    netaddr_ctx local;
    int32_t idx = (int32_t)ATOMIC_ADD(&_connected_count, 1);
    if (idx < N_CLIENTS
        && ERR_OK == netaddr_local(&local, sk.fd)) {
        _client_ports[idx] = netaddr_port(&local);
    }
    // 不主动 recv,纯等 server 广播触发 _net_recv 回调累计 received_count;
    // 给足 3s 兜底,主协程 polling 收齐就提前结束
    coro_sleep(task, 3000);
    ev_close(&task->loader->netev, &sk);
}

static void _startup(task_ctx *task) {
    task_multicast_args *arg = (task_multicast_args *)coro_get_arg(task);
    ATOMIC_SET(&_accepted_count, 0);
    ATOMIC_SET(&_connected_count, 0);
    ATOMIC_SET(&_received_count, 0);
    ATOMIC_SET(&_peer_bad, 0);

    task_accepted(task, _net_accept);
    task_recved(task, _net_recv);
    uint64_t lid;
    if (ERR_OK != task_listen(task, PACK_NONE, NULL, "0.0.0.0", arg->port, &lid, NETEV_ACCEPT | NETEV_PEERADDR)) {
        LOG_ERROR("multicast: task_listen %d error.", arg->port);
        return;
    }
    // 起 N 个 client worker (fork-and-forget,他们在自己协程内 coro_connect)
    int32_t i;
    for (i = 0; i < N_CLIENTS; i++) {
        coro_fork(coro_task_co(task), _client_worker, &arg->port);
    }
    // 等所有 client connect + accept 完成：每 50ms 查一次,最多 1s。
    // 上限不能再大——client 侧只挂 3s，后面收包还要留 2s
    int32_t acpcnt = 0;
    int32_t poll;
    for (poll = 0; poll < 20; poll++) {
        coro_sleep(task, 50);
        acpcnt = (int32_t)ATOMIC_GET(&_accepted_count);
        if (N_CLIENTS == acpcnt
            && N_CLIENTS == (int32_t)ATOMIC_GET(&_connected_count)) {
            break;
        }
    }
    if (N_CLIENTS != acpcnt) {
        LOG_ERROR("multicast: accepted %d/%d only.", acpcnt, N_CLIENTS);
        return;
    }
    if (0 != ATOMIC_GET(&_peer_bad)) {
        LOG_ERROR("multicast: peer address missing or not 127.0.0.1.");
        return;
    }
    // 每个 accept 拿到的对端端口都得是某个 client 的本地端口
    int32_t k;
    for (i = 0; i < N_CLIENTS; i++) {
        k = 0;
        while (k < N_CLIENTS && _client_ports[k] != _peer_ports[i]) {
            k++;
        }
        if (N_CLIENTS == k) {
            LOG_ERROR("multicast: peer port %d matches no client.", (int32_t)_peer_ports[i]);
            return;
        }
    }
    // 调 ev_send_multi 一次广播给所有 server-side 连接;copy=1 内部 memcpy,业务无需保留 payload
    if (ERR_OK != ev_send_multi(&task->loader->netev, _server_sks, N_CLIENTS,
                                MSG_BROADCAST, MSG_LEN, 1)) {
        LOG_ERROR("multicast: ev_send_multi failed.");
        return;
    }
    // 等所有 client 收到：每 50ms 检查一次,最多等 2s
    int32_t recvcnt = 0;
    for (poll = 0; poll < 40; poll++) {
        coro_sleep(task, 50);
        recvcnt = (int32_t)ATOMIC_GET(&_received_count);
        if (N_CLIENTS == recvcnt) {
            break;
        }
    }
    if (N_CLIENTS != recvcnt) {
        LOG_ERROR("multicast: received %d/%d.", recvcnt, N_CLIENTS);
        return;
    }
    ATOMIC_SET((atomic_t *)arg->ok, 1);
    LOG_INFO("multicast tested: %d/%d clients received broadcast.", recvcnt, N_CLIENTS);
}

void task_multicast_start(loader_ctx *loader, const char *name, uint16_t port, int32_t *ok) {
    if (NULL == ok) {
        return;
    }
    task_multicast_args *arg;
    CALLOC(arg, 1, sizeof(task_multicast_args));
    arg->ok = ok;
    arg->port = port;
    coro_task_register(loader, name, 0, _startup, NULL, _free, arg);
}
