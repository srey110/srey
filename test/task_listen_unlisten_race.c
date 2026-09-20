#include "task_listen_unlisten_race.h"

typedef struct task_unlisten_race_args {
    uint16_t port;
    int32_t *ok;
}task_unlisten_race_args;

// SO_REUSEPORT + 多 watcher 下 ev_unlisten 与 in-flight accept 的并发回归：
// 每轮 task_listen 后 coro_fork N 个客户端并发 connect，连接尚在飞行就 ev_unlisten，
// 触发 lib/event 跨 watcher 的 lsn 引用计数 + qtn 隔离队列延后释放路径
#define RACE_ITERS    30
#define RACE_CLIENTS  8
// 每轮 unlisten 之后留给端口释放的时间，理由同 task_listen_churn.c 的 CHURN_SETTLE_MS。
// 注意与下面那个 2ms 不是一回事：那个是故意只等一部分 connect 落进 accept，制造 in-flight 窗口
#define RACE_SETTLE_MS 500

// 累计连上的次数：unlisten 抢在前面时连不上是正常的，但一次都没连上就说明
// listen 整个没起来，而那种情况原来照样算通过（口径同 task_listen_churn）
static atomic_t _race_nconn;

// fork 出的客户端工作协程：连一次后立即关闭，连接失败静默忽略（unlisten 已发生）
static void _client_worker(task_ctx *task, void *arg) {
    uint16_t port = (uint16_t)(uintptr_t)arg;
    sock_ctx sk;
    if (ERR_OK == coro_connect(task, PACK_HTTP, NULL, "127.0.0.1", port, 0, NULL, &sk)) {
        ATOMIC_ADD(&_race_nconn, 1);
        ev_close(&task->loader->netev, &sk);
    }
}

static void _startup(task_ctx *task) {
    task_unlisten_race_args *arg = (task_unlisten_race_args *)coro_get_arg(task);
    ATOMIC_SET(&_race_nconn, 0);
    uint64_t lsnid;
    int32_t i, c;
    for (i = 0; i < RACE_ITERS; i++) {
        if (task_isclosing(task)) {
            return;
        }
        if (ERR_OK != task_listen(task, PACK_HTTP, NULL, "127.0.0.1", arg->port, &lsnid, 0)) {
            LOG_ERROR("unlisten_race iter %d: task_listen %u failed.", i, arg->port);
            return;
        }
        // 并发投 N 个 client 协程，accept 经 SO_REUSEPORT 内核 hash 分散到各 watcher
        for (c = 0; c < RACE_CLIENTS; c++) {
            coro_fork(task, _client_worker, (void *)(uintptr_t)arg->port);
        }
        // 短 sleep 让部分 connect 进入 accept、跨 watcher 投递落地，但不等全部完成
        coro_sleep(task, 2);
        if (task_isclosing(task)) {
            return;
        }
        // in-flight accept 仍存在时 unlisten：CMD_UNLSN 走每 watcher，末尾 CMD_LSN_UNREF
        // 到 worker[0]，触发 ref 归 0 + 入 watcher->qtn 隔离队列延后释放
        ev_unlisten(&task->loader->netev, lsnid);
        if (task_isclosing(task)) {
            return;
        }
        // 给飞行中 accept / cleanup / client 协程收尾，避免下一轮 listen 撞同端口
        coro_sleep(task, RACE_SETTLE_MS);
    }
    int32_t nconn = (int32_t)ATOMIC_GET(&_race_nconn);
    if (0 == nconn) {
        LOG_ERROR("listen_unlisten_race: no client ever connected in %d iters x %d clients.",
                  RACE_ITERS, RACE_CLIENTS);
        return;
    }
    *(arg->ok) = 1;
    LOG_INFO("listen_unlisten_race tested (%d iters x %d clients, %d connected).",
             RACE_ITERS, RACE_CLIENTS, nconn);
}

void task_listen_unlisten_race_start(loader_ctx *loader, const char *name, uint16_t port, int32_t *ok) {
    if (NULL == ok || 0 == port) {
        return;
    }
    task_unlisten_race_args *arg;
    CALLOC(arg, 1, sizeof(task_unlisten_race_args));
    arg->port = port;
    arg->ok = ok;
    coro_task_register(loader, name, 0, _startup, NULL, _free, arg);
}
