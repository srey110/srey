#include "task_multicast.h"

#define N_CLIENTS 5
#define MSG_BROADCAST "BROADCAST_HELLO"
#define MSG_LEN 15

typedef struct task_multicast_args {
    int32_t *ok;
    uint16_t port;
}task_multicast_args;

// 测试共享状态：跨多个回调累计计数与 fd 收集
static SOCKET _server_fds[N_CLIENTS];
static uint64_t _server_skids[N_CLIENTS];
static atomic_t _accepted_count;
static atomic_t _received_count;
static atomic_t _broadcast_sent;

// accept 端回调：累积 server-side fd 到数组,等满 N 个由主协程统一广播。
// 注意 ATOMIC_ADD 是 fetch_and_add 返回旧值,首次返回 0 即为本次写入的 idx
static void _net_accept(task_ctx *task, SOCKET fd, uint64_t skid, subtype_t pktype) {
    (void)task; (void)pktype;
    int32_t idx = (int32_t)ATOMIC_ADD(&_accepted_count, 1);
    if (idx < N_CLIENTS) {
        _server_fds[idx] = fd;
        _server_skids[idx] = skid;
    }
}

// 任意 fd 收到数据回调：client=1 时是 outgoing 连接收到 server 广播,验证内容并计数
static void _net_recv(task_ctx *task, SOCKET fd, uint64_t skid, subtype_t pktype, uint8_t client,
                      uint8_t slice, void *data, size_t size) {
    (void)task; (void)fd; (void)skid; (void)pktype; (void)slice;
    // client 字段含 STATUS_CLIENT (0x08) 标志位,非 0 即 outgoing 连接(收到 server 广播)
    if (0 != client && MSG_LEN == size && 0 == memcmp(data, MSG_BROADCAST, MSG_LEN)) {
        ATOMIC_ADD(&_received_count, 1);
    }
}

static void _client_worker(task_ctx *task, void *arg) {
    uint16_t port = *(uint16_t *)arg;
    SOCKET fd;
    uint64_t skid;
    if (ERR_OK != coro_connect(task, PACK_NONE, NULL, "127.0.0.1", port,
                               NETEV_NONE, NULL, &fd, &skid)) {
        LOG_ERROR("multicast: client coro_connect failed.");
        return;
    }
    // 不主动 recv,纯等 server 广播触发 _net_recv 回调累计 received_count;
    // 给足 3s 兜底,主协程 polling 收齐就提前结束
    coro_sleep(task, 3000);
}

static void _startup(task_ctx *task) {
    task_multicast_args *arg = (task_multicast_args *)coro_get_arg(task);
    ATOMIC_SET(&_accepted_count, 0);
    ATOMIC_SET(&_received_count, 0);
    ATOMIC_SET(&_broadcast_sent, 0);

    task_accepted(task, _net_accept);
    task_recved(task, _net_recv);
    uint64_t lid;
    if (ERR_OK != task_listen(task, PACK_NONE, NULL, "0.0.0.0", arg->port, &lid, NETEV_ACCEPT)) {
        LOG_ERROR("multicast: task_listen %d error.", arg->port);
        return;
    }
    // 起 N 个 client worker (fork-and-forget,他们在自己协程内 coro_connect)
    int32_t i;
    for (i = 0; i < N_CLIENTS; i++) {
        coro_fork(task, _client_worker, &arg->port);
    }
    // 等所有 client connect + accept 完成
    coro_sleep(task, 200);
    if (N_CLIENTS != ATOMIC_GET(&_accepted_count)) {
        LOG_ERROR("multicast: accepted %d/%d only.",
                  (int32_t)ATOMIC_GET(&_accepted_count), N_CLIENTS);
        return;
    }
    // 调 ev_send_multi 一次广播给所有 server-side fd;copy=1 内部 memcpy,业务无需保留 payload
    if (ERR_OK != ev_send_multi(&task->loader->netev, _server_fds, _server_skids, N_CLIENTS,
                                MSG_BROADCAST, MSG_LEN, 1)) {
        LOG_ERROR("multicast: ev_send_multi failed.");
        return;
    }
    ATOMIC_SET(&_broadcast_sent, 1);
    // 等所有 client 收到：每 50ms 检查一次,最多等 2s
    int32_t recvcnt = 0;
    int32_t poll;
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
    *(arg->ok) = 1;
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
