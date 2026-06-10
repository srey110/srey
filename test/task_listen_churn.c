#include "task_listen_churn.h"

typedef struct task_listen_churn_args {
    uint16_t port;
    int32_t *ok;
}task_listen_churn_args;

// 30 轮 listen → connect → close → unlisten 循环，捕获 in-flight accept 与 unlisten 的并发时序
#define CHURN_ITERS 30

static void _startup(task_ctx *task) {
    task_listen_churn_args *arg = (task_listen_churn_args *)coro_get_arg(task);
    uint64_t lsnid;
    SOCKET cfd;
    uint64_t cskid;
    int32_t r;
    int32_t i;
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
            ev_close(&task->loader->netev, cfd, cskid, 1);
        }
        // 立即 unlisten；accept 完成事件可能正落在 watcher 队列里，命中 _uev_qtn_freelsn 引用计数路径
        ev_unlisten(&task->loader->netev, lsnid);
        // 短 sleep 让 watcher 处理 cmd 队列；不能太长否则用例拖时
        coro_sleep(task, 10);
    }
    *(arg->ok) = 1;
    LOG_INFO("listen_churn tested (%d iters).", CHURN_ITERS);
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
