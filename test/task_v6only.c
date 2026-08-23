#include "task_v6only.h"

typedef struct task_v6only_args {
    uint16_t port;
    int32_t *ok;
}task_v6only_args;

static void _startup(task_ctx *task) {
    task_v6only_args *arg = (task_v6only_args *)coro_get_arg(task);
    uint64_t lsnid;
    SOCKET cfd;
    uint64_t cskid;
    if (task_isclosing(task)) {
        return;
    }
    if (ERR_OK != task_listen(task, PACK_NONE, NULL, "::", arg->port, &lsnid, 0)) {
        LOG_INFO("v6only: listen :: %u failed, no IPv6 on this host, skip.", arg->port);
        *(arg->ok) = 1;// 环境缺失记通过:只有"IPv4 连进 :: 监听"才是本用例要抓的回归
        return;
    }
    // 先确认 IPv6 回环本身是通的，否则下面 IPv4 连不上就分不清是 v6only 生效还是监听坏了
    if (ERR_OK != coro_connect(task, PACK_NONE, NULL, "::1", arg->port, 0, NULL, &cfd, &cskid)) {
        LOG_INFO("v6only: connect ::1 failed, no IPv6 loopback, skip.");
        ev_unlisten(&task->loader->netev, lsnid);
        *(arg->ok) = 1;
        return;
    }
    ev_close(&task->loader->netev, cfd, cskid);
    // 正题：v6only=1 之下 "::" 没有占住 IPv4 通配地址，这个端口的 IPv4 侧应当无人监听
    if (ERR_OK == coro_connect(task, PACK_NONE, NULL, "127.0.0.1", arg->port, 0, NULL, &cfd, &cskid)) {
        ev_close(&task->loader->netev, cfd, cskid);
        ev_unlisten(&task->loader->netev, lsnid);
        LOG_ERROR("v6only: 127.0.0.1 reached a :: listener, IPV6_V6ONLY not in effect.");
        return;
    }
    ev_unlisten(&task->loader->netev, lsnid);
    *(arg->ok) = 1;
    LOG_INFO("v6only tested.");
}

void task_v6only_start(loader_ctx *loader, const char *name, uint16_t port, int32_t *ok) {
    if (NULL == ok || 0 == port) {
        return;
    }
    task_v6only_args *arg;
    CALLOC(arg, 1, sizeof(task_v6only_args));
    arg->port = port;
    arg->ok = ok;
    coro_task_register(loader, name, 0, _startup, NULL, _free, arg);
}
