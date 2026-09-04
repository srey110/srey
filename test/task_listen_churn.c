#include "task_listen_churn.h"

typedef struct task_listen_churn_args {
    uint16_t port;
    int32_t *ok;
}task_listen_churn_args;

// 30 轮 listen → connect → close → unlisten 循环，捕获 in-flight accept 与 unlisten 的并发时序
#define CHURN_ITERS 30
// 每轮 unlisten 之后留给端口释放的时间。ev_unlisten 只发起拆除：IOCP 下 CancelIoEx 之后还要等
// 在途 AcceptEx 的完成包回来、ref 归零才 closesocket，慢速虚拟机上远超几十毫秒，
// 短了下一轮 listen 就撞 EADDRINUSE
#define CHURN_SETTLE_MS 500

static void _startup(task_ctx *task) {
    task_listen_churn_args *arg = (task_listen_churn_args *)coro_get_arg(task);
    uint64_t lsnid;
    SOCKET cfd;
    uint64_t cskid;
    int32_t r;
    int32_t i;
    int32_t nconn = 0;
    for (i = 0; i < CHURN_ITERS; i++) {
        if (task_isclosing(task)) {
            return;
        }
        if (ERR_OK != task_listen(task, PACK_HTTP, NULL, "127.0.0.1", arg->port, &lsnid, 0)) {
            LOG_ERROR("listen_churn iter %d: task_listen %u failed.", i, arg->port);
            return;
        }
        r = coro_connect(task, PACK_HTTP, NULL, "127.0.0.1", arg->port, 0, NULL, &cfd, &cskid);
        if (ERR_OK == r) {
            nconn++;
            ev_close(&task->loader->netev, cfd, cskid);
        }
        // 立即 unlisten；accept 完成事件可能正落在 watcher 队列里，命中 _uev_qtn_freelsn 引用计数路径
        ev_unlisten(&task->loader->netev, lsnid);
        coro_sleep(task, CHURN_SETTLE_MS);
    }
    // 连不上不当失败：task_listen 是异步落地的，本轮 connect 可能赶在监听真正就绪之前。
    // 但一轮都连不上就不是时序问题了 —— 那说明 listen 整个没起来，而原来这种情况照样算通过。
    // 逐轮强判会在慢机上抖，取过半
    if (nconn * 2 < CHURN_ITERS) {
        LOG_ERROR("listen_churn: only %d/%d iters connected.", nconn, CHURN_ITERS);
        return;
    }
    *(arg->ok) = 1;
    LOG_INFO("listen_churn tested (%d iters, %d connected).", CHURN_ITERS, nconn);
}

void task_listen_churn_start(loader_ctx *loader, const char *name, uint16_t port, int32_t *ok) {
    if (NULL == ok || 0 == port) {
        return;
    }
    task_listen_churn_args *arg;
    CALLOC(arg, 1, sizeof(task_listen_churn_args));
    arg->port = port;
    arg->ok = ok;
    coro_task_register(loader, name, 0, _startup, NULL, _free, arg);
}
