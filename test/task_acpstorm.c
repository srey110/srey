#include "task_acpstorm.h"
#include "utils/netaddr.h"

typedef struct task_acpstorm_args {
    uint16_t port;
    int32_t *ok;
}task_acpstorm_args;

// 轮数与 listen_churn 对齐，便于两边对照
#define STORM_ITERS 30
// 每轮灌多少条不等完成的 connect。太少撞不上在途 AcceptEx，太多在慢速虚拟机上拖垮整轮
#define STORM_CONNS 16
// 每个 ev_ctx 的网络线程数。多于 1 才有 accept 分发跨 watcher 的路径
#define STORM_NTH 2
// listen 落地是异步的，灌连接前先让它就绪；短了整轮都连不上，测不到东西
#define STORM_READY_MS 30

// 三个回调都留空：本用例只关心拆除时序，不关心数据。r_cb 非空是 ev_listen 的硬要求
static int32_t _on_accept(ev_ctx *ev, sock_ctx *sk, ud_cxt *ud) {
    (void)ev;
    (void)sk;
    (void)ud;
    return ERR_OK;
}
static void _on_recv(ev_ctx *ev, sock_ctx *sk,
                     int32_t client, buffer_ctx *buf, size_t size, ud_cxt *ud) {
    (void)ev;
    (void)sk;
    (void)client;
    (void)buf;
    (void)size;
    (void)ud;
}
// 发起一批不等完成的连接：拿到 EINPROGRESS/WSAEWOULDBLOCK 就走，
// 让 SYN 在路上，服务端那边的 AcceptEx 正好处于在途
static int32_t _storm_fire(SOCKET *fds, uint16_t port) {
    netaddr_ctx addr;
    if (ERR_OK != netaddr_set(&addr, "127.0.0.1", port)) {
        return ERR_FAILED;
    }
    int32_t n = 0;
    int32_t i;
    for (i = 0; i < STORM_CONNS; i++) {
        fds[i] = sock_create_cloexec(netaddr_family(&addr), SOCK_STREAM, 0, 1);
        if (INVALID_SOCK == fds[i]) {
            continue;
        }
        // 非阻塞 connect 恒立即返回，成功与"在途"都算数，不判返回值
        (void)connect(fds[i], netaddr_addr(&addr), netaddr_size(&addr));
        n++;
    }
    return n;
}
static void _storm_close(SOCKET *fds) {
    int32_t i;
    for (i = 0; i < STORM_CONNS; i++) {
        if (INVALID_SOCK != fds[i]) {
            CLOSE_SOCK(fds[i]);
            fds[i] = INVALID_SOCK;
        }
    }
}
static void _startup(task_ctx *task) {
    task_acpstorm_args *arg = (task_acpstorm_args *)coro_get_arg(task);
    SOCKET fds[STORM_CONNS];
    ev_ctx ev;
    cbs_ctx cbs;
    uint64_t lsnid;
    uint16_t port;
    int32_t i;
    int32_t fired = 0;
    ZERO(&cbs, sizeof(cbs));
    cbs.acp_cb = _on_accept;
    cbs.r_cb = _on_recv;
    for (i = 0; i < STORM_ITERS; i++) {
        if (task_isclosing(task)) {
            return;
        }
        for (int32_t j = 0; j < STORM_CONNS; j++) {
            fds[j] = INVALID_SOCK;
        }
        // 每轮换端口：Windows 监听口带 SO_EXCLUSIVEADDRUSE，立即重绑同端口必撞 EADDRINUSE
        port = (uint16_t)(arg->port + i);
        ev_init(&ev, STORM_NTH, NULL);
        if (ERR_OK != ev_listen(&ev, NULL, "127.0.0.1", port, &cbs, NULL, &lsnid)) {
            LOG_ERROR("acpstorm iter %d: ev_listen %u failed.", i, (uint32_t)port);
            ev_free(&ev);
            return;
        }
        coro_sleep(task, STORM_READY_MS);
        fired += _storm_fire(fds, port);
        // 不给 accept 留任何时间，就地拆除 —— 这一行就是本用例要压的窗口
        ev_free(&ev);
        _storm_close(fds);
    }
    // 发起数是本用例唯一的健康判据：ev_free 是否干净由 PageHeap / ASan / memcheck 去看，
    // 这里只保证确实灌进去过东西，而不是一轮空转还报绿
    if (fired < STORM_ITERS) {
        LOG_ERROR("acpstorm: only %d conns fired in %d iters.", fired, STORM_ITERS);
        return;
    }
    *(arg->ok) = 1;
    LOG_INFO("acpstorm tested (%d iters x %d conns, %d fired).",
             STORM_ITERS, STORM_CONNS, fired);
}
void task_acpstorm_start(loader_ctx *loader, const char *name, uint16_t port, int32_t *ok) {
    if (NULL == ok || 0 == port) {
        return;
    }
    task_acpstorm_args *arg;
    CALLOC(arg, 1, sizeof(task_acpstorm_args));
    arg->port = port;
    arg->ok = ok;
    coro_task_register(loader, name, 0, _startup, NULL, _free, arg);
}
